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
#
# --- task 6041: three of the six `ext` leaves -----------------------------
#
#   ext register jira      Both are thin over `engine_external`'s system
#   ext register github    surface (task 6041), which is why they land
#                          together: the difference between them is not
#                          layering but DEFAULTS. `register github` treats
#                          `--auth-env` as OPTIONAL and its absence selects
#                          `gh-cli` auth with the literal ref `default`,
#                          while `register jira` requires all three flags.
#                          A tree that marked github's `--auth-env` required
#                          would refuse a legitimate registration, so the
#                          required-ness is pinned in catalog parity.
#   ext list               Two renderers with three traps between them: the
#                          JSON form is ONE OBJECT PER LINE (not an array),
#                          it emits ZERO BYTES on an empty database where
#                          the text form says "no external systems
#                          registered", and it OMITS a null `base_url` /
#                          `default_project` rather than emitting `null`.
#                          All three are oracle-captured.
#
# The other three `ext` leaves are NOT here and each names what it waits on:
# `ext test` on the adapter FACTORY (auth-env resolution plus the `gh auth
# token` subprocess arm, whose five refusal messages are its whole observable
# surface), and `ext create` / `ext propagate-one` / `ext propagate` on the
# create/propagate half of `engine_extsync`. The `sync` family
# (`pull`/`push`/`status`/`resolve`) is likewise absent at THIS layer even
# though its whole engine landed in task 6041: those handlers resolve an
# entity ref through verbs (`task`, `plan`, `link`) that this binary does not
# expose yet, so a ported `sync pull` would have no way to reach a link.
#   workspace doctor   Blocked ONLY on layer 3. engine_workspace (task
#                      6110) already carried the diagnose-and-repair pass
#                      and BOTH renderers, so the handler is nine lines of
#                      wiring. That thinness is evidence, not a shortcut.
#   skills             Retired upstream (plan 918 M5) and reduced to a
#                      placeholder node -- but a placeholder that this
#                      tree, unlike the Zig one, must bind to a handler,
#                      because `cliapp::leaf_keys` classifies a childless node
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
# --- task 6040: the agent-lifecycle operator verbs -------------------------
#
# Fifteen more leaves plus one placeholder: the six ported `capture` leaves,
# all seven `handoff` leaves, `resume validate`, and a deliberately loud
# `resume` stub.
#
# THREE MECHANISMS THIS CYCLE ADDED, each forced by a shape the tree had not
# met before:
#
#   dual group-and-leaf nodes   `handoff` and `resume` carry subcommands AND
#                               their own handler; the oracle dispatches the
#                               parent when no child is named. `dispatch`'s
#                               rule narrowed from "a matched node with
#                               children renders help" to "...and the table
#                               has no entry for it". `unreachable_handlers`
#                               widened to walk every node, not only leaves.
#   parent-level flags          `handoff` is the first group whose PARENT
#                               carries flags. CLI11 does not inherit them
#                               at parse time, so each child redeclares them
#                               — and `planar.cliapp.schema` had to learn to
#                               emit an inherited/local overlap ONCE.
#                               `fallthrough()` was tried first and is
#                               unusable: it bleeds the parent's POSITIONAL
#                               onto every child and, where parent and child
#                               share a positional name (`resume` /
#                               `resume validate`, both `task-id`), throws
#                               `OptionAlreadyAdded` while the tree is being
#                               BUILT, aborting every invocation of the
#                               binary.
#   injected status matrix      `handoff`'s transitions are guarded by
#                               `engine_planning`'s matrix, injected as a
#                               callable because D15/D18 forbid the
#                               layer-2 edge. Same seam
#                               `agentatomic::task_policy` uses.
#
# NOT landed, with the oracle behavior captured so the port that lands them
# has bytes to hit:
#
#   resume (the packet)  Blocked at LAYER 2 on four unported surfaces; the
#                        inventory and the captured payload are in
#                        src/cmd/planar/handlers/resume.cppm and
#                        src/lib/engine/runtime/CMakeLists.txt. Registered
#                        as a `not_implemented` placeholder (exit 64) rather
#                        than left unwired, because an unwired DUAL node
#                        would fall to the help path and exit 0 — a silent
#                        success where the oracle produces a packet.
#   capture commits      Task 6099. 1205 lines of git shelling, no
#                        process-spawn seam. Deferred WITH its dependency.
#   feedback triage      Three leaves, blocked at layer 2 on
#                        `engine.planning.feedback_triage` (302 lines), which
#                        has no counterpart in `src/lib/engine/planning/`.
#                        Oracle-captured against a pinned scratch arena:
#                          feedback triage list --json  -> exit 0, `[]\n`
#                                                          (TERMINATED, unlike
#                                                          `handoff list --json`)
#                          feedback triage list         -> exit 0,
#                                                          `(no feedback triage)\n`
#                          feedback triage show 1 --json -> exit 2,
#                            `error: finding must be task:<id> or question:<id>`
#                        Note the `<finding>` positional is an ENTITY REF,
#                        not an integer — a port that declares it as an int
#                        will get the exit code and the message wrong.
#   report               Task 6121. Needs the two introspect buckets. Its
#                        full `--json` envelope was re-captured this cycle
#                        and is unchanged from task 6106's finding: every
#                        vendor reports `{"state":"unavailable","scanned":0}`
#                        and `cli_log` reports `disabled` under a pinned
#                        arena, so the leaf IS hermetically testable here;
#                        the unported buckets are the whole of what blocks it.
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
# Note this is also why the deleted `planar.cli.output`'s emit()/emit_list()
# were never used
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
#
# --- task 6189: three engine-complete families, wired ---------------------
#
# Seven leaves, and the reason they landed together is that all three of
# their buckets were finished at LAYER 2 and had never been called. Task
# 6109 (engine_local) and task 6095 (engine_closure, engine_grouping) each
# shipped the whole read/write path AND byte-exact renderers for every
# render mode and every refusal line. What was missing was a caller.
#
#   local list/link/unlink/import/migrate
#                      The whole family. It opens NO database -- the durable
#                      state is two JSON manifests plus the vendor symlink
#                      trees -- so it is the counterexample that keeps
#                      `context::db_opened()` meaningful across a five-leaf
#                      family rather than a single leaf. It is also the
#                      family with THREE different JSON envelope conventions
#                      (NDJSON, NDJSON-in-a-`result`-wrapper, and one object
#                      per run) and two different zero-byte empty cases, all
#                      of which the renderers own and this layer writes
#                      verbatim.
#   closure show       The read half. `closure compute` STAYS a declared
#                      exit-64 refusal: its extractor is 1637 lines of
#                      tree-sitter AST work over a filesystem corpus walk,
#                      tree-sitter is not vendored here, and its handler
#                      would additionally compose engine_planning +
#                      engine_identity. Same shape as `bench harvest` and
#                      `models resolve` -- a family with one ported leaf and
#                      one refusal, named rather than silently absent.
#   groups recommend   The whole `groups` family, read-only. `--solver
#                      mtkahypar` is ACCEPTED rather than refused, because
#                      the degradation is REPORTED (`solver:"greedy"`,
#                      `optimal_available:false`) and is byte-identical to
#                      what the oracle produces on a host with no solver
#                      binary. An unknown `--solver` value is still exit 2,
#                      so the flag is validated rather than ignored.
#
# The three DEPENDS added here -- engine_local, engine_closure,
# engine_grouping -- are all layer-2 and none of the three handlers composes
# two buckets, so no leaf in this batch needed the D20 shape.
#
# ORACLE PARITY NOTE, recorded because it is host-dependent and will mislead
# whoever runs the differential next: `groups recommend --solver mtkahypar`
# is DELIBERATELY absent from parity.t.cpp. MtKaHyPar is installed on the
# machine this port was derived on, and there the oracle really does run it
# and return a different partition (one slice of three tasks versus greedy's
# two). Pinning either side would make the case pass or fail on what happens
# to be on the host. The degradation itself is pinned against this binary
# alone in closure_groups_leaves.t.cpp.

