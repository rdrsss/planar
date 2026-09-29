# planar_cmd_planar_agent — the `planar-agent` agent-callable binary
# (plan 996, task 6107).
#
# The SECOND layer-3 target. Task 6105 stood up the layer with `planar`;
# this task adds the other three binaries beside it, and the interesting
# question it had to answer was not "how do I write another main()" but
# "what happens to the vocabulary four cmd targets appear to share".
#
# --- shared layer-3 vocabulary, and why there is none -----------------------
#
# D18 keeps the same-layer prohibition for `cmd_*`, and
# cmake/architecture.cmake FATALs at configure time on a `cmd_* -> cmd_*`
# edge. So `context`, `exit`, `handler`, `dispatch` and `tree` are DUPLICATED
# per binary rather than shared. That looks like the guard forcing
# duplication. It is not, and the finding is worth stating plainly:
#
#   THE FOUR BINARIES DO NOT ACTUALLY SHARE THIS VOCABULARY. What looks
#   shared is exactly what the capability boundary makes different.
#
#     database acquisition   `planar`        opens read/write, creates the
#                                            parent directory, APPLIES
#                                            MIGRATIONS, then checks ahead.
#                            `planar-agent`  opens read/write, creates the
#                                            parent directory, NEVER
#                                            migrates, refuses BOTH skew
#                                            directions (this file's binary).
#                            `planar-watch`  opens STRICT READ-ONLY, creates
#                                            NOTHING, sets no WAL pragma,
#                                            never migrates.
#                            `planar-execute` has no SQLite handle at all.
#
#     exit codes             `parse_error` is 2 on `planar` and 1 here;
#                            `schema_version_behind` is 1 on `planar` and 7
#                            here. Both verified against the reference
#                            binaries, both invisible at a call site.
#
#   A single shared `context` would have to carry all four database
#   policies behind a flag, and a single shared `exit` would have to carry
#   the binary as a parameter — which is precisely the mistake task 6066
#   renamed `exit_code_for_parse_error_planar_binary` to prevent, one
#   defaulted argument away from applying the wrong policy silently.
#   Duplicating ~120 lines of identical-looking-but-differently-bound code
#   is the cheaper failure mode by a wide margin.
#
#   What IS genuinely shared lives one layer DOWN and was already there:
#   `planar.cli` owns the parser, the help renderer, the schema emitter and
#   the exit-code TABLE (`exit_code_for(kind, binary_kind)`), so no number
#   is duplicated — only the one-line binding of this binary to its own row
#   of it. The one addition this task made at layer 1 is
#   `render_version_text(program, ...)`, because each binary prints its own
#   name and the previous single-argument form hardcoded `"planar "`.
#
#   The test HARNESS is the one exception, and it is deduplicated as a
#   plain HEADER (`src/cmd/parity_harness.hpp`), which creates no target
#   edge — see that file's own header for why a library there would have
#   been both illegal and actively harmful.
#
# --- verb subset -----------------------------------------------------------
#
#   version   The spine: parse against THIS binary's tree, dispatch, bytes
#             on stdout, exit 0, no database. Also the program-name
#             divergence `render_version_text`'s new overload exists for.
#   schema    The capability boundary made machine-readable, and a FRAGMENT
#             renderer (schema_json documents "no trailing newline"), the
#             counterexample to a blanket append-nothing rule.
#   the claim ritual (task 6038)
#             pull / peek / claim / heartbeat / claim-associate /
#             complete / fail / release / block / action start|end /
#             reconcile / abort. See the section below.
#
# Parse failures (exit 1 — NOT the operator binary's 2) are covered across
# the set, so 0 and 1 are both produced by this binary and a mapping
# collapsed to one code cannot pass.
#
# --- the claim ritual (plan 996, task 6038) ---------------------------------
#
# TWELVE more verbs, and the reason this target grew two engine DEPENDS:
#
#   engine_runtime   the agent_work_claims + agent_actions store, the atomic
#                    claim-ritual transactions, and the stdout renderers
#                    (agentactivity / agentatomic / agentrender).
#   engine_planning  the task transition matrix and the plan roll-up, which
#                    `agentatomic` may not import itself (both are layer 2,
#                    D15/D18) and which THIS target injects into it as
#                    `agentatomic::task_policy`. `policy.cpp` is the whole
#                    of that seam -- one function, so every handler in this
#                    binary runs under the same policy.
#
# Layer 3 depending on two layer-2 buckets is exactly what the layering
# allows and what makes the injection legal; see engine/runtime's
# CMakeLists for why the alternative (duplicating the matrix, or extracting
# planning policy down to layer 1) was rejected.
#
# `locality.cpp` is the one file here that leaves the process: three `git`
# subprocesses per claim. It sits at layer 3 deliberately so the engine
# bucket takes a locality VALUE and needs no spawn dependency. Since plan
# 996 task 6128 it no longer owns the spawning itself — the `popen` +
# quoting + trim + exit-status handling moved to the layer-1 `planar.git`
# seam (DEPENDS git, below), which `src/cmd/planar/handlers/init.cpp` had
# independently duplicated and which `engine/identity` needs for worktree
# detection. What stays here is the locality POLICY: which three commands
# to run and how to read their answers.
#
# --- Remaining deferred behavior -------------------------------------------
#
# All declared agent leaves, including `ingest`, are ported.  The remaining
# deferral is `sessioncommits.
# recordClaimWindowBestEffort`, the git revision walk the four terminal
# handlers run AFTER their transaction. `--no-locality-probe` is still
# declared on all four (dropping a flag changes the --help bytes) and
# currently gates nothing there. Same call `capture commits` and `bench
# harvest` already made.
#
planar_binary(planar_agent
  MAIN
    main.cpp
  INTERFACE
    exit.cppm
    context.cppm
    database.cppm
    handler.cppm
    args.cppm
    locality.cppm
    policy.cppm
    main.cppm
    handlers/shared/cli.cppm
    handlers/version/command.cppm
    handlers/pull/command.cppm
    handlers/peek/command.cppm
    handlers/complete/command.cppm
    handlers/fail/command.cppm
    handlers/release/command.cppm
    handlers/block/command.cppm
    handlers/claim/command.cppm
    handlers/heartbeat/command.cppm
    handlers/claim_associate/command.cppm
    handlers/action/command.cppm
    handlers/ingest/command.cppm
    handlers/reconcile/command.cppm
    handlers/abort/command.cppm
    handlers/schema/command.cppm
    handlers/run/command.cppm
    handlers/dispatch/command.cppm
    handlers/context/command.cppm

    surface.cppm
    handlers/shared/support.cppm
    handlers/version/version.cppm
    handlers/schema/schema.cppm
    handlers/shared/claims.cppm
    handlers/shared/terminal.cppm
    handlers/shared/recovery.cppm
    handlers/action/action.cppm
    handlers/dispatch/dispatch.cppm
    handlers/context/context.cppm
    handlers/run/runs.cppm
    dispatch.cppm
  SOURCES
    context.cpp
    database.cpp
    locality.cpp
    policy.cpp
    handlers/version/version.cpp
    handlers/schema/schema.cpp
    handlers/shared/claims.cpp
    handlers/shared/terminal.cpp
    handlers/shared/recovery.cpp
    handlers/action/action.cpp
    handlers/dispatch/dispatch.cpp
    handlers/context/context.cpp
    handlers/ingest/ingest.cpp
    handlers/run/runs.cpp
    dispatch.cpp
  DEPENDS
    cmd_internal
    cliapp
    db
    git
    engine_runtime
    engine_routing
    json_text
    engine_planning
    engine_hostqueue)

