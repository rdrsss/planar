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
#                      the tree at all. src/engine/external/ was stood
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
# `ext test` WAS here as a deferral and is now ported — see task 6258's
# section at the end of this file. The other three `ext` leaves are still
# absent and each names what it waits on: `ext create` / `ext propagate-one` /
# `ext propagate` on the create/propagate half of `engine_extsync`. The `sync`
# family
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
#   report                     LANDED at task 6352, once `engine_introspect`
#                              (task 6121) and `engine_introspection_adapters`
#                              (tasks 6102, 6352) were both complete AND the
#                              layer-1 `introspection_preview` extraction
#                              (decision 981) let the two meet through
#                              `bundle::preview` without an `engine_* ->
#                              engine_*` edge. Its OTHER reputed blocker --
#                              that it scans the operator's home directory
#                              for vendor transcripts and is therefore not
#                              hermetically testable -- did not survive a
#                              probe: run under this file's own pinned
#                              arena, `report --json` reports every vendor
#                              as {"state":"unavailable","scanned":0} and
#                              cli_log as "disabled" (or "unavailable" when
#                              logging is on but the DB adapter has nothing
#                              to report). The scan follows $HOME, which
#                              run_pinned() already redirects, so the leaf
#                              was testable here exactly like every other
#                              one all along. See handlers/report/command.cppm.
#   spec ingest --apply        needs engine.ingestor.apply (1,616 lines)
#                              and engine.workbench (unported entirely).
#                              engine_ingest here is the READ side only.
#   import / synthesize        LANDED at task 6106 after the layer-2 importer
#                              and synthesize staging engines were ported.
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
#   feedback triage      LANDED at task 6303, with
#                        `engine.planning.feedback_triage` (302 Zig lines)
#                        underneath it. What this family cost was the
#                        FIXTURE, not the code: `findingPlan` compares the
#                        finding's plan slug against the literal
#                        `planar-feedback`, so a plain plan makes every
#                        interesting arm refuse with `DifferentFeedbackPlan`
#                        for the same uninteresting reason. A usable arena
#                        needs an association (a bare registered project
#                        refuses `plan create` at exit 5), a plan created
#                        with that exact slug, and — for the QUESTION arm —
#                        a `derives-from` entity link, which `question add
#                        --plan` writes for you. `AmbiguousFeedbackPlan` is
#                        reachable ONLY through a question carrying two such
#                        links, and is unreachable for a task at all.
#                        Oracle-captured empty states, unchanged by the port:
#                          feedback triage list --json  -> exit 0, `[]\n`
#                          feedback triage list         -> exit 0,
#                                                          `(no feedback triage)\n`
#                          feedback triage show 1 --json -> exit 2,
#                            `error: finding must be task:<id> or question:<id>`
#                        The `<finding>` positional is an ENTITY REF, not an
#                        integer, and the id inside it takes Zig's `parseInt`
#                        spelling: `task:+2` and `task:1_0` both parse,
#                        `task:2_` does not.
#
# NOT landed, with the oracle behavior captured so the port that lands them
# has bytes to hit:
#
#   resume (the packet)  Blocked at LAYER 2 on four unported surfaces; the
#                        inventory and the captured payload are in
#                        src/cmd/planar/handlers/resume/command.cppm and
#                        src/engine/runtime/CMakeLists.txt. Registered
#                        as a `not_implemented` placeholder (exit 64) rather
#                        than left unwired, because an unwired DUAL node
#                        would fall to the help path and exit 0 — a silent
#                        success where the oracle produces a packet.
#   capture commits      Task 6099. 1205 lines of git shelling, no
#                        process-spawn seam. Deferred WITH its dependency.
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
#   - (RESOLVED) The worktree gate. It needed a git-subprocess seam; `git`
#     (tasks 6128/6137) landed one and `worktree_gate.cpp` now calls
#     `git::detect_worktree`. This bullet said "does not exist" long after
#     it did.
#   - (RESOLVED for ONE of the three named here) `workflow run`, wired at
#     task 6272 over the new layer-1 `process` seam. The other two are NOT
#     spawn-blocked and never were, which is why they are still absent:
#     `bench harvest` needs ~671 unported engine lines (`runs/harvest.zig`)
#     and `capture commits` ~1100 (the git-walk half of
#     `runtime/sessioncommits.zig`). The seam was a necessary condition for
#     all three and a sufficient one for exactly one — the same shape task
#     6258 hit when the adapter factory unblocked one leaf of a predicted
#     eight. See src/lib/process/CMakeLists.txt.
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
#                      `optimal_available:false`); master ships no solver arm
#                      (decision 1293). An unknown `--solver` value is still exit 2,
#                      so the flag is validated rather than ignored.
#
# The three DEPENDS added here -- engine_local, engine_closure,
# engine_grouping -- are all layer-2 and none of the three handlers composes
# two buckets, so no leaf in this batch needed the D20 shape.
#
# `groups recommend --solver mtkahypar` is absent from parity.t.cpp. The
# accept-and-degrade behavior is pinned against this binary alone in
# closure_groups_leaves.t.cpp; the solver arm lives on branch
# `dev/grouping-solvers` (decision 1293).

