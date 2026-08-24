# planar_engine_runtime — engine/runtime bucket (plan 996, task 6094).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on
# layer-1 modules only, never sideways onto another `engine_*` bucket
# (D15/D18's same-layer prohibition for layers 2 and 3). This module needs
# only `db`, so no D19 extraction was required and none was invented.
#
# Behavior-preserving port (D2) of zig/src/engine/runtime/{session,
# snapshot,capture}.zig plus the body-composition and rendering halves of
# zig/src/cmd/planar/handlers/capture/*.zig -- six of the seven `capture`
# schema leaves (session, end, note, command, file, snapshot).
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `capture commits`, the seventh leaf. It rests entirely on
#     zig/src/engine/runtime/sessioncommits.zig (1205 lines), whose whole
#     job is shelling `git` through `std.process.run` -- revision walks,
#     ref resolution, per-commit metadata. There is no process-spawn
#     abstraction in this tree, and building one inside a port task would
#     be a much larger change than the leaf it serves. Deferred WITH its
#     dependency.
#   - `capture.zig`'s `probeStartGitContext` and `recordSessionWindow`,
#     for the same reason. Their DB-visible halves are preserved:
#     `session::set_start_git_context_if_unset` is ported and tested
#     (including its write-once semantics), and `capture::open_session`
#     takes the probed context as an optional argument instead of probing
#     for it. `recordSessionWindow` is a no-op whenever `repo_root` or
#     `head_sha_at_start` is NULL, which is every session this tree can
#     currently open, so its absence changes nothing observable here.
#   - `policy.audit.record` calls in `startSession` / `endSession` /
#     `snapshot.create`. There is still no `policy.audit` module in the
#     C++ tree; `engine_planning/CMakeLists.txt`,
#     `engine_identity/association.cppm` and `engine_promotion` all
#     document the same omission. No `capture` leaf reads or emits audit
#     rows, so no observable CLI contract is affected.
#
# `runtime.cppm` is the umbrella re-export, matching
# `lib/engine/identity/identity.cppm`.
#
# STDOUT TERMINATOR CONTRACT (task 6114, resolving the M4 boundary review's
# F1). A renderer in this tree that returns a COMPLETE stdout payload returns
# the oracle's exact bytes INCLUDING the trailing newline; the layer-3 caller
# writes the string verbatim and appends nothing. A renderer that returns a
# FRAGMENT the caller composes into a larger line -- an error-message body
# that becomes `error: <body>` on stderr -- returns no terminator and its
# @return says so. The alternative shape ("renderer returns the body, caller
# appends the terminator") cannot express two payloads this tree actually has:
# a leaf whose empty output is ZERO BYTES rather than a bare newline
# (`workflow list --json`, `local list --json`), and NDJSON, where the newline
# is per-RECORD rather than per-document. Both are pinned. See
# `json_text/CMakeLists.txt` for the escaping half of the same contract.

# --- the agent plane (plan 996, task 6038) ---------------------------------
#
# `agentactivity` / `agentatomic` / `agentrender` port
# zig/src/engine/runtime/agentactivity/{types,store,atomic,json}.zig -- the
# `agent_work_claims` + `agent_actions` store, the atomic claim-ritual
# transactions, and the stdout payloads the claim verbs write.
#
# THEY LIVE IN THIS BUCKET, not a new one, because the claim store needs
# `session::ensure_active` to mint the `sessions` row every claim's
# `session_id` FK points at. A separate `engine_agent` bucket would have
# needed an `engine_* -> engine_*` edge for that, which D15/D18 forbid and
# cmake/architecture.cmake FATALs on. The Zig tree draws the same line:
# `engine/runtime/session.zig` and `engine/runtime/agentactivity/` are
# siblings under one bucket.
#
# TWO layer-2 dependencies could NOT be resolved that way, and are injected
# instead of imported (see agentatomic.cppm's header for the full argument):
#
#   policy.status.check      -> `planar.engine.planning.transitions`
#   plan.recomputeStatus     -> `planar.engine.planning.plan`
#
# Both are planning POLICY living in a sibling bucket. Duplicating them here
# would put two copies of the task transition matrix and the plan roll-up
# aggregate in the tree, free to drift; extracting them to layer 1 under D19
# would put planning policy in a base library, which is the wrong shape (D19
# is for shared PRIMITIVES -- `scope_ray`-style grammar, `json_text`-style
# escaping). So `agentatomic::task_policy` takes them as callables and
# `cmd_planar_agent` -- which is layer 3 and may depend on both buckets --
# supplies the real ones. The side benefit is that the property this
# milestone exists to prove ("the guard runs before the UPDATE, the roll-up
# runs inside the same transaction") becomes directly observable: a test
# passes a recording probe.
#
# NOT ported, named rather than dropped silently:
#
#   - `locality.probe`. Three `git` subprocess calls per claim
#     (`symbolic-ref` / `rev-parse` / `status --porcelain`). `locality` is an
#     INPUT to this bucket; the probe lives at layer 3, and what THAT port
#     captures and does not is documented in
#     src/cmd/planar-agent/locality.cppm. Same call `capture commits` and
#     `bench harvest` already made.
#   - `sessioncommits.recordClaimWindowBestEffort`, which the four terminal
#     handlers call AFTER their transaction. Same git-subprocess dependency,
#     and it is outside the transaction in the original, so nothing atomic
#     rests on it.
#   - `latestActiveClaimForSession`, `nextWork`,
#     `recordEntityCreateAction`. Read paths for `planar-watch`'s display
#     surfaces and for `planar` verbs, not for any `planar-agent` verb.
#     Deferred with the binaries that consume them.
#
#     CORRECTION (task 6040): this list previously also named
#     `resolveClaimScope`, `listActive`, `listStale`, `listByEntity` and
#     `listBySession` as unported. They LANDED in commit bedc18b, with the
#     `planar-watch` read paths, as `resolve_claim_scope`,
#     `list_active_claims_sorted`, `list_stale_claims`,
#     `list_claims_by_entity` and `list_claims_by_session`. The comment was
#     not updated with the code. `list_claims_by_entity` in particular is
#     what the `handoff` composite uses to copy worktree context off the
#     active claim, and a reader trusting this list would have re-ported it.
#     Check the module interface, not this note.
#   - `policy.audit.record`, still absent tree-wide (task 6100, an open
#     operator decision). No claim verb reads or emits audit rows.

