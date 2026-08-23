# planar_cmd_planar — the `planar` operator binary (plan 996, task 6105).
#
# The FIRST layer-3 target in the C++ tree, and the first point at which a
# C++ binary runs a verb end to end. Milestones M0-M4 built engine buckets
# only: libraries and unit tests, no main(), no dispatch. Three consecutive
# cycles blocked leaves (`unlink`, `dashboard`, `report`, `skills`,
# `workspace init`, and the ingest apply/import/synthesize trio) on this
# layer's absence, because D20 (decision 947) puts a verb that composes
# several layer-2 peers at layer 3.
#
# ONE TARGET PER BINARY, and that is a constraint, not a preference. D18
# keeps the same-layer prohibition for `cmd_*` — "the one-handler-per-binary
# shape" — so this binary CANNOT be split into a `planar_cmd_planar_handlers`
# library plus a thin executable linking it: cmake/architecture.cmake would
# FATAL on the layer-3 -> layer-3 edge. planar_binary() grew INTERFACE, MAIN
# and a Catch2 test lane in this task precisely so a single target can still
# be written in module interface units and still have its handler layer under
# test (see that function's header comment for the full account; before this
# task planar_binary() silently ignored any *.t.cpp beside it).
#
# --- verb subset -----------------------------------------------------------
#
# Five leaves, chosen to exercise the mechanisms rather than to cover
# surface. Porting the remaining ~200 verbs is the milestones that follow.
#
#   version            The spine alone: parse -> dispatch -> stdout -> exit 0,
#                      no database, no engine bucket, no scope.
#   workflow list      The stdout terminator contract at its sharpest --
#                      `--json` emits NDJSON and ZERO BYTES on an empty
#                      catalog. Also the env-lookup seam.
#   workflow show      The non-zero exit path (exit 1) with a renderer that
#                      returns a COMPLETE stderr line, prefix included.
#   annotate add       The layer-3 composition D20 exists for: engine_identity
#                      (scope resolution) + engine_planning (annotation CRUD),
#                      two buckets that cannot reach each other. Also the
#                      lazy database actually opening and migrating.
#   annotate list      The counterexample to `workflow list --json`: a JSON
#                      renderer documented as returning NO terminator, so the
#                      caller appends one. The rule is per-renderer.
#
# Parse failures (exit 2) are covered across the set, and the exit-code
# mapping is pinned with 0, 1, 2 and 64 all produced by the same binary so a
# mapping collapsed to a single code cannot pass.
#
# --- task 6106: the leaves that were waiting on this layer ----------------
#
# Three more, drawn from the six this file's original header listed as
# blocked. They are NOT interchangeable, and the difference is the cycle's
# main finding:
#
#   unlink             Blocked TWICE. Layer 3 was missing AND so was its
#                      layer-2 bucket -- there was no engine_external in
#                      the tree at all. src/lib/engine/external/ was stood
#                      up here (create/show/delete only; see that
#                      directory's CMakeLists for what is deferred and with
#                      which verb). The leaf then composes engine_external
#                      + engine_runtime, two buckets the guard FATALs an
#                      edge between -- the second instance of the D20 shape
#                      `annotate add` proves, and the one this milestone
#                      was named for.
#   workspace doctor   Blocked ONLY on layer 3. engine_workspace (task
#                      6110) already carried the diagnose-and-repair pass
#                      and BOTH renderers, so the handler is nine lines of
#                      wiring. That thinness is evidence, not a shortcut.
#   skills             Retired upstream (plan 918 M5) and reduced to a
#                      placeholder node -- but a placeholder that this
#                      tree, unlike the Zig one, must bind to a handler,
#                      because `planar.cli.cmd` classifies a childless node
#                      as a LEAF and dispatch would otherwise answer exit
#                      64 against the oracle's help page and exit 0.
#
# NOT landed, each blocked at LAYER 2 rather than layer 3 -- which corrects
# the premise this file was written under. The dependency inventory is in
# task 6106's coder report; the summary:
#
#   dashboard                  needs engine.runtime.agentactivity (5,702
#                              lines across seven Zig files; claims,
#                              next-work, claim-scope resolution, the
#                              latest-action fold-in and its JSON writers).
#                              Not ported.
#   report                     needs engine.introspect AND
#                              engine.introspection_adapters. Neither is
#                              ported. Its OTHER reputed blocker -- that it
#                              scans the operator's home directory for
#                              vendor transcripts and is therefore not
#                              hermetically testable -- does not survive a
#                              probe: run under this file's own pinned
#                              arena, `report --json` reports every vendor
#                              as {"state":"unavailable","scanned":0} and
#                              cli_log as "disabled". The scan follows
#                              $HOME, which run_pinned() already redirects,
#                              so the leaf IS testable here exactly like
#                              every other one. The unported buckets are
#                              the whole of what blocks it.
#   spec ingest --apply        needs engine.ingestor.apply (1,616 lines)
#                              and engine.workbench (unported entirely).
#                              engine_ingest here is the READ side only.
#   import / synthesize        need engine.import / engine.synthesize +
#                              engine.llm. None ported.
#
# Omitting an unported child from a group node stays the rule (`workflow
# run`), so `planar workspace --help` lists one command where the oracle
# lists four and remains not oracle-comparable; the LEAF pages still are.
#
# --- terminator contract ---------------------------------------------------
#
# Inherited from commit 5728133 (the M4 boundary review's F1) and NOT
# relaxed here: a renderer that returns a COMPLETE payload returns the
# oracle's exact bytes INCLUDING the terminator, and this layer writes it
# verbatim and appends nothing. A renderer that returns a FRAGMENT carries no
# terminator and says so in its @return, and this layer composes it. Both
# shapes are live in this subset and both are pinned in handlers.t.cpp:
# `workflows::render::list_json` is complete (empty catalog -> zero bytes),
# `annotation::render_list_json` is a fragment (documented "with no trailing
# newline"). Reading each renderer's own @return is the rule; a blanket
# always-append or never-append breaks one of the two.
#
# Note this is also why `planar.cli.output`'s emit()/emit_list() are NOT used
# by any renderer-backed leaf here: those helpers unconditionally append '\n'
# after the Glaze payload, which predates the 5728133 contract and would
# break `workflow list --json`'s zero-byte case. They remain correct for a
# Glaze-reflected struct that has no engine renderer at all.
#
# --- HOME / DB safety ------------------------------------------------------
#
# `planar.cmd.planar.context`'s `process_env()` is the ONLY function in this
# target that calls std::getenv; everything else reads through a context's
# env-lookup callable, the same structural mechanism engine_workflows,
# engine_local and engine_workspace already use. That is what lets the test
# binary run real handlers against a scratch root with no process-environment
# mutation, and it is a direct response to commit 3ec6c37, where two cli
# parity tests shelled the reference binary with the inherited environment
# and opened the operator's live ~/.planar/planar.db on every ctest run.
#
# --- deferred, with their dependencies -------------------------------------
#
#   - The bare-invocation TTY cockpit gate (no cockpit in this tree).
#   - The worktree gate (needs a git-subprocess seam that does not exist --
#     the same dependency that deferred `bench harvest`, `capture commits`
#     and `workflow run`).
#   - cli_log invocation capture (no cli_log surface in this tree).
#   - policy.audit rows -- following the precedent every engine bucket
#     already documents; task 6100 is the open operator decision.