# --- task 6258: the adapter factory, and ONE leaf ------------------------
#
# `ext test`, plus the auth-resolution unit it was deferred on
# (handlers/shared/ext_adapter_factory.cppm). Four DEPENDS are new — `adapter`,
# `http`, `engine_extsync` and the already-present `engine_external` — and
# the factory is the reason: it composes the bucket that IMPLEMENTS adapters
# with the bucket that stores the ROW an adapter is built from, and D18
# FATALs that edge inside layer 2. Layer 3 is the first place both may be
# named, which is where the Zig original also sits. This was checked against
# cmake/architecture.cmake BEFORE the module was written, so unlike task 6190
# it cost no mid-cycle redesign.
#
# WHAT THE FACTORY DID NOT UNBLOCK, contrary to the task's own premise. The
# task body predicted eight leaves across two families. It is ONE:
#
#   ext create / propagate-one / propagate
#                      Blocked on the create/propagate half of
#                      engine_extsync, which is a SEPARATE absence from the
#                      factory and is unchanged by this cycle.
#   sync pull / push / resolve
#                      Blocked on there being no `sync` handler module at
#                      this layer at all, plus the entity-ref resolution
#                      this file's earlier note already describes. The
#                      factory is necessary for them and nowhere near
#                      sufficient.
#
# Recorded because "the factory unblocks eight leaves" is the estimate that
# justified scheduling this task, and it was wrong in a way only visible
# after the factory existed.
#
# `ext test` REACHES NO NETWORK, which is why it needs no fixture server:
# the oracle's own handler comment says it does not probe the remote, and
# its `probeOk` only asks whether construction returned an adapter. So the
# leaf is fully testable in-process, and the `gh-cli` arm is covered by
# injecting a `token_command` rather than by depending on a `gh` binary
# being installed on the test machine.

