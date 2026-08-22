# Fixture (k): F3 (plan 996, task 6078, M3 review remediation). A blind
# review reproduced this exactly: `planar_cmd_execute` links
# `planar_engine_execute` (making it an execute carrier) and separately
# links `planar_engine_middleware`, which is declared with a RAW
# add_library() — never routed through planar_module()/planar_binary(),
# so it is never appended to PLANAR_MODULE_TARGETS. `planar_engine_middleware`
# itself links `planar_db` directly — a real carrier -> db violation, the
# exact scenario ../execute-transitive-db/ already proves the closure-aware
# walk catches THROUGH A REGISTERED intermediate. Here the intermediate is
# UNREGISTERED: before F3's fix, the BFS closure in
# cmake/architecture.cmake had no adjacency recorded for
# `engine_middleware` at all (pass 1 only builds adjacency for targets in
# PLANAR_MODULE_TARGETS), so the walk over `planar_cmd_execute`'s closure
# silently stopped at `engine_middleware` and never reached `db` —
# configure SUCCEEDED ("GUARD PASSED") against a genuine violation.
# module.cmake's "every planar_* target MUST use this helper" was a
# comment, not a check.
#
# After the fix, pass 1b notices that `planar_cmd_execute` (and, forced by
# construction here, `planar_engine_middleware` is a real TARGET named
# `planar_engine_middleware` reachable from a registered target's link
# list) is a `planar_`-prefixed name that IS a TARGET but is absent from
# the registered set, and refuses configure — independent of whether the
# db edge would ever have been reached.
#
# See ../upward/CMakeLists.txt for why this is plain translation units,
# not C++ modules, and ../execute-transitive-db/CMakeLists.txt for why this
# is a manual add_executable() + PLANAR_MODULE_TARGETS registration rather
# than the real planar_binary() (no toolchain wiring needed either).
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_unregistered_target_caught CXX)

add_library(planar_db STATIC db.cpp)                                # layer 1, registered
add_library(planar_engine_execute STATIC engine_execute.cpp)        # layer 2, registered
add_library(planar_engine_middleware STATIC engine_middleware.cpp)  # layer 2, DELIBERATELY UNREGISTERED

# Legal in isolation, but planar_engine_middleware itself is never
# registered below — this is the real carrier->db violation that must
# still be caught even though it is only reachable through an
# unregistered intermediate.
target_link_libraries(planar_engine_middleware PUBLIC planar_db)

add_executable(planar_cmd_execute main.cpp)  # layer 3 (^cmd_ prefix)
target_link_libraries(planar_cmd_execute PRIVATE
  planar_engine_execute      # makes this target an execute carrier
  planar_engine_middleware)  # unregistered intermediate reaching planar_db

# planar_engine_middleware is DELIBERATELY OMITTED from this list — that
# omission is the induced defect under test.
set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_db planar_engine_execute planar_cmd_execute)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
