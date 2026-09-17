#include <iostream>
#include <string>
#include <vector>

#include "CpuInfo.hpp"
#include "LikwidHook.hpp"
#include "PerfRecorder.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [--metrics-file <path>] <program> [program-args...]\n";
        return 1;
    }

    std::string json_path = "likwid_metrics.json";
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.size() >= 2 && (args[0] == "--metrics-file" || args[0] == "-m")) {
        json_path = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }

    if (args.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--metrics-file <path>] <program> [program-args...]\n";
        return 1;
    }

    const std::vector<std::string>& command = args;

    try {
        CpuInfo cpu = readCpuInfo();
        std::string family_code = std::to_string(cpu.family);

        LikwidHook hook(json_path);
        std::string architecture_code = hook.resolveArchitectureCode(family_code, cpu.model, cpu.stepping);

        std::cout << "Detected CPU family " << family_code << ", model " << cpu.model << ", stepping "
                  << cpu.stepping << " -> architecture " << architecture_code << "\n";

        if (!hook.isFlops(family_code, architecture_code)) {
            std::cerr << "No FLOPS metrics are defined for architecture " << architecture_code << ".\n";
            return 1;
        }

        std::vector<std::string> events = hook.get_flops_events(family_code, architecture_code);
        std::cout << "Recording " << events.size() << " event(s) via perf_events while running the target program...\n";

        std::map<std::string, double> counter_values = profile_events(command, events);

        std::map<std::string, double> flops = hook.flops(family_code, architecture_code, counter_values);

        std::cout << "FLOPS metrics:\n";
        for (const auto& [name, value] : flops) {
            std::cout << "  " << name << " = " << value << "\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        std::cerr << "Note: this requires Linux perf_events support (perf_event_open); it is commonly"
                     " unavailable under WSL.\n";
        return 1;
    }

    return 0;
}

