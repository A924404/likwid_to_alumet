#include "PerfRecorder.hpp"

#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace {

void initializePfm() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    pfm_err_t status = pfm_initialize();
    if (status != PFM_SUCCESS) {
        throw std::runtime_error(std::string("pfm_initialize failed: ") + pfm_strerror(status));
    }
    initialized = true;
}

// Encodes `event_name` via libpfm4 and opens a disabled perf counter attached to `child`.
int openCounter(const std::string& event_name, pid_t child) {
    struct perf_event_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);

    pfm_perf_encode_arg_t arg;
    std::memset(&arg, 0, sizeof(arg));
    arg.attr = &attr;
    arg.size = sizeof(arg);

    pfm_err_t status = pfm_get_os_event_encoding(event_name.c_str(), PFM_PLM0 | PFM_PLM3, PFM_OS_PERF_EVENT, &arg);
    if (status != PFM_SUCCESS) {
        throw std::runtime_error("Could not encode event '" + event_name + "': " + pfm_strerror(status));
    }

    attr.disabled = 1;
    attr.enable_on_exec = 1;
    // Guarantee a dedicated PMC for the whole run; without this the event can be silently
    // scheduled out (reading back 0) whenever other counters contend for the same PMU slot.
    attr.pinned = 1;
    // Needed to detect a counter that never got a hardware slot at all (e.g. all PMCs taken
    // by the NMI watchdog), which otherwise reads back as an indistinguishable 0.
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    int fd = perf_event_open(&attr, child, -1, -1, 0);
    if (fd < 0) {
        throw std::runtime_error("perf_event_open failed for '" + event_name + "': " + std::strerror(errno));
    }
    return fd;
}

}  // namespace

std::map<std::string, double> profile_events(const std::vector<std::string>& command,
                                              const std::vector<std::string>& event_names) {
    if (command.empty()) {
        throw std::runtime_error("No command given to profile");
    }
    initializePfm();

    pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if (child == 0) {
        // Stop here so the parent can attach perf counters before the target actually runs.
        raise(SIGSTOP);

        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& arg : command) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);  // execvp only returns on failure
    }

    int status = 0;
    if (waitpid(child, &status, WUNTRACED) < 0) {
        throw std::runtime_error(std::string("waitpid (stop) failed: ") + std::strerror(errno));
    }

    std::vector<int> fds;
    try {
        for (const auto& event_name : event_names) {
            fds.push_back(openCounter(event_name, child));
        }
    } catch (...) {
        for (int fd : fds) {
            close(fd);
        }
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        throw;
    }

    kill(child, SIGCONT);
    waitpid(child, &status, 0);

    std::map<std::string, double> results;
    for (size_t i = 0; i < fds.size(); ++i) {
        struct {
            uint64_t value;
            uint64_t time_enabled;
            uint64_t time_running;
        } counter;
        bool read_ok = read(fds[i], &counter, sizeof(counter)) == sizeof(counter);
        close(fds[i]);
        if (!read_ok) {
            throw std::runtime_error("Could not read counter for '" + event_names[i] + "'");
        }
        if (counter.time_running == 0) {
            throw std::runtime_error("Event '" + event_names[i] +
                                      "' was never scheduled onto a hardware counter (no free PMC). This is "
                                      "commonly caused by the NMI watchdog reserving a counter; try 'echo 0 > "
                                      "/proc/sys/kernel/nmi_watchdog' (as root) and re-run.");
        }
        results[event_names[i]] = static_cast<double>(counter.value);
    }

    return results;
}
