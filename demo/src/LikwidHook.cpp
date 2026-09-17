#include "LikwidHook.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "FormulaEvaluator.hpp"

namespace {

bool evaluateSteppingCondition(const std::string& condition, int stepping) {
    std::istringstream iss(condition);
    std::string lhs, op;
    int value = 0;
    iss >> lhs >> op >> value;
    if (lhs != "stepping") {
        throw std::runtime_error("Unsupported model condition: " + condition);
    }
    if (op == ">=") return stepping >= value;
    if (op == "<=") return stepping <= value;
    if (op == ">") return stepping > value;
    if (op == "<") return stepping < value;
    if (op == "==") return stepping == value;
    if (op == "!=") return stepping != value;
    throw std::runtime_error("Unsupported model condition operator: " + condition);
}

}  // namespace

LikwidHook::LikwidHook(const std::string& json_path) {
    arch_def = loadJsonFile(json_path);
}

nlohmann::json LikwidHook::loadJsonFile(const std::string& json_path) {
    std::ifstream file(json_path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open likwid metrics file: " + json_path);
    }
    nlohmann::json content;
    file >> content;
    return content;
}

const nlohmann::json* LikwidHook::findArchitecture(const std::string& family_code,
                                                    const std::string& architecture_code) const {
    auto architectures_it = arch_def.find("architectures");
    if (architectures_it == arch_def.end()) {
        return nullptr;
    }
    auto family_it = architectures_it->find(family_code);
    if (family_it == architectures_it->end() || !family_it->is_array()) {
        return nullptr;
    }
    for (const auto& architecture : *family_it) {
        auto definitions_it = architecture.find("definitions");
        if (definitions_it == architecture.end()) {
            continue;
        }
        auto abbreviation_it = definitions_it->find("architecture_abbreviation");
        if (abbreviation_it != definitions_it->end() && *abbreviation_it == architecture_code) {
            return &architecture;
        }
    }
    return nullptr;
}

std::string LikwidHook::resolveArchitectureCode(const std::string& family_code, int model_code, int stepping) const {
    auto architectures_it = arch_def.find("architectures");
    if (architectures_it == arch_def.end()) {
        throw std::runtime_error("No 'architectures' entry in the loaded JSON");
    }
    auto family_it = architectures_it->find(family_code);
    if (family_it == architectures_it->end() || !family_it->is_array()) {
        throw std::runtime_error("Unknown CPU family: " + family_code);
    }

    std::string fallback;
    for (const auto& architecture : *family_it) {
        auto definitions_it = architecture.find("definitions");
        if (definitions_it == architecture.end()) {
            continue;
        }
        auto models_it = definitions_it->find("models");
        if (models_it == definitions_it->end() || !models_it->is_array()) {
            continue;
        }
        std::string abbreviation = definitions_it->at("architecture_abbreviation").get<std::string>();
        for (const auto& model : *models_it) {
            if (model.at("model_code").get<int>() != model_code) {
                continue;
            }
            auto condition_it = model.find("condition");
            if (condition_it == model.end()) {
                if (fallback.empty()) {
                    fallback = abbreviation;
                }
                continue;
            }
            if (evaluateSteppingCondition(condition_it->get<std::string>(), stepping)) {
                return abbreviation;
            }
        }
    }
    if (!fallback.empty()) {
        return fallback;
    }
    throw std::runtime_error("No architecture found for family " + family_code + " model " +
                              std::to_string(model_code));
}

bool LikwidHook::isFlops(const std::string& family_code, const std::string& architecture_code) const {
    const nlohmann::json* architecture = findArchitecture(family_code, architecture_code);
    if (architecture == nullptr) {
        return false;
    }
    auto metrics_it = architecture->find("metrics");
    if (metrics_it == architecture->end()) {
        return false;
    }
    auto flops_it = metrics_it->find("flops");
    return flops_it != metrics_it->end() && flops_it->is_array() && !flops_it->empty();
}