planar_binary(planar
  MAIN
    main.cpp
  INTERFACE
    exit.cppm
    context.cppm
    handler.cppm
    scope.cppm
    tree.cppm
    surface.cppm
    handlers/version.cppm
    handlers/catalog.cppm
    handlers/workflow.cppm
    handlers/annotate.cppm
    handlers/ext.cppm
    handlers/unlink.cppm
    handlers/skills.cppm
    handlers/workspace.cppm
    handlers/workbench.cppm
    handlers/capture.cppm
    handlers/init.cppm
    handlers/models.cppm
    handlers/plan.cppm
    handlers/assoc.cppm
    handlers/task.cppm
    handlers/question.cppm
    handlers/decision.cppm
    handlers/links.cppm
    handlers/handoff.cppm
    handlers/resume.cppm
    handlers/runs.cppm
    handlers/local.cppm
    handlers/closure.cppm
    handlers/groups.cppm
    verb_classification.cppm
    worktree_gate.cppm
    cli_log.cppm
    dispatch.cppm
  SOURCES
    context.cpp
    cli_log.cpp
    scope.cpp
    verb_classification.cpp
    worktree_gate.cpp
    tree.cpp
    surface.cpp
    handlers/version.cpp
    handlers/catalog.cpp
    handlers/workflow.cpp
    handlers/annotate.cpp
    handlers/ext.cpp
    handlers/unlink.cpp
    handlers/skills.cpp
    handlers/workspace.cpp
    handlers/workbench.cpp
    handlers/capture.cpp
    handlers/init.cpp
    handlers/models.cpp
    handlers/plan.cpp
    handlers/assoc.cpp
    handlers/task.cpp
    handlers/question.cpp
    handlers/decision.cpp
    handlers/links.cpp
    handlers/handoff.cpp
    handlers/resume.cpp
    handlers/runs.cpp
    handlers/local.cpp
    handlers/closure.cpp
    handlers/groups.cpp
    dispatch.cpp
  DEPENDS
    cliapp
    db
    git
    json_text
    engine_closure
    engine_config
    engine_entitylink
    engine_external
    engine_grouping
    engine_identity
    engine_local
    engine_models
    engine_planning
    engine_runs
    engine_runtime
    engine_workbench
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

# Each binary builds its own CLI::App tree directly (plan 996, task 6123),
# so the CLI11 module target is linked per binary rather than laundered
# through a shared layer-1 wrapper.
target_link_libraries(planar_cmd_planar PRIVATE CLI11::CLI11_Module)
if(TARGET planar_cmd_planar_tests)
  target_link_libraries(planar_cmd_planar_tests PRIVATE CLI11::CLI11_Module)
  # `../catalog_parity.hpp` — the declaration-level oracle comparison that
  # replaced the deleted help-page byte diffs (plan 996, task 6123). A plain
  # header, included rather than linked, for the reason parity_harness.hpp's
  # own header gives: D18 forbids a `cmd_* -> cmd_*` edge and a shared
  # layer-1 library for test scaffolding would be worse. It parses JSON with
  # Glaze, hence the link.
  target_include_directories(planar_cmd_planar_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(planar_cmd_planar_tests PRIVATE glaze::glaze)
endif()
