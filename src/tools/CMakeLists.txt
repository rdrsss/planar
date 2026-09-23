# src/tools/ — project tooling, one directory per tool (decision 1000).
#
# Each tool here is a standalone executable, deliberately NOT a
# planar_module() target: it has no module interface other consumers
# import, and (for cli_usage_lint and surface_lint specifically) it must
# lint the ACTUAL SHIPPED BINARY via subprocess, so it stays outside the
# planar_* module graph D15 polices. See each tool's own CMakeLists.txt.

add_subdirectory(cli_usage_lint)
add_subdirectory(cli_docs_coverage)
add_subdirectory(surface_lint)
add_subdirectory(centurion_client_proof)
