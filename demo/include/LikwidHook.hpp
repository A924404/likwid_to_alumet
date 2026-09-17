#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Wraps a likwid_metrics.json-shaped file and evaluates its FLOPS / memory
// volume formulas for a given CPU family / architecture pair.
class LikwidHook {
public:
    // Loads and parses the JSON file located at `json_path`.
    explicit LikwidHook(const std::string& json_path);

    // Resolves the architecture_abbreviation for a family/model/stepping triplet,
    // as read from /proc/cpuinfo's "cpu family" / "model" / "stepping" fields.
    // Throws std::runtime_error if no matching architecture is found.
    std::string resolveArchitectureCode(const std::string& family_code, int model_code, int stepping) const;

    // Whether the given family/architecture defines any "flops" metrics.
    bool isFlops(const std::string& family_code, const std::string& architecture_code) const;

    // Whether the given family/architecture defines any "memory_volume" metrics.
    bool isMem(const std::string& family_code, const std::string& architecture_code) const;

    // Event names (deduplicated, first-seen order) required by the "flops" metrics.
    std::vector<std::string> get_flops_events(const std::string& family_code,
                                               const std::string& architecture_code) const;

    // Event names (deduplicated, first-seen order) required by the "memory_volume" metrics.
    std::vector<std::string> get_mem_events(const std::string& family_code,
                                             const std::string& architecture_code) const;

    // Evaluates every "flops" formula using `events` (event name -> raw counter value).
    // Returns metric name -> computed value.
    std::map<std::string, double> flops(const std::string& family_code,
                                         const std::string& architecture_code,
                                         const std::map<std::string, double>& events) const;

    // Evaluates every "memory_volume" formula using `events` (event name -> raw counter value).
    // Returns metric name -> computed value.
    std::map<std::string, double> mem_volume(const std::string& family_code,
                                              const std::string& architecture_code,
                                              const std::map<std::string, double>& events) const;

private:
    nlohmann::json arch_def;

    // Reads and parses a likwid_metrics.json-shaped file, returning its content.
    static nlohmann::json loadJsonFile(const std::string& json_path);

    // Locates the architecture record for (family_code, architecture_code), or
    // nullptr if the family or the architecture abbreviation is not present.
    const nlohmann::json* findArchitecture(const std::string& family_code,
                                            const std::string& architecture_code) const;

    // Shared implementation for get_flops_events / get_mem_events.
    std::vector<std::string> getEvents(const std::string& family_code,
                                        const std::string& architecture_code,
                                        const std::string& category) const;

    // Shared implementation for flops / mem_volume.
    std::map<std::string, double> computeMetrics(const std::string& family_code,
                                                  const std::string& architecture_code,
                                                  const std::string& category,
                                                  const std::map<std::string, double>& events) const;
};
