# src/ — tech-spec § Target layout / § File-level tree.
#
# `cmd/` (operator/agent-callable binaries) landed at plan 996 task 6105 with
# the `planar` operator binary — the first layer-3 target and the first point
# at which a C++ binary runs a verb end to end. `planar-agent`,
# `planar-watch` and `planar-execute` follow in task 6107.
#
# Order matters: `lib/` configures before the engine, and both configure
# before command binaries that name their targets in DEPENDS.

add_subdirectory(lib)
add_subdirectory(engine)
add_subdirectory(cmd)
