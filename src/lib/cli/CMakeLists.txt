# planar.cli — the etcli-zig command-tree/parser/help/completion/schema port
# (plan 996, tasks cpp-cli-tree-parity + cpp-cli-schema-catalog). Layer-1
# base module: no DEPENDS on engine/cmd (D15). `validate`
# (authoring-time tree-shape lint) is still a separate, not-yet-landed
# task and is not part of this CMake target yet.
#
# Test binaries shell the reference Zig binaries (parser.t.cpp,
# help.t.cpp, schema.t.cpp) to assert byte-for-byte exit-code, help-text,
# and schema-catalog parity for the modeled verbs (task add/done under
# `./zig/zig-out/bin/planar`, agent fail under
# `./zig/zig-out/bin/planar-agent`) — same SKIP-when-absent pattern as
# src/lib/db/migrate.t.cpp (D6, the zig/ tree is the parity oracle until
# M10 cutover).

planar_module(cli
  INTERFACE
    flag.cppm
    cmd.cppm
    error.cppm
    parser.cppm
    help.cppm
    completion.cppm
    schema.cppm
    version.cppm
    output.cppm
    exit.cppm
    cli.cppm
  SOURCES
    parser.cpp
    help.cpp
    completion.cpp
    schema.cpp
    version.cpp)

# output.cppm's emit()/emit_list() templates call glz::write_json directly
# (D8: Glaze is the serialization layer) — PUBLIC so a downstream module
# that imports planar.cli.output for instantiation picks up Glaze's
# (header-only) include paths transitively, same as any other module that
# reaches for it.
target_link_libraries(planar_cli PUBLIC glaze::glaze)

# --- version metadata (task cpp-cli-output-logging) ------------------------
#
# Opt-in, mirroring zig/build.zig's `-Dversion-meta` contract (CLAUDE.md §
# Build And Test: "Two build-cache invariants keep iterative builds cheap").
# Default OFF: `planar::cli::current_build_info()` (version.cpp) reads the
# PLANAR_GIT_SHA / PLANAR_BUILD_DATE / PLANAR_GIT_DIRTY macros only when
# defined, and falls back to the "dev"/"dev"/false sentinel otherwise — so
# a default dev configure never re-resolves git state (and never
# re-triggers a rebuild) on every commit or every clean<->dirty flip.
# Passing -DPLANAR_VERSION_META=ON (the install-time equivalent of
# install.sh's -Dversion-meta=true) resolves real git metadata ONCE, at
# configure time, into these definitions.
option(PLANAR_VERSION_META "Embed real git sha/date/dirty in \`planar version\` (invalidates the build cache on every commit and dirty-flag flip; default OFF embeds \"dev\")" OFF)
if(PLANAR_VERSION_META)
  find_package(Git QUIET)
  set(_planar_git_sha "unknown")
  set(_planar_git_dirty 0)
  if(GIT_EXECUTABLE)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _planar_git_sha_out
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _planar_git_sha_result
      ERROR_QUIET)
    if(_planar_git_sha_result EQUAL 0 AND _planar_git_sha_out)
      set(_planar_git_sha "${_planar_git_sha_out}")
    endif()

    execute_process(
      COMMAND "${GIT_EXECUTABLE}" status --porcelain
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _planar_git_status_out
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _planar_git_status_result
      ERROR_QUIET)
    if(_planar_git_status_result EQUAL 0 AND _planar_git_status_out)
      set(_planar_git_dirty 1)
    endif()
  endif()
  string(TIMESTAMP _planar_build_date "%Y-%m-%dT%H:%M:%SZ" UTC)

  target_compile_definitions(planar_cli PRIVATE
    "PLANAR_GIT_SHA=\"${_planar_git_sha}\""
    "PLANAR_BUILD_DATE=\"${_planar_build_date}\""
    "PLANAR_GIT_DIRTY=${_planar_git_dirty}")
  if(TARGET planar_cli_tests)
    target_compile_definitions(planar_cli_tests PRIVATE PLANAR_VERSION_META=1)
  endif()
endif()

# --- schema-catalog lint-parity stub (task cpp-cli-schema-catalog) --------
#
# Deliberately not a planar_module() target (see schema_stub.cpp's header
# comment) — a standalone executable, literally named `planar` (via
# OUTPUT_NAME, isolated in its own RUNTIME_OUTPUT_DIRECTORY so it never
# collides with anything else in the build tree), that answers `<bin>
# schema` the way `zig/tools/cli_usage_lint` expects. schema.t.cpp's
# `[lint-parity]` case runs the real, unmodified lint tool against this
# binary's stdout.
add_executable(planar_cli_schema_stub schema_stub.cpp)
set_target_properties(planar_cli_schema_stub PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON
  OUTPUT_NAME "planar"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/schema_stub_bin")
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(planar_cli_schema_stub PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()
target_link_libraries(planar_cli_schema_stub PRIVATE planar_cli)

if(TARGET planar_cli_tests)
  target_compile_definitions(planar_cli_tests PRIVATE
    PLANAR_ZIG_PLANAR_BIN="${CMAKE_SOURCE_DIR}/zig/zig-out/bin/planar"
    PLANAR_ZIG_PLANAR_AGENT_BIN="${CMAKE_SOURCE_DIR}/zig/zig-out/bin/planar-agent"
    PLANAR_ZIG_CLI_USAGE_LINT_SRC="${CMAKE_SOURCE_DIR}/zig/tools/cli_usage_lint.zig"
    PLANAR_CLI_SCHEMA_STUB_BIN="$<TARGET_FILE:planar_cli_schema_stub>")
  add_dependencies(planar_cli_tests planar_cli_schema_stub)
  # schema.t.cpp parses catalogs generically via glz::generic to prove
  # structural (not string) equality against the reference binaries.
  target_link_libraries(planar_cli_tests PRIVATE glaze::glaze)
endif()
