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
#
# Parse failures (exit 1 — NOT the operator binary's 2) are covered across
# the set, so 0 and 1 are both produced by this binary and a mapping
# collapsed to one code cannot pass.
#
# --- NOT landed, each blocked at LAYER 2 -----------------------------------
#
# Sixteen of the oracle's eighteen verbs, and the blocker is the same bucket
# for most of them. See `tree.cppm`'s header for the per-verb inventory; the
# summary:
#
#   pull / peek / claim / heartbeat / claim-associate / complete / fail /
#   release / block / abort / reconcile / action
#                       need the agent_work_claims + agent_actions store
#                       (zig engine.runtime.agentactivity, 5,702 lines) and
#                       the atomic terminal-verb transactions. Not ported.
#   ingest              needs the vendor hook-event adapters. Not ported.
#   run start / run end write `workflow_runs` — a DIFFERENT table from the
#                       `runs` table the ported `engine_runs` bucket owns.
#                       Verified by reading the Zig handler's literal
#                       INSERT; reusing engine_runs here would write the
#                       wrong table.
#   dispatch / context  need the routing_dispatch_* and run-scoped context
#                       tables. Not ported.
#
# Omitting an unported child from a group node stays the rule (task 6106),
# so `planar-agent --help` lists two commands where the oracle lists
# eighteen and is not oracle-comparable; the LEAF pages still are.

planar_binary(planar_agent
  MAIN
    main.cpp
  INTERFACE
    exit.cppm
    context.cppm
    handler.cppm
    tree.cppm
    handlers/version.cppm
    handlers/schema.cppm
    dispatch.cppm
  SOURCES
    context.cpp
    tree.cpp
    handlers/version.cpp
    handlers/schema.cpp
    dispatch.cpp
  DEPENDS
    cli
    db)

if(TARGET planar_cmd_planar_agent_tests)
  # parity.t.cpp shells the built binary and diffs its stdout, stderr and
  # exit code against the Zig oracle's. Absent oracle => those cases SKIP
  # (D6: the zig/ tree is the oracle until M10).
  target_compile_definitions(planar_cmd_planar_agent_tests PRIVATE
    "PLANAR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_agent>\""
    "PLANAR_ZIG_BIN=\"${CMAKE_SOURCE_DIR}/zig/zig-out/bin/planar-agent\"")
  target_include_directories(planar_cmd_planar_agent_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_dependencies(planar_cmd_planar_agent_tests planar_cmd_planar_agent)
endif()
