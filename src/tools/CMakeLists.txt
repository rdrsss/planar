# src/tools/ — project tooling, one directory per tool (decision 1000).
#
# Each tool here is a standalone executable, not a planar_module() target.
# The lint tools inspect shipped binaries by subprocess; Scriptorium has a
# private C++ module but no dependency on the planning-state command graph.

add_subdirectory(cli_usage_lint)
add_subdirectory(cli_docs_coverage)
add_subdirectory(surface_lint)
add_subdirectory(scriptorium)
add_subdirectory(queue_contention_probe)
