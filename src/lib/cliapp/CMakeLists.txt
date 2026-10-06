# planar_cliapp — the layer-1 surface that survived deleting src/lib/cli
# (plan 996, task 6123).
#
# WHAT THIS TARGET IS NOT: it is not a renamed `planar_cli`. That module was
# a hand-rolled command-tree type, argv parser, help renderer, exit-code
# table, JSON emitter and completion generator — 5,684 lines. Task 6123
# deleted it and made each `src/cmd/<binary>/` build its own `CLI::App`
# directly against `import cli11;`. CLI11 now owns tokenization, value
# coercion, subcommand resolution, required/choice enforcement, and help
# rendering. Nothing in this directory parses argv or declares a verb.
#
# WHAT IS HERE, and why each piece is layer-1 shared rather than
# per-binary duplicated:
#
#   walk        Tree-walk primitives over a built CLI::App (visible
#               children, all-nodes, inherited flags, canonical names).
#               Pure functions OF a tree; they hold none.
#
#   args        The parsed-argv value shape handlers read, harvested out of
#               a CLI::App after parse — plus `parse_int64_zig`, the SINGLE
#               remaining implementation of Zig's `std.fmt.parseInt(i64, s,
#               10)` semantics. The tree used to carry THREE (cli/parser.cpp
#               plus a verbatim copy in each of cmd/planar/args.cppm and
#               cmd/planar-agent/args.cppm); the task brief asked for
#               consolidation on the survivor and this is it.
#
#   schema      The `<bin> schema` JSON catalog `zig/tools/cli_usage_lint`
#               consumes. CLI11 emits nothing of the sort, and the lint is
#               a live gate in `make cli-usage-check`. See schema.cppm's
#               header for the measured verdict (the lint reads SIX keys;
#               `deprecated` and the twelve-key `doc` blob are not among
#               them, and the emitter this replaces sourced both from
#               hardcoded constants anyway).
#
#   surface     Declaring a whole command surface onto a built CLI::App
#               from data (plan 996, task 6065). The same argument as
#               `schema` one step earlier: three binaries need the SAME
#               find-or-create walk over a generated node table, and the
#               table itself is per-binary and lives under src/cmd/. This
#               holds no tree either -- it takes one and extends it.
#
#   completion  The shell-completion generator. CLI11 2.7.2 ships no
#               completion facility at all, so the choice was keep-or-drop,
#               not adopt-or-keep. See completion.cppm's header.
#
#   version     Build-metadata resolution and `<bin> version` rendering.
#               Relocated UNCHANGED from src/lib/cli/version.cppm: it never
#               had anything to do with parsing, and its opt-in
#               PLANAR_VERSION_META build-cache contract (below) is the one
#               piece of src/lib/cli/CMakeLists.txt that had to survive.
#
# D18 forbids a `cmd_* -> cmd_*` edge — the four binaries must not reach
# sideways into each other's command trees. That prohibition is about
# TREES. A layer-1 library that describes whatever tree it is handed is the
# same shape D19 already established for `scope_ref` and `json_text`, and
# the alternative here was a second and third verbatim copy of a 400-line
# JSON emitter — precisely the duplication that had already silently
# diverged in three of `json_text`'s ten call sites before it was
# extracted.

planar_module(cliapp
  INTERFACE
    walk.cppm
    args.cppm
    schema.cppm
    surface.cppm
    completion.cppm
    version.cppm
  SOURCES
    schema.cpp
    surface.cpp
    completion.cpp
    version.cpp
  DEPENDS
    json_text)

# Every interface unit here does `import cli11;`, so the module target must
# propagate to consumers — PUBLIC, not PRIVATE.
target_link_libraries(planar_cliapp PUBLIC CLI11::CLI11_Module)
if(TARGET planar_cliapp_tests)
  target_link_libraries(planar_cliapp_tests PRIVATE CLI11::CLI11_Module)
endif()

