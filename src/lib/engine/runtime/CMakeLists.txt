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
# half (`recordCount`) -- see sessioncommits.cppm's header for exactly what
# landed and what is still deferred (the FAIL-SOFT `walk` variant and the
# claim-window fold neither `capture commits` nor any ported verb reaches).
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
#     6358 ported. STILL UNWIRED, and the justification this paragraph
#     used to carry ("a no-op whenever `repo_root`/`head_sha_at_start` is
#     NULL, which is every session this tree can currently open") is now
#     STALE and is corrected here rather than silently dropped: task 6128
#     landed the git probe that stamps both columns on `capture session`,
#     so a session opened inside a repository DOES carry them, and
#     `capture end` on it is a real, reachable divergence from the oracle
#     (no automatic commit rows) rather than an unreachable one. task 6358
#     ports the git-walk PRIMITIVES `recordSessionWindow` would need
#     (`walk_strict` in `sessioncommits.cppm`; `recordSessionWindow` itself
#     calls the FAIL-SOFT `walk`, still absent -- see that header) but
#     deliberately does NOT wire them into `capture_end`, which is a
#     separate leaf outside this task's claimed scope. Tracked for a
#     follow-up task rather than fixed here.
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
# process-spawn seam this bucket used to lack.
#
# STILL DEFERRED — the FAIL-SOFT `walk` (used only by `recordSessionWindow`,
# `capture end`'s automatic capture; see the paragraph above), and
# `walkClaimWindow` / `recordClaimWindowBestEffort` (the `planar-agent`
# terminal verbs' claim-window fold). Nothing in this task's diff touches
# `audit commits` or `bench harvest`; `audit commits` was already ported
# (task 6272 corrected the earlier claim that it needed this git-walk
# slice — it never did, see sessioncommits.cppm's header) and
# `bench harvest` remains genuinely blocked on functionality this module
# still does not carry. The module header says so at length, because the
# module NAME is the thing most likely to mislead a later reader.

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
