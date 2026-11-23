# The three-phase serve loop, the oracles and the read-only probes, on
# hand-traceable traces
add_executable(testPrefixsim test_prefixsim.cpp)
prefixsim_target_options(testPrefixsim)
target_link_libraries(testPrefixsim prefixsim_lib)
add_test(NAME testPrefixsim COMMAND testPrefixsim)

# lcp trace format: the ids prefixsim assigns, its reader, the round trips
add_executable(testLcp test_lcp.cpp)
prefixsim_target_options(testLcp)
target_link_libraries(testLcp prefixsim_lib)
add_test(NAME testLcp COMMAND testLcp)

# Fixed-size quantiles: exact tiny samples, certified intervals on 2M gaps.
add_executable(testGapQuantiles test_gap_quantiles.cpp)
prefixsim_target_options(testGapQuantiles)
add_test(NAME testGapQuantiles COMMAND testGapQuantiles)

# Real trace reader/CLI: large raw ids, exact/approx quantiles and bad times.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME testTraceReuseStatCli
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_trace_reuse_stat_cli.py
                $<TARGET_FILE:traceReuseStat>)
endif()