# TASK 6278 added `tree` -- the hierarchical drill-down verb, and the
# largest single unblocked leaf left in the inventory (1284 non-test Zig
# lines). Its engine is the new layer-2 `engine_tree`; see
# src/engine/tree/CMakeLists.txt for what was ported, what was
# deliberately not, and why the agent-activity rollup is duplicated there
# rather than reached sideways into `engine_runtime`.
#
# Root command assembly now lives in `main.cppm`; `handlers/tree/` owns only
# the `planar tree` verb. The declaration was moved there from the old root
# builder without changing the CLI surface.
#
# WHAT THE ORACLE RUN FOUND, and none of it transfers from a sibling verb:
#   - `--sort` is DECLARED AND INERT. tree.zig stores `Filter.sort` and
#     never reads it; every query orders by id. `--sort updated` and
#     `--sort bogus` are byte-identical to a bare `tree` and neither
#     refuses. Reproduced deliberately (accept, ignore) and pinned in
#     tree_leaf.t.cpp so a future "fix" trips a test. It is a real Planar
#     defect and needs its own task against the oracle.
#   - EMPTY FLAG VALUES MEAN THREE DIFFERENT THINGS on this one verb:
#     `--kind ''` refuses at exit 2, `--status ''` matches nothing at exit
#     0, and `--scope ''` means GLOBAL at exit 0.
#   - The two refusals do NOT share an exit code: an unknown `--kind` is 2,
#     an unknown `--scope` is 1.
#   - `--depth 0` is UNBOUNDED, identical to `--depth -1`; the oracle's
#     guard is `max_depth <= 0`.
#   - `--kind <any derived kind>` renders an EMPTY tree. The scaffold rule
#     keeps a filtered-out plan only for a plan-or-task descendant, so
#     artifact/decision/scenario/question filters all collapse. Captured
#     for all four rather than generalised from one.
#
# `tree --help` is NOT oracle-comparable and no test claims it is -- CLI11
# renders help here (see main.cppm). The declaration is compared instead,
# via parity.t.cpp's `schema` catalog case, which already covered this leaf.
#
# TASK 6299 added `promote`, `demote` and `test-spec status` (handlers/
# promotion.{cppm,cpp} and handlers/test_spec.{cppm,cpp}), and its FIRST
# deliverable was a probe rather than a port: ten leaves that had never been
# examined this milestone, measured by SYMBOL against this tree. The split it
# found is the reason only three moved.
#
# WHAT MOVED, and why it was cheap: nothing about these three was inferrable
# from their names. `planar.engine.promotion` (task 6094) and
# `planar.engine.planning.test_spec_status` had each been ported in FULL --
# mutations, lookups AND output renderers -- long before any handler existed
# to call them, so all three leaves are lookup/compute/write wiring. The only
# new build edge is `engine_promotion` in DEPENDS below.
#
# WHAT THE ORACLE RUN FOUND, none of it transferable from a sibling:
#   - THE TWO BAD-KIND REFUSALS DO NOT SHARE A BUCKET. `promote bogus:1`
#     exits 2 (`parse_ref` does not know the kind) and `promote session:1`
#     exits 1 with `reading entity scope: InvalidScope` (`session` IS an
#     `entity_kind`, so it survives `parse_ref` and is refused by the
#     pre-read). Collapsing promotability into one check would have merged
#     them.
#   - `plan:0` and `plan:-1` are MALFORMED, not absent: exit 2 invalid-ref,
#     not the exit 1 `NotFound` a well-formed absent id gets.
#   - `demote --from` is parsed and DISCARDED. A slug that does not exist is
#     not a refusal; the engine call takes no source scope.
#   - `test-spec status` on a REAL MILESTONE reports `plan '2' not found`.
#     Both lookups require `parent_plan_id is null`, so a valid non-anchor
#     plan is indistinguishable from an absent one. Reproduced as-is.
#   - The success messages carry a DOUBLE space before `(was:`, and the text
#     table's counts carry an explicit `+` sign. Both come from the engine's
#     own renderers and were captured there.
#
# WHAT DID NOT MOVE, and it is the more useful half of the cycle. Seven of
# the ten stayed, each for a measured reason, and the per-leaf findings are
# recorded in dispatch.t.cpp beside the count assertion. The two worth
# repeating here because they contradict a plausible reading:
#   - "the workbench engine is fully ported, so its three remaining leaves
#     are free" is HALF TRUE. `workbench edit` and `extract-questions` need
#     nothing missing; `workbench publish` calls
#     `engine.extsync.parent_issue.recordLink`, and that module is listed
#     unported in src/engine/extsync/CMakeLists.txt.
#   - `link` is NOT engine-blocked -- `external::link::create` and
#     `external::system::show_by_slug` are both here. It is blocked on one
#     FLAG: `--propagate` runs `ext propagate`. The oracle's own
#     handlers/link.zig header claims that flag "refuses with NotImplemented
#     when set"; that comment is STALE and the code propagates. Captured:
#     `link plan:1 --to jira-demo:DEMO-2 --propagate --json` writes the link
#     row, emits a second propagation envelope, and exits 2. Needs a task
#     against the oracle for the stale comment.

