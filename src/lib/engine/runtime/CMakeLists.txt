# planar_engine_runtime — engine/runtime bucket (plan 996, task 6094).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on
# layer-1 modules only, never sideways onto another `engine_*` bucket
# (D15/D18's same-layer prohibition for layers 2 and 3). This module needs
# `db` and the layer-1 `git` and `policy` seams (see below); no D19
# extraction was required for any of them and none was invented.
#
# Behavior-preserving port (D2) of zig/src/engine/runtime/{session,
# snapshot,capture}.zig plus the body-composition and rendering halves of
# zig/src/cmd/planar/handlers/capture/*.zig -- all seven `capture` schema
# leaves (session, end, note, command, file, snapshot, commits).
#
# `capture commits`, the seventh leaf, landed at task 6358 once the
# layer-1 `planar.git` seam (tasks 6128/6137) closed the process-spawn gap
# this bucket used to be blocked on. It rests on
# zig/src/engine/runtime/sessioncommits.zig's STRICT git-walk functions
# (`resolveRepoRootStrict`, `walkStrict`, `resolveShas`) plus the write
# half (`recordCount`). Task 6360 added the FAIL-SOFT `walk` variant and
# wired it into `capture end`'s automatic harvest (below) and into
# `planar-agent`'s terminal-verb claim-window fold (see the agent-plane
# section) -- see sessioncommits.cppm's header for exactly what each task
# landed.
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `capture.zig`'s `probeStartGitContext`: still handler-owned, not
#     engine-owned. `session::set_start_git_context_if_unset` is ported and
#     tested (including its write-once semantics), and `capture::open_session`
#     takes the probed context as an optional argument instead of probing
#     for it; `handlers/capture.cpp`'s `capture_session` supplies it via
#     `planar.git::probe_start_context` (task 6128).
#   - `capture.zig`'s `recordSessionWindow` -- `capture end`'s AUTOMATIC
#     commit capture, distinct from the `capture commits` MANUAL leaf task
#     6358 ported. WIRED at task 6360: `capture::close_session` now calls
#     the private `record_session_window` helper (capture.cpp) before
#     appending the "session ended" note, using `sessioncommits::walk`
#     (fail-soft) for the git side and surfacing a database failure as
#     `capture_error::query_failed`, matching the oracle's `try
#     recordSessionWindow(...)`. This closed a real, reachable divergence:
#     task 6128 landed the git probe that stamps `repo_root` /
#     `head_sha_at_start` on `capture session`, so a session opened inside
#     a repository carries them and `capture end` skipping the harvest was
#     observable, not moot.
#   - `policy.audit.record` calls in `startSession`, `endSession`,
#     `handoff::{create,validate,consume,abandon}`, and `snapshot::create`
#     landed in task 6191. Each is a separate statement after its primary
#     mutation, matching the Zig oracle's ordering and preserving NULL
#     actor/scope through the layer-1 writer.
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
#   - `sessioncommits.recordClaimWindowBestEffort` -- WIRED at task 6360.
#     The four `cmd_planar_agent` terminal handlers (`complete`/`fail`/
#     `release`/`block`, in `terminal.cpp`) call it AFTER their transaction
#     commits and BEFORE the shared envelope is emitted, matching the
#     oracle's call order. It is outside the terminal op's own transaction
#     in both the oracle and this port, so nothing atomic rests on it, and
#     a failure there is swallowed rather than surfaced.
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
#   - `policy.audit.record`: correct as-is. The claim surface is the one
#     part of this bucket with NO Zig call sites (agentatomic/agentactivity
#     write `agent_actions`, not `audit_log`), so nothing here is waiting
#     on the layer-1 `planar.policy` module task 6100 landed. The gap in
#     this bucket is session/handoff/snapshot, above.

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
#   - LANDED (task 6455): the 8-section RESUME PACKET (`resume.zig`'s
#     `buildPacket`, ~500 of its 994 lines) as
#     `resumecheck::build_packet` + `render_packet_json`/`render_packet_text`.
#     This note previously named FOUR blocking layer-2 surfaces; re-verified
#     by RUNNING the oracle, only ONE was genuinely missing.
#     `external.link::links_for_entity` had already landed (task 6106) and
#     all three of `engine.planning.{decision,question,artifact}` already
#     existed by the time this task started -- both claims in the prior
#     revision of this note were stale, not wrong-when-written. The
#     plan_step claim was never actually true: `plan_step.cppm`'s
#     `list_steps(conn, plan_id)` (ordered by ordinal) already existed;
#     only `plan.cppm` itself lacked a step read path, and the note
#     conflated the sibling module with its neighbor. Only
#     `session::recentEntriesForTask` was genuinely absent -- landed this
#     task as `session::recent_entries_for_task` alongside
#     `recent_sessions_for_task` (the audit-footer reader, which the prior
#     note did not even list as a dependency).
#
#     None of that changes the LAYER shape: `plan_step`, `external.link`
#     and `engine.planning.{decision,question,artifact}` remain sibling
#     layer-2 buckets, and importing any of them from `engine_runtime`
#     would still be the `engine_* -> engine_*` edge D15/D18 forbid.
#     `build_packet` sidesteps this the same way the oracle's OWN
#     `buildPlanPosition` / `buildOperationalPlane` / `buildDecisions` /
#     `buildQuestions` / `buildArtifacts` do: raw SQL against
#     `plan_steps` / `external_links` / `decisions` / `questions` /
#     `artifacts` directly, not a call into the sibling module. Only
#     `session`, `snapshot`, `agentactivity` and `handoff` are consulted
#     through real calls, because all four already live in THIS bucket.
#     See resumecheck.cppm's header for the full account, including why
#     `planar.engine.planning.decision`'s existing `decisions_for_task`
#     is deliberately NOT reused (different query, different semantics).
#   - `resolveWorktreeForTask`'s git side. The worktree fields it copies
#     come from the claim row, which IS readable here; nothing shells git.
#   - No `policy.audit.record` gap remains here: task 6191 wired
#     create/validate/consume/abandon (and the session/snapshot call sites
#     noted above) through the layer-1 `planar.policy` writer.

