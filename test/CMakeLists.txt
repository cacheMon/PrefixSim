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