planar_binary(planar
  MAIN
    main.cpp
  INTERFACE
    exit.cppm
    context.cppm
    database.cppm
    handler.cppm
    scope.cppm
    main.cppm
    handlers/explore/command.cppm
    handlers/help/command.cppm
    declare.cppm
    surface.cppm
    docs.cppm
    handlers/version/command.cppm
    handlers/shared/catalog.cppm
    handlers/schema/command.cppm
    handlers/completion/command.cppm
    handlers/workflow/command.cppm
    handlers/workflow/run.cppm
    handlers/workflow/show.cppm
    handlers/workflow/list.cppm
    handlers/annotate/command.cppm
    handlers/annotate/receipt.cppm
    handlers/annotate/request_command.cppm
    handlers/annotate/sweep.cppm
    handlers/annotate/verify.cppm
    handlers/annotate/bulk_archive.cppm
    handlers/annotate/bulk_dismiss.cppm
    handlers/annotate/bulk_resolve.cppm
    handlers/annotate/archive.cppm
    handlers/annotate/dismiss.cppm
    handlers/annotate/resolve.cppm
    handlers/annotate/tag.cppm
    handlers/annotate/remove.cppm
    handlers/annotate/update.cppm
    handlers/annotate/capabilities.cppm
    handlers/annotate/list.cppm
    handlers/annotate/show.cppm
    handlers/annotate/add.cppm
    handlers/shared/ext_adapter_factory.cppm
    # `link` (singular) writes `external_links` and is `unlink`'s exact
    # counterpart; `links` (plural, above) writes `entity_links`. The two
    # spellings mirror the CLI's own `planar link` vs `planar links add`.
    handlers/link/command.cppm
    handlers/unlink/command.cppm
    handlers/skills/command.cppm
    handlers/workspace/command.cppm
    handlers/workspace/routing.cppm
    handlers/workspace/regenerate.cppm
    handlers/workspace/doctor.cppm
    handlers/workspace/init.cppm
    handlers/workspace/routing_show.cppm
    handlers/workspace/routing_build.cppm
    handlers/workbench/command.cppm
    handlers/workbench/edit.cppm
    handlers/workbench/extract_questions.cppm
    handlers/workbench/publish.cppm
    handlers/workbench/list.cppm
    handlers/workbench/gc.cppm
    handlers/workbench/restore.cppm
    handlers/workbench/archive.cppm
    handlers/workbench/sync.cppm
    handlers/workbench/resolve.cppm
    handlers/workbench/status.cppm
    handlers/workbench/push.cppm
    handlers/workbench/pull.cppm
    handlers/workbench/lint.cppm
    handlers/capture/command.cppm
    handlers/capture/snapshot.cppm
    handlers/capture/file.cppm
    handlers/capture/request_command.cppm
    handlers/capture/note.cppm
    handlers/capture/end.cppm
    handlers/capture/commits.cppm
    handlers/capture/session.cppm
    handlers/init/command.cppm
    handlers/models/command.cppm
    handlers/models/registry.cppm
    handlers/models/outcomes.cppm
    handlers/models/experiments.cppm
    handlers/models/resolve.cppm
    handlers/models/evals.cppm
    handlers/models/registry_export.cppm
    handlers/models/registry_verify_identity.cppm
    handlers/models/registry_eligibility.cppm
    handlers/models/registry_observe.cppm
    handlers/models/registry_unbind.cppm
    handlers/models/registry_bind.cppm
    handlers/models/registry_remove.cppm
    handlers/models/registry_update.cppm
    handlers/models/registry_add.cppm
    handlers/models/registry_list.cppm
    handlers/plan/command.cppm
    handlers/plan/descendants.cppm
    handlers/plan/step.cppm
    handlers/plan/closeout.cppm
    handlers/plan/recompute_status.cppm
    handlers/plan/divergence.cppm
    handlers/plan/recommend_strategy.cppm
    handlers/plan/next.cppm
    handlers/plan/link.cppm
    handlers/plan/review.cppm
    handlers/plan/diff.cppm
    handlers/plan/view.cppm
    handlers/plan/edit.cppm
    handlers/plan/update.cppm
    handlers/plan/list.cppm
    handlers/plan/show.cppm
    handlers/plan/create.cppm
    handlers/plan/step_link.cppm
    handlers/plan/step_skip.cppm
    handlers/plan/step_done.cppm
    handlers/plan/step_list.cppm
    handlers/plan/step_add.cppm
    handlers/assoc/command.cppm
    handlers/assoc/detect.cppm
    handlers/assoc/members.cppm
    handlers/assoc/remove.cppm
    handlers/assoc/add.cppm
    handlers/assoc/create.cppm
    handlers/assoc/list.cppm
    handlers/scope/command.cppm
    handlers/scope/clear.cppm
    handlers/scope/pop.cppm
    handlers/scope/use.cppm
    handlers/scope/suggest.cppm
    handlers/scope/show.cppm
    handlers/search/command.cppm
    handlers/tree/command.cppm
    handlers/shared/promotion.cppm
    handlers/demote/command.cppm
    handlers/promote/command.cppm
    handlers/test_spec/command.cppm
    handlers/test_spec/status.cppm
    handlers/health/command.cppm
    handlers/health/hygiene.cppm
    handlers/audit/command.cppm
    handlers/audit/handoff_readiness.cppm
    handlers/audit/publish_decision.cppm
    handlers/audit/session.cppm
    handlers/audit/commits.cppm
    handlers/audit/trail.cppm
    handlers/dashboard/command.cppm
    handlers/report/command.cppm
    handlers/task/command.cppm
    handlers/task/facts_stage.cppm
    handlers/task/facts.cppm
    handlers/task/touches.cppm
    handlers/task/reopen.cppm
    handlers/task/link.cppm
    handlers/task/block.cppm
    handlers/task/cancel.cppm
    handlers/task/done.cppm
    handlers/task/review.cppm
    handlers/task/diff.cppm
    handlers/task/view.cppm
    handlers/task/edit.cppm
    handlers/task/update.cppm
    handlers/task/list.cppm
    handlers/task/packet.cppm
    handlers/task/show.cppm
    handlers/task/add.cppm
    handlers/task/touches_remove.cppm
    handlers/task/touches_list.cppm
    handlers/task/touches_infer.cppm
    handlers/task/touches_add.cppm
    handlers/feedback/command.cppm
    handlers/feedback/triage_set.cppm
    handlers/feedback/triage_show.cppm
    handlers/feedback/triage_list.cppm
    handlers/feedback/triage.cppm
    handlers/question/command.cppm
    handlers/question/link.cppm
    handlers/question/show.cppm
    handlers/question/list.cppm
    handlers/question/wontfix.cppm
    handlers/question/answer.cppm
    handlers/question/review.cppm
    handlers/question/diff.cppm
    handlers/question/view.cppm
    handlers/question/edit.cppm
    handlers/question/add.cppm
    handlers/decision/command.cppm
    handlers/decision/link.cppm
    handlers/decision/review.cppm
    handlers/decision/diff.cppm
    handlers/decision/view.cppm
    handlers/decision/edit.cppm
    handlers/decision/withdraw.cppm
    handlers/decision/supersede.cppm
    handlers/decision/accept.cppm
    handlers/decision/list.cppm
    handlers/decision/show.cppm
    handlers/decision/add.cppm
    handlers/scenario/command.cppm
    handlers/scenario/link.cppm
    handlers/scenario/show.cppm
    handlers/scenario/list.cppm
    handlers/scenario/retire.cppm
    handlers/scenario/verify.cppm
    handlers/scenario/review.cppm
    handlers/scenario/diff.cppm
    handlers/scenario/view.cppm
    handlers/scenario/edit.cppm
    handlers/scenario/add.cppm
    handlers/artifact/command.cppm
    handlers/artifact/link.cppm
    handlers/artifact/review.cppm
    handlers/artifact/diff.cppm
    handlers/artifact/view.cppm
    handlers/artifact/edit.cppm
    handlers/artifact/update.cppm
    handlers/artifact/list.cppm
    handlers/artifact/show.cppm
    handlers/artifact/add.cppm
    handlers/document/command.cppm
    editor.cppm
    editflow.cppm
    handlers/shared/drafting.cppm
    handlers/shared/atvalue.cppm
    handlers/links/command.cppm
    handlers/links/trail.cppm
    handlers/links/remove.cppm
    handlers/links/list.cppm
    handlers/links/add.cppm
    handlers/handoff/command.cppm
    handlers/handoff/show.cppm
    handlers/handoff/list.cppm
    handlers/handoff/abandon.cppm
    handlers/handoff/consume.cppm
    handlers/handoff/validate.cppm
    handlers/handoff/create.cppm
    handlers/resume/command.cppm
    handlers/resume/validate.cppm
    handlers/shared/runs.cppm
    handlers/run/command.cppm
    handlers/run/show.cppm
    handlers/run/finish.cppm
    handlers/run/event.cppm
    handlers/run/start.cppm
    handlers/bench/command.cppm
    handlers/bench/show.cppm
    handlers/bench/finish.cppm
    handlers/bench/harvest.cppm
    handlers/bench/touch.cppm
    handlers/bench/event.cppm
    handlers/bench/start.cppm
    handlers/local/command.cppm
    handlers/local/migrate.cppm
    handlers/local/import.cppm
    handlers/local/unlink.cppm
    handlers/local/link.cppm
    handlers/local/list.cppm
    handlers/templates/command.cppm
    handlers/templates/path.cppm
    handlers/templates/init.cppm
    handlers/templates/validate.cppm
    handlers/templates/render.cppm
    handlers/templates/show.cppm
    handlers/templates/list.cppm
    handlers/config/command.cppm
    handlers/config/path.cppm
    handlers/config/init.cppm
    handlers/config/validate.cppm
    handlers/config/edit.cppm
    handlers/config/show.cppm
    handlers/closure/command.cppm
    handlers/closure/show.cppm
    handlers/closure/compute.cppm
    handlers/groups/command.cppm
    handlers/groups/recommend.cppm
    handlers/spec/command.cppm
    handlers/spec/ingest.cppm
    handlers/import/command.cppm
    handlers/synthesize/command.cppm
    verb_classification.cppm
    worktree_gate.cppm
    cli_log.cppm
    dispatch.cppm
  SOURCES
    context.cpp
    database.cpp
    cli_log.cpp
    scope.cpp
    verb_classification.cpp
    worktree_gate.cpp
    declare.cpp
    handlers/version/command.cpp
    handlers/shared/catalog.cpp
    handlers/workflow/command.cpp
    handlers/annotate/command.cpp
    handlers/shared/ext_adapter_factory.cpp
    handlers/link/command.cpp
    handlers/unlink/command.cpp
    handlers/skills/command.cpp
    handlers/workspace/command.cpp
    handlers/workbench/command.cpp
    handlers/capture/command.cpp
    handlers/init/command.cpp
    handlers/models/command.cpp
    handlers/plan/command.cpp
    handlers/assoc/command.cpp
    handlers/scope/command.cpp
    handlers/search/command.cpp
    handlers/tree/command.cpp
    handlers/shared/promotion.cpp
    handlers/test_spec/command.cpp
    handlers/health/command.cpp
    handlers/audit/command.cpp
    handlers/dashboard/command.cpp
    handlers/report/command.cpp
    handlers/task/command.cpp
    handlers/feedback/command.cpp
    handlers/question/command.cpp
    handlers/decision/command.cpp
    handlers/scenario/command.cpp
    handlers/artifact/command.cpp
    handlers/document/command.cpp
    editor.cpp
    editflow.cpp
    handlers/shared/drafting.cpp
    handlers/shared/atvalue.cpp
    handlers/links/command.cpp
    handlers/handoff/command.cpp
    handlers/resume/command.cpp
    handlers/shared/runs.cpp
    handlers/local/command.cpp
    handlers/templates/command.cpp
    handlers/config/command.cpp
    handlers/closure/command.cpp
    handlers/groups/command.cpp
    handlers/spec/command.cpp
    handlers/import/command.cpp
    handlers/synthesize/command.cpp
    dispatch.cpp
  DEPENDS
    cmd_internal
    adapter
    cliapp
    db
    git
    process
    http
    json_text
    # New at task 6357, for `planar health` (handlers/health/command.cpp): the
    # `resolve_homes`/`is_::status` composition folds the layer-1 classifier
    # in directly, the same shape `report` composes its two engine buckets
    # with (see the `engine_introspect`/`engine_introspection_adapters`
    # note below).
    installed_surface
    engine_closure
    engine_config
    engine_entitylink
    engine_external
    engine_extsync
    engine_grouping
    engine_identity
    # New at task 6352, for `report` (handlers/report/command.cpp): the DB-aggregate
    # half (`engine_introspect`) and the filesystem-discovery half
    # (`engine_introspection_adapters`) it composes. Both were already
    # ported (tasks 6121, 6102) and are D15-forbidden from depending on
    # each other; this binary is where they are legally allowed to meet.
    engine_introspect
    engine_introspection_adapters
    # New at task 6324, for `task packet` (handlers/task/command.cpp). The leaf uses
    # exactly one entry point — `packet::assemble_task` plus its two
    # renderers. It does NOT reach the spec-ingest read pipeline; see
    # src/engine/ingest/CMakeLists.txt for why the routing packet shares
    # that bucket instead of getting an `engine_routing` one of its own.
    engine_ingest
    engine_importer
    engine_synthesize
    engine_local
    engine_health
    engine_models
    engine_planning
    document_authority
    # New at task 6299, for handlers/shared/promotion.cpp. `promote` and `demote`
    # are the binary's only consumers of this bucket, and it carries their
    # output renderers as well as the two mutations — see its own header for
    # why those renderers live in the engine rather than here.
    engine_promotion
    engine_runs
    engine_runtime
    engine_search
    engine_tree
    engine_templates
    engine_workbench
    engine_workflows
    engine_workspace)

