# Fixture: a generator expression that resolves to a planar_* target name
# WITHOUT the literal substring `planar_` appearing in the stored link
# entry. Plan 996 task 6074.
#
# ../genex-edge/ proves the guard refuses `$<0:planar_engine_widget>`.
# That entry contains `planar_` literally, which is exactly what the
# pre-task-6074 check matched on — so genex-edge/ passed against a check
# that was blind to the case below, and could not have told the two apart.
#
# `$<TARGET_PROPERTY:dep_holder,DEP>` is the smallest form of the blind
# spot: the entry CMake stores names only `dep_holder` and `DEP`, neither
# of which contains `planar_`, and it resolves at GENERATE time to
# whatever that property holds — here, the layer-2 `planar_engine_widget`,
# which a layer-1 target must never depend on. Nothing available at
# configure time can distinguish this from a genex that resolves to a
# third-party name, which is precisely why the guard now refuses EVERY
# `$<` entry that survives the unconditional `$<LINK_ONLY:...>` unwrap
# rather than pattern-matching for plausibility (see
# cmake/architecture.cmake's header comment, "Generator expressions", for
# the measurement showing the real Planar graph has zero such entries).
#
# THIS FIXTURE IS THE DISCRIMINATOR: it configures CLEANLY against the
# pre-task-6074 check (the guard never sees an edge at all — the entry
# does not match `^planar_(.+)$` either, so it is silently dropped) and
# FATALs against the current one.
#
# See ../upward/CMakeLists.txt for why this is plain translation units,
# not C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_genex_no_literal CXX)

add_library(planar_engine_widget STATIC engine_widget.cpp)  # layer 2
add_library(planar_core STATIC core.cpp)                    # layer 1

# The name is carried on a NON-planar interface target, so the link entry
# CMake records mentions neither `planar_` nor the real dependency.
add_library(dep_holder INTERFACE)
set_property(TARGET dep_holder PROPERTY DEP planar_engine_widget)

# The induced violation: the same upward edge ../upward/ and ../genex-edge/
# induce, hidden behind a genex whose text gives the guard nothing to
# pattern-match on.
target_link_libraries(planar_core PUBLIC "$<TARGET_PROPERTY:dep_holder,DEP>")

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_engine_widget planar_core)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
