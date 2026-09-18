cmake_minimum_required(VERSION 3.14)
project(likwid_hook_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(likwid_hook src/LikwidHook.cpp)
target_include_directories(likwid_hook PUBLIC include)
target_link_libraries(likwid_hook PUBLIC nlohmann_json::nlohmann_json)

set(PFM_ROOT "" CACHE PATH "Optional custom install prefix for libpfm4 (perfmon), e.g. /opt/libpfm4")

find_path(PFM_INCLUDE_DIR NAMES perfmon/pfmlib.h HINTS "${PFM_ROOT}" PATH_SUFFIXES include)
find_library(PFM_LIBRARY NAMES pfm HINTS "${PFM_ROOT}" PATH_SUFFIXES lib lib64)

if(PFM_INCLUDE_DIR AND PFM_LIBRARY)
    add_executable(likwid_flops_profiler src/main.cpp src/PerfRecorder.cpp)
    target_include_directories(likwid_flops_profiler PRIVATE ${PFM_INCLUDE_DIR})
    target_link_libraries(likwid_flops_profiler PRIVATE likwid_hook ${PFM_LIBRARY})
else()
    message(WARNING "libpfm4 not found (headers and/or library); skipping likwid_flops_profiler. "
                     "Install it with e.g. 'sudo apt-get install libpfm4-dev'.")
endif()
