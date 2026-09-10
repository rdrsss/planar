# Fixture (g): generator expressions are unevaluated at configure time, so
# an edge hidden in `$<...>` slips past a walk that only pattern-matches
# `^planar_(.+)$` against the raw LINK_LIBRARIES/INTERFACE_LINK_LIBRARIES
# strings. Plan 996 task 6069 (M3 pre-work).
#
# `$<0:planar_engine_widget>` is CMake's canonical "always-false, drop this
# entry" generator expression — it never actually gets planar_engine_widget
# to the link line at generate time. That is deliberate: what this fixture
# proves is that cmake/architecture.cmake DETECTS the mere presence of a
# `$<` generator expression and refuses configure outright (see that file's
# header comment, "Generator expressions") — not that it evaluates the
# expression correctly. Even a permanently-false genex must be refused,
# because this walk explicitly does not attempt to evaluate what CMake
# itself defers to generate time; pretending otherwise for "obviously safe"
# forms would be exactly the fragile partial-evaluation this task rejects.
#
# See ../upward/CMakeLists.txt for why this is plain translation units, not
# C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_genex_edge CXX)

add_library(planar_engine_widget STATIC engine_widget.cpp)  # layer 2
add_library(planar_core STATIC core.cpp)                    # layer 1

# The induced violation: an upward edge hidden inside a generator
# expression, wrapped so it is a no-op at generate time and would never be
# caught by CMake's own build graph either — the guard must refuse this
# purely because it cannot evaluate it at configure time, not because the
# edge would ever actually materialize.
target_link_libraries(planar_core PUBLIC "$<0:planar_engine_widget>")

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_engine_widget planar_core)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