# --- the audit READ plane (plan 996, task 6090) ----------------------------
#
# `audit_trail` ports zig/src/engine/runtime/audit_trail.zig whole:
# `forEntity`, `forEntityWithLinks`, `forEntityGrep`, `sessionTimeline`.
# Before this it had NO C++ counterpart at any layer -- the write path
# (`src/lib/policy/audit.cppm`) existed, the read path did not, and the two
# are easy to conflate by name alone. See audit_trail.cppm's header.
#
# It lands in THIS bucket, not a new `engine_audit`, because
# `session_timeline` reads `sessions` / `session_entries` -- the tables
# `session.cppm` already owns. A separate bucket would need an
# `engine_* -> engine_*` edge for that, which D15/D18 forbid. The Zig tree
# files it as a sibling of session.zig under engine/runtime/ identically.
#
# Only ONE of its four functions was WIRED to a leaf in task 6090
# (`session_timeline` serves `audit session`). The other three were ported
# unwired rather than left out because the task body named the whole Zig
# module as the unit of work, and splitting one 240-line read module across
# two cycles would have left the second re-deriving the same four `select`
# shapes.
#
# UPDATE (task 6262): all four are now wired. `audit trail` landed, and it
# reaches the `external_links` / `sync_events` half through
# `engine_external`'s `link::show`, `system::show_by_id` and
# `sync::events_for_link` — the note this replaces said no module in this
# tree had that read path, which was true when task 6090 wrote it and had
# stopped being true by the time the leaf was ported. Check the module
# interfaces, not this file.
#
# --- the session-commit read AND strict-write path (plan 996, tasks 6262 --
# --- and 6358) ---------------------------------------------------------- -
#
# `sessioncommits` STARTED as a deliberate fraction of its 1205-line
# oracle (task 6262: `list_for_sessions`/`list_filtered`, pure SQL). Task
# 6358 added the STRICT git-walk slice `capture commits` needs
# (`resolve_repo_root_strict`, `walk_strict`, `resolve_shas`,
# `record_count`), now that `planar.git` (tasks 6128/6137) supplies the
# process-spawn seam this bucket used to lack. Task 6360 added the
# FAIL-SOFT `walk` (used by `capture end`'s automatic `recordSessionWindow`
# harvest and as the fallback leg of the claim-window fold) and
# `record_claim_window_best_effort` (the private `walkClaimWindow` folded
# in as an internal helper), wiring both `capture end` and the four
# `planar-agent` terminal verbs to the oracle's automatic commit capture.
#
# Still genuinely blocked: `bench harvest`, on functionality this module
# does not carry. `audit commits` needs none of this — it only reads rows
# an earlier walk already wrote (task 6272 corrected the earlier claim
# that it needed the git-walk slice; see sessioncommits.cppm's header).
# The module header says so at length, because the module NAME is the
# thing most likely to mislead a later reader.

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
    audit_trail.cppm
    sessioncommits.cppm
    contextrecords.cppm
    workflowruns.cppm
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
    audit_trail.cpp
    sessioncommits.cpp
    contextrecords.cpp
    workflowruns.cpp
  DEPENDS
    db
    git
    json_text
    policy
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
