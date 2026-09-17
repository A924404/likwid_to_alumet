# LikwidHook demo

A small C++ library plus a `likwid_flops_profiler` executable that:

1. Reads a `likwid_metrics.json`-shaped file through the `LikwidHook` class.
2. Detects the host CPU family/model/stepping from `/proc/cpuinfo` and resolves
   the matching architecture record.
3. Records the FLOPS-related hardware events (via Linux `perf_events`) while
   running an arbitrary program given on the command line.
4. Applies the JSON formulas to the recorded counters and prints the resulting
   FLOPS metrics.

## Layout

```
demo/
├── CMakeLists.txt
├── include/LikwidHook.hpp   # class declaration
└── src/
    ├── LikwidHook.cpp       # implementation
    ├── FormulaEvaluator.hpp # numeric "+ - * / ( )" expression evaluator
    ├── CpuInfo.hpp          # /proc/cpuinfo family/model/stepping parser
    ├── PerfRecorder.hpp/.cpp# perf_events-based hardware counter recording
    └── main.cpp             # likwid_flops_profiler entry point
```

## Requirements

* Linux (reads `/proc/cpuinfo` and uses the `perf_events` kernel subsystem)
* CMake >= 3.14 and a C++17 compiler
* Internet access on first configure (CMake `FetchContent` downloads
  [nlohmann/json](https://github.com/nlohmann/json))
* [libpfm4](http://perfmon2.sourceforge.net/) development headers/library
  (`sudo apt-get install libpfm4-dev` on Debian/Ubuntu), used to translate
  the event names from the JSON into a `perf_event_attr`. If it isn't found,
  `likwid_flops_profiler` is skipped and only the `likwid_hook` library is built.
* Sufficient permissions to use `perf_events` (e.g. `perf_event_paranoid` low
  enough, or `CAP_PERFMON`/root)

> **WSL note:** WSL generally does not implement the `perf_events` subsystem
> (or only partially), so `perf_event_open()` typically fails there with
> `ENOSYS`/`EINVAL`/permission errors regardless of privileges. The CPU
> detection and FLOPS-formula logic can still be exercised independently of
> `perf`, but the profiler itself needs a real Linux kernel with `perf_events`
> support (bare metal or a compatible VM) to actually record counters.

## Build

```sh
cd demo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces the `likwid_hook` static library and, if libpfm4 was found,
the `likwid_flops_profiler` executable under `build/`.

## Run the profiler

```sh
./build/likwid_flops_profiler <program> [program-args...]
```

For example, from the repository root:

```sh
./demo/build/likwid_flops_profiler /path/to/your/benchmark --its-own-args
```

It expects `likwid_metrics.json` to be present in the current working
directory. The tool will:

1. Parse `/proc/cpuinfo` to get the CPU family, model, and stepping.
2. Resolve the matching `architecture_abbreviation` from the JSON (using each
   model's `condition`, e.g. `"stepping >= 5"`, when a model code is shared by
   several architectures).
3. Fail with a clear error if no FLOPS metrics are defined for that architecture.
4. Fork the target program, attach one perf counter per required FLOPS event
   (stopped until the target's `execve`, then enabled automatically via
   `enable_on_exec`), and let it run to completion.
5. Read back the raw counter values and evaluate each FLOPS formula, printing
   `metric name = value` pairs.

Memory-volume metrics (`isMem`/`get_mem_events`/`mem_volume`) are intentionally
not wired into the profiler yet; only FLOPS are recorded end-to-end.

## Using `LikwidHook` in your own code

```cpp
#include "LikwidHook.hpp"

LikwidHook hook("likwid_metrics.json");

// Discover what's available for a family/architecture pair.
bool has_flops = hook.isFlops("6", "CLX");
bool has_mem   = hook.isMem("6", "CLX");

// Or resolve the architecture code straight from /proc/cpuinfo values.
std::string architecture_code = hook.resolveArchitectureCode("6", /*model=*/85, /*stepping=*/7);

// Get the event names you need to measure.
std::vector<std::string> flops_events = hook.get_flops_events("6", "CLX");
std::vector<std::string> mem_events   = hook.get_mem_events("6", "CLX");

// Provide measured values keyed by event name...
std::map<std::string, double> events = {
    {"FP_ARITH_INST_RETIRED_128B_PACKED_DOUBLE", 12345.0},
    // ... one entry per name returned by get_flops_events/get_mem_events
};

// ...and get metric name -> computed value back.
std::map<std::string, double> flops_results = hook.flops("6", "CLX", events);
std::map<std::string, double> mem_results   = hook.mem_volume("6", "CLX", events);
```

`flops`/`mem_volume`/`resolveArchitectureCode` throw `std::runtime_error` if
the family/architecture/model is unknown or if `events` is missing a value
required by one of the formulas.

## Using it as a CMake dependency

From another CMake project:

```cmake
add_subdirectory(path/to/demo)
target_link_libraries(your_target PRIVATE likwid_hook)
target_include_directories(your_target PRIVATE path/to/demo/include)
```

`likwid_hook` (the `LikwidHook` class) has no dependency on `perf`/`libpfm4`;
that dependency is confined to the `likwid_flops_profiler` executable.

