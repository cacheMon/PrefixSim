cmake_minimum_required(VERSION 3.12)
project(PrefixSim C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(PREFIXSIM_ENABLE_TESTS "build the PrefixSim tests" ON)

# ==============================================================================
# libCacheSim, from the submodule
# ==============================================================================
# The eviction algorithms, the pin predicate and the lcp reader live there.
# EXCLUDE_FROM_ALL builds only what PrefixSim links; its other tools, such as
# cachesim and tracePrint, can still be built by name. Its options
# (ENABLE_S4FIFO, ENABLE_LRB, ENABLE_3L_CACHE, SUPPORT_TTL, LOG_LEVEL, ...)
# pass straight through.
set(LIBCACHESIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libCacheSim)
if(NOT EXISTS ${LIBCACHESIM_DIR}/CMakeLists.txt)
    message(FATAL_ERROR
        "libCacheSim/ is empty: run `git submodule update --init`")
endif()
# libCacheSim's own tests are run in libCacheSim.
set(ENABLE_TESTS OFF CACHE BOOL "build libCacheSim's tests" FORCE)
list(APPEND CMAKE_MODULE_PATH ${LIBCACHESIM_DIR}/cmake/Modules)
add_subdirectory(${LIBCACHESIM_DIR} EXCLUDE_FROM_ALL)

# libCacheSim sets its feature switches as directory-wide definitions, and
# some of them (SUPPORT_TTL, ENABLE_S4FIFO, ...) change the structs PrefixSim
# shares with it, so PrefixSim compiles with exactly the same ones.
get_directory_property(LIBCACHESIM_DEFINITIONS
    DIRECTORY ${LIBCACHESIM_DIR} COMPILE_DEFINITIONS)
get_directory_property(LIBCACHESIM_INCLUDES
    DIRECTORY ${LIBCACHESIM_DIR} INCLUDE_DIRECTORIES)
get_directory_property(LIBCACHESIM_CXX_FLAGS
    DIRECTORY ${LIBCACHESIM_DIR} DEFINITION LIBCACHESIM_CXX_FLAGS)
get_directory_property(GLib_INCLUDE_DIRS
    DIRECTORY ${LIBCACHESIM_DIR} DEFINITION GLib_INCLUDE_DIRS)

# libCacheSim links glib by name and adds its directories to the linker search
# path for its own targets only, so PrefixSim's executables need them too
# (glib is off the default path on macOS).
get_directory_property(GLib_LIBRARY_DIRS
    DIRECTORY ${LIBCACHESIM_DIR} DEFINITION GLib_LIBRARY_DIRS)
link_directories(${GLib_LIBRARY_DIRS})

# ==============================================================================
# prefixsim
# ==============================================================================
# The simulator core lives in a library so the tests can drive it in-process
# without going through the CLI, and traceConvLLM shares its parser and writers.
add_library(prefixsim_lib STATIC
    src/simulator.cpp
    src/trace.cpp
    src/lcp_writer.cpp
    src/jsonl_writer.cpp
)
target_compile_definitions(prefixsim_lib PUBLIC ${LIBCACHESIM_DEFINITIONS})
target_include_directories(prefixsim_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBCACHESIM_INCLUDES}
    ${GLib_INCLUDE_DIRS}
)
target_link_libraries(prefixsim_lib PUBLIC libCacheSim)

# Every PrefixSim target builds with libCacheSim's warnings, -Werror included.
function(prefixsim_target_options target)
    target_compile_options(${target} PRIVATE ${LIBCACHESIM_CXX_FLAGS})
endfunction()

prefixsim_target_options(prefixsim_lib)

add_executable(prefixsim src/main.cpp)
prefixsim_target_options(prefixsim)
target_link_libraries(prefixsim prefixsim_lib)

# traceConvLLM -- qwen-style JSONL <-> lcp. It links prefixsim_lib to reuse its
# JSONL parser, positional encoding and next-access annotation, so the
# converter and the simulator cannot disagree about what a trace means.
add_executable(traceConvLLM tools/traceConvLLM.cpp)
prefixsim_target_options(traceConvLLM)
target_link_libraries(traceConvLLM prefixsim_lib)

install(TARGETS prefixsim traceConvLLM RUNTIME DESTINATION bin)

# ==============================================================================
# tests
# ==============================================================================
if(PREFIXSIM_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