# `--metadata` is validated with glz::validate_json -- the same purpose-built
# probe engine/config and engine/runs use, and the only one that enables
# Glaze's validate_trailing_whitespace (task 6086). PRIVATE: Glaze is an
# implementation detail of two flag validators, not part of any interface.
target_link_libraries(planar_cmd_planar_agent PRIVATE glaze::glaze)
if(TARGET planar_cmd_planar_agent_tests)
  target_link_libraries(planar_cmd_planar_agent_tests PRIVATE glaze::glaze SQLite::SQLite3)
endif()

if(TARGET planar_cmd_planar_agent_tests)
  # parity.t.cpp shells the built binary and pins its stdout, stderr and
  # exit code against expectations transcribed from the Zig oracle before
  # the M10 cutover deleted it (decisions 963/982). Only this binary's own
  # path is needed now; nothing here shells a second implementation.
  target_compile_definitions(planar_cmd_planar_agent_tests PRIVATE
    "PLANAR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_agent>\"")
  target_include_directories(planar_cmd_planar_agent_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_dependencies(planar_cmd_planar_agent_tests planar_cmd_planar_agent)
endif()

# Each binary builds its own CLI::App tree directly (plan 996, task 6123),
# so the CLI11 module target is linked per binary rather than laundered
# through a shared layer-1 wrapper.
target_link_libraries(planar_cmd_planar_agent PRIVATE CLI11::CLI11_Module)
if(TARGET planar_cmd_planar_agent_tests)
  target_link_libraries(planar_cmd_planar_agent_tests PRIVATE CLI11::CLI11_Module)
endif()