# --- version metadata (relocated verbatim from src/lib/cli/CMakeLists.txt) --
#
# Opt-in, mirroring zig/build.zig's `-Dversion-meta` contract (CLAUDE.md §
# Build And Test: "Two build-cache invariants keep iterative builds cheap").
# Default OFF: `planar::cliapp::current_build_info()` (version.cpp) reads the
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
  # Exact source snapshot supplied by the dist host wrapper (Docker has no .git).
  set(PLANAR_SOURCE_SHA "" CACHE STRING "Trusted distribution source commit SHA")
  set(PLANAR_SOURCE_DIRTY "" CACHE STRING "Trusted distribution source dirty state (0 or 1)")
  if(PLANAR_SOURCE_SHA OR NOT PLANAR_SOURCE_DIRTY STREQUAL "")
    string(LENGTH "${PLANAR_SOURCE_SHA}" _planar_source_sha_length)
    if(NOT _planar_source_sha_length EQUAL 40 OR NOT PLANAR_SOURCE_SHA MATCHES "^[0-9a-f]+$"
        OR NOT PLANAR_SOURCE_DIRTY MATCHES "^[01]$")
      message(FATAL_ERROR "Distribution source identity requires a full SHA and dirty state 0 or 1")
    endif()
    if(_planar_git_sha_result EQUAL 0 AND
        (NOT PLANAR_SOURCE_SHA STREQUAL _planar_git_sha OR
         NOT PLANAR_SOURCE_DIRTY STREQUAL "${_planar_git_dirty}"))
      message(FATAL_ERROR "Distribution source identity differs from the git snapshot")
    endif()
    set(_planar_git_sha "${PLANAR_SOURCE_SHA}")
    set(_planar_git_dirty "${PLANAR_SOURCE_DIRTY}")
  endif()
  string(TIMESTAMP _planar_build_date "%Y-%m-%dT%H:%M:%SZ" UTC)

  target_compile_definitions(planar_cliapp PRIVATE
    "PLANAR_GIT_SHA=\"${_planar_git_sha}\""
    "PLANAR_BUILD_DATE=\"${_planar_build_date}\""
    "PLANAR_GIT_DIRTY=${_planar_git_dirty}")
  if(TARGET planar_cliapp_tests)
    target_compile_definitions(planar_cliapp_tests PRIVATE PLANAR_VERSION_META=1)
  endif()
endif()

# A release label is independent of the opt-in git metadata contract.
set(PLANAR_RELEASE_VERSION "" CACHE STRING "Release tag printed by version (empty prints dev)")
if(PLANAR_RELEASE_VERSION)
  target_compile_definitions(planar_cliapp PRIVATE "PLANAR_RELEASE_VERSION=\"${PLANAR_RELEASE_VERSION}\"")
endif()

# --- schema-catalog lint-parity stub --------------------------------------
#
# Relocated from src/lib/cli/CMakeLists.txt and re-pointed at the CLI11
# emitter. Deliberately not a planar_module() target: a standalone
# executable, literally named `planar` (via OUTPUT_NAME, isolated in its own
# RUNTIME_OUTPUT_DIRECTORY so it never collides with anything else in the
# build tree), that answers `<bin> schema` the way `zig/tools/cli_usage_lint`
# expects. schema.t.cpp's `[lint-parity]` case runs the real, UNMODIFIED
# lint tool against this binary's stdout — the standing evidence that
# deleting src/lib/cli did not break the `make cli-usage-check` gate.
add_executable(planar_cliapp_schema_stub schema_stub.cpp)
set_target_properties(planar_cliapp_schema_stub PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON
  OUTPUT_NAME "planar"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/schema_stub_bin")
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(planar_cliapp_schema_stub PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()
target_link_libraries(planar_cliapp_schema_stub PRIVATE planar_cliapp CLI11::CLI11_Module)

if(TARGET planar_cliapp_tests)
  target_compile_definitions(planar_cliapp_tests PRIVATE
    PLANAR_CLIAPP_SCHEMA_STUB_BIN="$<TARGET_FILE:planar_cliapp_schema_stub>")
  add_dependencies(planar_cliapp_tests planar_cliapp_schema_stub)
endif()

# The PLANAR_CLI_USAGE_LINT_BIN definition and its add_dependencies edge
# live in the top-level CMakeLists.txt, not here: `cli_usage_lint`
# (src/tools/cli_usage_lint/) is configured by add_subdirectory(src/tools),
# which runs AFTER add_subdirectory(src) — so the target does not exist yet
# at this point in the configure walk. See that file's comment beside
# add_subdirectory(src/tools).