bool LikwidHook::isMem(const std::string& family_code, const std::string& architecture_code) const {
    const nlohmann::json* architecture = findArchitecture(family_code, architecture_code);
    if (architecture == nullptr) {
        return false;
    }
    auto metrics_it = architecture->find("metrics");
    if (metrics_it == architecture->end()) {
        return false;
    }
    auto memory_it = metrics_it->find("memory_volume");
    return memory_it != metrics_it->end() && memory_it->is_array() && !memory_it->empty();
}

std::vector<std::string> LikwidHook::getEvents(const std::string& family_code,
                                                const std::string& architecture_code,
                                                const std::string& category) const {
    std::vector<std::string> events;
    const nlohmann::json* architecture = findArchitecture(family_code, architecture_code);
    if (architecture == nullptr) {
        return events;
    }
    auto metrics_it = architecture->find("metrics");
    if (metrics_it == architecture->end()) {
        return events;
    }
    auto category_it = metrics_it->find(category);
    if (category_it == metrics_it->end() || !category_it->is_array()) {
        return events;
    }
    std::vector<std::string> seen;
    for (const auto& metric : *category_it) {
        auto events_it = metric.find("events");
        if (events_it == metric.end()) {
            continue;
        }
        for (const auto& [counter, event_name] : events_it->items()) {
            std::string name = event_name.get<std::string>();
            if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
                seen.push_back(name);
                events.push_back(name);
            }
        }
    }
    return events;
}

std::vector<std::string> LikwidHook::get_flops_events(const std::string& family_code,
                                                        const std::string& architecture_code) const {
    return getEvents(family_code, architecture_code, "flops");
}

std::vector<std::string> LikwidHook::get_mem_events(const std::string& family_code,
                                                      const std::string& architecture_code) const {
    return getEvents(family_code, architecture_code, "memory_volume");
}

std::map<std::string, double> LikwidHook::computeMetrics(const std::string& family_code,
                                                           const std::string& architecture_code,
                                                           const std::string& category,
                                                           const std::map<std::string, double>& events) const {
    std::map<std::string, double> results;
    const nlohmann::json* architecture = findArchitecture(family_code, architecture_code);
    if (architecture == nullptr) {
        throw std::runtime_error("Unknown family/architecture: " + family_code + "/" + architecture_code);
    }
    auto metrics_it = architecture->find("metrics");
    if (metrics_it == architecture->end()) {
        return results;
    }
    auto category_it = metrics_it->find(category);
    if (category_it == metrics_it->end() || !category_it->is_array()) {
        return results;
    }
    for (const auto& metric : *category_it) {
        std::string name = metric.at("name").get<std::string>();
        std::string expression = metric.at("formula").get<std::string>();

        // Substitute each counter (e.g. "PMC0") in the formula with its supplied event value.
        for (const auto& [counter, event_name] : metric.at("events").items()) {
            std::string event = event_name.get<std::string>();
            auto value_it = events.find(event);
            if (value_it == events.end()) {
                throw std::runtime_error("Missing value for event '" + event + "' required by metric '" + name + "'");
            }
            std::ostringstream value_stream;
            value_stream << std::setprecision(17) << value_it->second;
            expression = std::regex_replace(expression, std::regex("\\b" + counter + "\\b"), value_stream.str());
        }

        detail::FormulaEvaluator evaluator(expression);
        results[name] = evaluator.evaluate();
    }
    return results;
}

std::map<std::string, double> LikwidHook::flops(const std::string& family_code,
                                                  const std::string& architecture_code,
                                                  const std::map<std::string, double>& events) const {
    return computeMetrics(family_code, architecture_code, "flops", events);
}

std::map<std::string, double> LikwidHook::mem_volume(const std::string& family_code,
                                                       const std::string& architecture_code,
                                                       const std::map<std::string, double>& events) const {
    return computeMetrics(family_code, architecture_code, "memory_volume", events);
}
