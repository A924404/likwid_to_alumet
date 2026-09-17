#pragma once

#include <fstream>
#include <stdexcept>
#include <string>

// Minimal parser for the fields of /proc/cpuinfo needed to resolve a
// likwid_metrics.json architecture record (family, model, stepping).
struct CpuInfo {
    int family = -1;
    int model = -1;
    int stepping = -1;
};

inline CpuInfo readCpuInfo(const std::string& path = "/proc/cpuinfo") {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open " + path);
    }

    auto trim = [](std::string s) {
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        return start == std::string::npos ? std::string() : s.substr(start, end - start + 1);
    };

    CpuInfo info;
    std::string line;
    while (std::getline(file, line)) {
        // Only the first processor block is needed: family/model/stepping are uniform per-host.
        if (line.empty() && info.family >= 0 && info.model >= 0) {
            break;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        if (key == "cpu family") {
            info.family = std::stoi(value);
        } else if (key == "model") {
            info.model = std::stoi(value);
        } else if (key == "stepping") {
            info.stepping = std::stoi(value);
        }
    }

    if (info.family < 0 || info.model < 0) {
        throw std::runtime_error("Could not find 'cpu family'/'model' in " + path);
    }
    return info;
}