if(TARGET planar_cmd_planar_tests)
  # parity.t.cpp shells the built binary and pins its stdout, stderr and
  # exit code against expectations transcribed from the Zig oracle before
  # the M10 cutover deleted it (decisions 963/982), so only this target's
  # own output path is needed. PLANAR_TARGET_SOURCE_ROOT is separate and
  # still live: unported_inventory.t.cpp reads the CHECKED-IN
  # `src/cmd/planar/surface.cppm` and `src/cmd/planar-agent/surface.cppm`
  # files' `k_unported` inventories by text scan, and must resolve those
  # against this checkout rather than the process's cwd. (`planar-watch`'s
  # is checked by a direct call instead — see the `target_sources` block
  # below, task 6626.)
  # (`.cppm`, not `.cpp`: plan 1051 M11 folded every generated `node_spec`
  # table away and left each binary with a single-file `surface.cppm` --
  # `planar-watch` at task 6613, `planar-agent` at 6614, `planar` at 6636.
  # `scripts/gen-cli-surface.py`, the generator that used to feed those
  # tables, was deleted at task 6616 once no binary had one left to fill.)
  target_compile_definitions(planar_cmd_planar_tests PRIVATE
    "PLANAR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar>\""
    "PLANAR_TARGET_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}\"")
  add_dependencies(planar_cmd_planar_tests planar_cmd_planar)

  # Task 6626: `unported_inventory.t.cpp` checks `planar-watch`'s unported
  # inventory with a DIRECT CALL to `planar::cmd::watch::unported_paths()`
  # rather than the text scan the other two binaries still get, because
  # that module is self-contained (`import std;` only — see its own
  # header) and safe to compile a second time into this test-only target.
  # This is a test-target-local edge, not a shipped `cmd_planar ->
  # cmd_planar_watch` dependency: D15's walk does not police `_tests`
  # targets (see `planar_binary()`'s own comment on why), and no
  # `planar_cmd_planar_watch` library is linked — the source is recompiled
  # standalone, the same way this test target already recompiles
  # `planar_cmd_planar`'s own INTERFACE list instead of linking it.
  target_sources(planar_cmd_planar_tests PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_SOURCE_DIR}/src/cmd/planar-watch"
    FILES "${CMAKE_SOURCE_DIR}/src/cmd/planar-watch/surface.cppm")
