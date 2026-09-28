# Command-level lifecycle scenarios use the real planar tree and handlers.
# Compile them into the existing test target so no cmd -> cmd link edge is added.
target_sources(planar_cmd_planar_tests PRIVATE planning_lifecycle.t.cpp)
