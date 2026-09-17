#pragma once

#include <map>
#include <string>
#include <vector>

// Runs `command` (program + its arguments) as a child process, records the
// given hardware event names (libpfm4 symbolic names, e.g.
// "FP_ARITH_INST_RETIRED_SCALAR_DOUBLE") via Linux perf_events for the
// lifetime of that child, and returns event name -> raw counter value.
//
// Throws std::runtime_error if libpfm4 cannot encode an event or if
// perf_event_open() fails. perf_events support is commonly unavailable
// under WSL, so failures here should be expected on such hosts.
std::map<std::string, double> profile_events(const std::vector<std::string>& command,
                                              const std::vector<std::string>& event_names);