endif()

# Each binary builds its own CLI::App tree directly (plan 996, task 6123),
# so the CLI11 module target is linked per binary rather than laundered
# through a shared layer-1 wrapper.
target_link_libraries(planar_cmd_planar PRIVATE CLI11::CLI11_Module)
# Glaze, new at task 6295 for `ext create`. That leaf POSTs on the RAW
# transport rather than through the `external_adapter` interface — the
# four-operation interface has no create — so it has to read the provider's
# response itself (Jira's `key`, GitHub's `number` / `html_url`). Every
# other JSON parse in this binary's layer is Glaze's too; `planar.json_text`
# is emit-only and deliberately stays that way.
target_link_libraries(planar_cmd_planar PRIVATE glaze::glaze)
if(TARGET planar_cmd_planar_tests)
  target_link_libraries(planar_cmd_planar_tests PRIVATE CLI11::CLI11_Module)
  # `../catalog_parity.hpp` — the declaration-level oracle comparison that
  # replaced the deleted help-page byte diffs (plan 996, task 6123). A plain
  # header, included rather than linked, for the reason parity_harness.hpp's
  # own header gives: D18 forbids a `cmd_* -> cmd_*` edge and a shared
  # layer-1 library for test scaffolding would be worse. It parses JSON with
  # Glaze, hence the link.
  # Family tests moved under handlers/<family>/ still share the root's
  # test-only JSON envelope helper.
  target_include_directories(planar_cmd_planar_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(planar_cmd_planar_tests PRIVATE glaze::glaze)
endif()

# The counter-reset log helper (plan 1089, task qp-migration; tech spec 656
# § Counter resets). `scripts/queue-logs-after-reset.py` is what
# migrations/README.md § Host-queue rollback recovery tells an operator to
# run after a rollback, restore or re-init rewinds `queue_entries`'
# AUTOINCREMENT counter. A plain `add_test`, not a Catch2 case: the helper is
# a Python script, and its runner builds every fixture in a scratch
# directory with Python's own `sqlite3` module. Python is already a build
# dependency (the Python test runners registered at configure; INSTALL.md § Prerequisites).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME queue_logs_after_reset_helper
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/queue-logs-after-reset.test.py"
    "${PROJECT_SOURCE_DIR}/scripts/queue-logs-after-reset.py")
set_tests_properties(queue_logs_after_reset_helper PROPERTIES LABELS "queue;scripts")

# The installer's agent.db retirement reader (plan 1089, task
# qp-install-retire; tech spec 656 "Store reader commands").
# `scripts/install-lib/queue_retire.py` is what install.sh runs to judge the
# retired store's queue entries before removing it; its suite starts and
# reaps its own sleeping children. Registered here, not only in
# install-manifest-test.sh, so the Linux gate runs its /proc cases too.
add_test(NAME queue_retire_reader
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/install-lib/queue_retire.test.py"
    "${PROJECT_SOURCE_DIR}/scripts/install-lib/queue_retire.py")
set_tests_properties(queue_retire_reader PROPERTIES LABELS "queue;scripts")
# `install_queue.t.cpp`'s `queue_retire_live_oracle` runs the same reader.
if(TARGET planar_cmd_planar_tests)
  target_compile_definitions(planar_cmd_planar_tests PRIVATE "PLANAR_PYTHON3=\"${Python3_EXECUTABLE}\"")
endif()
