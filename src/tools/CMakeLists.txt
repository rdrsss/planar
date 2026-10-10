# src/tools/ — project tooling, one directory per tool (decision 1000).
#
# Each tool here is a standalone executable, not a planar_module() target.
# The lint tools inspect shipped binaries by subprocess.

add_subdirectory(cli_usage_lint)
add_subdirectory(cli_docs_coverage)
add_subdirectory(surface_lint)
add_subdirectory(centurion_client_proof)
add_subdirectory(queue_contention_probe)

# scripts/render-codex-agents.py (Codex custom-agent TOML renderer) unit tests.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME codex_agents_render
  COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/scripts/render_codex_agents_test.py")
set_tests_properties(codex_agents_render PROPERTIES LABELS "scripts")