# --- the agent-lifecycle operator surface (plan 996, task 6040) ------------
#
# `handoff` and `resumecheck` port zig/src/engine/runtime/handoff.zig and
# the `validate` third of zig/src/engine/runtime/resume.zig, together with
# the rendering halves of the `planar handoff *` and `planar resume
# validate` handlers.
#
# `handoff` needs the handoff STATUS MATRIX, which lives in
# `planar.engine.planning.transitions` -- a sibling layer-2 bucket. It is
# INJECTED as a `transition_check` callable rather than imported, for the
# reason agentatomic.cppm already gives at length: D15/D18 forbid the edge,
# duplicating the matrix here would create a second copy free to drift, and
# a layer-1 extraction under D19 would put planning policy in a base
# library. `cmd_planar` supplies the real one. The `.handoff` arm was added
# to that matrix in this task; its header had reserved it.
#
# NOT ported, named rather than dropped silently:
#
#   - The 8-section RESUME PACKET (`resume.zig`'s `buildPacket`, ~500 of
#     its 994 lines). Blocked at LAYER 2 on FOUR surfaces, none of them in
#     this bucket: the plan_step completed/current/remaining fold
#     (`engine.planning.plan` has no step read path), external_links listed
#     BY ENTITY with their remote state (`engine.external.link` is
#     create/show/remove only), `session::recentEntriesForTask` (this
#     bucket has `list_entries_for_session` only), and
#     `engine.planning.{decision,question,artifact}` -- three modules that
#     do not exist in `src/lib/engine/planning/` at all. The `resume`
#     leaf's oracle behavior is captured in
#     src/cmd/planar/handlers/resume.cppm so the port that lands it has the
#     bytes to hit.
#   - `resolveWorktreeForTask`'s git side. The worktree fields it copies
#     come from the claim row, which IS readable here; nothing shells git.
#   - `policy.audit.record` in create/validate/consume/abandon. Still no
#     `policy.audit` module tree-wide (task 6100). No handoff leaf reads or
#     emits audit rows.

planar_module(engine_runtime
  INTERFACE
    session.cppm
    snapshot.cppm
    capture.cppm
    agentactivity.cppm
    agentatomic.cppm
    agentrender.cppm
    handoff.cppm
    resumecheck.cppm
    runtime.cppm
  SOURCES
    session.cpp
    snapshot.cpp
    capture.cpp
    agentactivity.cpp
    agentatomic.cpp
    agentrender.cpp
    handoff.cpp
    resumecheck.cpp
  DEPENDS
    db
    json_text
  # TEST_DEPENDS, not DEPENDS: `handoff.t.cpp` imports
  # `planar.engine.planning.transitions` to exercise `handoff`'s injected
  # guard with the REAL matrix rather than a hand-written double, which is
  # the only way a test here can catch the two arms disagreeing. Putting it
  # in DEPENDS would link engine_planning into the PRODUCTION target and
  # create exactly the `engine_* -> engine_*` edge D15 forbids and
  # cmake/architecture.cmake FATALs on. The seam is the whole point: the
  # production module still reaches the matrix only through the callable
  # `cmd_planar` supplies.
  TEST_DEPENDS
    engine_planning)