planar_binary(planar
  MAIN
    main.cpp
  INTERFACE
    exit.cppm
    context.cppm
    args.cppm
    handler.cppm
    scope.cppm
    tree.cppm
    handlers/version.cppm
    handlers/workflow.cppm
    handlers/annotate.cppm
    handlers/unlink.cppm
    handlers/skills.cppm
    handlers/workspace.cppm
    dispatch.cppm
  SOURCES
    context.cpp
    scope.cpp
    tree.cpp
    handlers/version.cpp
    handlers/workflow.cpp
    handlers/annotate.cpp
    handlers/unlink.cpp
    handlers/skills.cpp
    handlers/workspace.cpp
    dispatch.cpp
  DEPENDS
    cli
    db
    engine_external
    engine_identity
    engine_planning
    engine_runtime
    engine_workflows
    engine_workspace)

if(TARGET planar_cmd_planar_tests)
  # parity.t.cpp shells the built binary and diffs its stdout, stderr and
  # exit code against the Zig oracle's. It needs to know where both live:
  # PLANAR_CPP_BIN is this target's own output, PLANAR_ZIG_BIN the reference.
  # Absent oracle => those cases SKIP, the same posture db/migrate.t.cpp and
  # cli/help.t.cpp already take (D6: the zig/ tree is the oracle until M10).
  target_compile_definitions(planar_cmd_planar_tests PRIVATE
    "PLANAR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar>\""
    "PLANAR_ZIG_BIN=\"${CMAKE_SOURCE_DIR}/zig/zig-out/bin/planar\"")
  add_dependencies(planar_cmd_planar_tests planar_cmd_planar)
endif()
