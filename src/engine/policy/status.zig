//! Policy: per-entity-kind status transition rules.
//!
//! Each entity kind owns a set of legal status values and a transition
//! matrix saying which → which moves are allowed. This module is the
//! single source of truth for questions like "can a done task be
//! reopened via bare update?" — the CRUD functions in the planning
//! modules call `check(.kind, current, desired, force)` and trust the
//! answer.
//!
//! Per-entity matrices:
//!
//!   task  {todo, doing, blocked, done, cancelled}
//!     todo     → {doing, blocked, cancelled}
//!     doing    → {todo, blocked, done, cancelled}
//!     blocked  → {doing, done, cancelled}
//!     done, cancelled → terminal for bare update; escape via `task reopen --reason`
//!     identity (from == to) → no-op early return
//!     force=true → bypasses the matrix (records task_reopens at the call site)
//!
//!   plan  {draft, active, paused, done, abandoned}
//!     draft    → active
//!     active   → {paused, done, abandoned}
//!     paused   → active
//!     done, abandoned → terminal
//!     identity (from == to) → no-op early return
//!     NOTE: plan.recomputeStatus deliberately bypasses this check (see plan.zig).
//!
//!   question  {open, answered, wontfix}
//!     open → {answered, wontfix}; answered, wontfix → terminal
//!
//!   scenario  {draft, ready, verified, failing, retired}
//!     draft    → {ready, retired}
//!     ready    → {verified, failing, retired}
//!     verified → {failing, retired}
//!     failing  → {verified, retired}
//!     retired  → terminal
//!     identity (from == to) → no-op early return
//!     NOTE: `scenario verify --outcome pass` on a `draft` scenario
//!     auto-walks draft → ready → verified internally (two policy-checked
//!     hops) so the operator workflow `scenario add → scenario verify`
//!     works without an explicit `scenario ready` step.  There is no
//!     `scenario ready` CLI verb.
//!
//!   decision  {proposed, accepted, superseded, withdrawn}
//!     proposed  → {accepted, superseded, withdrawn}
//!     accepted  → {superseded, withdrawn}
//!     superseded, withdrawn → terminal
//!     identity (from == to) → no-op early return
//!     NOTE: decision.validateTransition delegates to this arm.
//!
//!   artifact  {draft, active, superseded, retired}
//!     draft   → active
//!     active  → {draft, superseded, retired}
//!     superseded, retired → terminal
//!     identity (from == to) → no-op early return
//!     NOTE: artifact.validateTransition delegates to this arm.
//!
//!   handoff and annotation arms remain permissive stubs until their
//!   respective milestones land.  The signature is locked so callers can
//!   wire it from day one.

const std = @import("std");

pub const EntityKind = enum {
    plan,
    task,
    question,
    scenario,
    decision,
    artifact,
    handoff,
    annotation,
};

pub const Error = error{
    IllegalTransition,
    UnknownStatus,
};

/// Validate a status transition for the given entity kind.
///
/// Returns `error.IllegalTransition` when the move is not in the
/// legal edge set for `kind`.  Returns without error when `force` is
/// `true` (caller-level override) or when `from == to` (identity
/// no-op).
///
/// The `.task`, `.plan`, `.question`, `.scenario`, `.decision`, and
/// `.artifact` arms are fully enforced.  `.handoff` and `.annotation`
/// remain permissive stubs until their respective milestones land.
pub fn check(
    kind: EntityKind,
    from: []const u8,
    to: []const u8,
    force: bool,
) Error!void {
    // Identity transition is a no-op for every kind.
    if (std.mem.eql(u8, from, to)) return;

    switch (kind) {
        .plan => {
            // Real enforcement.  See module docstring for the full matrix.
            //
            // Status set: {draft, active, paused, done, abandoned}.
            //   draft    → active
            //   active   → {paused, done, abandoned}
            //   paused   → active
            //   done, abandoned → terminal (no outgoing operator edges)
            //
            // plan.recomputeStatus (plan.zig) deliberately bypasses this
            // check — it is an engine-internal aggregate roll-up whose
            // target is computed by computeTarget() and can only emit
            // transitions the aggregate matrix considers valid.  It is not
            // an operator transition and does not go through this validator.
            //
            // `plan update` has no --force flag, so force is always false
            // at the plan call site.
            const legal: bool = blk: {
                if (std.mem.eql(u8, from, "draft")) {
                    break :blk std.mem.eql(u8, to, "active");
                } else if (std.mem.eql(u8, from, "active")) {
                    break :blk std.mem.eql(u8, to, "paused") or
                        std.mem.eql(u8, to, "done") or
                        std.mem.eql(u8, to, "abandoned");
                } else if (std.mem.eql(u8, from, "paused")) {
                    break :blk std.mem.eql(u8, to, "active");
                } else if (std.mem.eql(u8, from, "done") or
                    std.mem.eql(u8, from, "abandoned"))
                {
                    // Terminal for operator transitions.
                    break :blk false;
                } else {
                    return Error.UnknownStatus;
                }
            };
            if (!legal) return Error.IllegalTransition;
        },
        .task => {
            // Real enforcement.  See module docstring for the full matrix.
            //
            // `force=true` bypasses the matrix — the call site records the
            // task_reopens audit row when the force move is a terminal→open
            // transition (source='task-update-force').
            if (force) return;

            // Legal edges by source status.
            const legal: bool = blk: {
                if (std.mem.eql(u8, from, "todo")) {
                    break :blk std.mem.eql(u8, to, "doing") or
                        std.mem.eql(u8, to, "blocked") or
                        std.mem.eql(u8, to, "cancelled");
                } else if (std.mem.eql(u8, from, "doing")) {
                    break :blk std.mem.eql(u8, to, "todo") or
                        std.mem.eql(u8, to, "blocked") or
                        std.mem.eql(u8, to, "done") or
                        std.mem.eql(u8, to, "cancelled");
                } else if (std.mem.eql(u8, from, "blocked")) {
                    break :blk std.mem.eql(u8, to, "doing") or
                        std.mem.eql(u8, to, "done") or
                        std.mem.eql(u8, to, "cancelled");
                } else if (std.mem.eql(u8, from, "done") or
                    std.mem.eql(u8, from, "cancelled"))
                {
                    // Terminal for bare update.  Only `task reopen --reason`
                    // (or `--force`) may leave this state; both bypass check
                    // before reaching here.
                    break :blk false;
                } else {
                    // Unknown source status.
                    return Error.UnknownStatus;
                }
            };
            if (!legal) return Error.IllegalTransition;
        },
        .question => {
            // Status set: {open, answered, wontfix}.
            // Legal transitions: open → {answered, wontfix} only.
            // Both `answered` and `wontfix` are terminal — there is
            // no `question reopen` verb. Terminal → anything raises
            // IllegalTransition.
            //
            // Plan 352 bug 2: M4 scenario surfaced that wontfix from
            // answered silently succeeded; the engine called this
            // check but the arm was a permissive TODO stub.
            if (std.mem.eql(u8, from, "open")) {
                if (std.mem.eql(u8, to, "answered") or std.mem.eql(u8, to, "wontfix")) return;
                return Error.IllegalTransition;
            }
            // from is terminal (answered or wontfix): refuse any move.
            return Error.IllegalTransition;
        },
        .scenario => {
            // Real enforcement.  See module docstring for the full matrix.
            //
            // Status set: {draft, ready, verified, failing, retired}.
            //   draft    → {ready, retired}
            //   ready    → {verified, failing, retired}
            //   verified → {failing, retired}
            //   failing  → {verified, retired}
            //   retired  → terminal (no outgoing operator edges)
            //
            // scenario verbs expose no --force flag, so force is always false
            // at scenario call sites.
            const legal: bool = blk: {
                if (std.mem.eql(u8, from, "draft")) {
                    break :blk std.mem.eql(u8, to, "ready") or
                        std.mem.eql(u8, to, "retired");
                } else if (std.mem.eql(u8, from, "ready")) {
                    break :blk std.mem.eql(u8, to, "verified") or
                        std.mem.eql(u8, to, "failing") or
                        std.mem.eql(u8, to, "retired");
                } else if (std.mem.eql(u8, from, "verified")) {
                    break :blk std.mem.eql(u8, to, "failing") or
                        std.mem.eql(u8, to, "retired");
                } else if (std.mem.eql(u8, from, "failing")) {
                    break :blk std.mem.eql(u8, to, "verified") or
                        std.mem.eql(u8, to, "retired");
                } else if (std.mem.eql(u8, from, "retired")) {
                    // Terminal for operator transitions.
                    break :blk false;
                } else {
                    return Error.UnknownStatus;
                }
            };
            if (!legal) return Error.IllegalTransition;
        },
        .decision => {
            // Real enforcement.  See module docstring for the full matrix.
            //
            // Status set: {proposed, accepted, superseded, withdrawn}.
            //   proposed  → {accepted, superseded, withdrawn}
            //   accepted  → {superseded, withdrawn}
            //   superseded, withdrawn → terminal (no outgoing operator edges)
            //
            // decision.validateTransition delegates to this arm and maps the
            // returned errors back to the module's existing error spelling
            // (TerminalStatus / InvalidStatus) so callers are unaffected.
            //
            // Decision verbs expose no --force flag; force is always false
            // at decision call sites.
            const legal: bool = blk: {
                if (std.mem.eql(u8, from, "proposed")) {
                    break :blk std.mem.eql(u8, to, "accepted") or
                        std.mem.eql(u8, to, "superseded") or
                        std.mem.eql(u8, to, "withdrawn");
                } else if (std.mem.eql(u8, from, "accepted")) {
                    break :blk std.mem.eql(u8, to, "superseded") or
                        std.mem.eql(u8, to, "withdrawn");
                } else if (std.mem.eql(u8, from, "superseded") or
                    std.mem.eql(u8, from, "withdrawn"))
                {
                    // Terminal for all operator transitions.
                    break :blk false;
                } else {
                    return Error.UnknownStatus;
                }
            };
            if (!legal) return Error.IllegalTransition;
        },
        .artifact => {
            // Real enforcement.  See module docstring for the full matrix.
            //
            // Status set: {draft, active, superseded, retired}.
            //   draft  → active
            //   active → {draft, superseded, retired}
            //   superseded, retired → terminal (no outgoing operator edges)
            //
            // artifact.validateTransition delegates to this arm and maps the
            // returned errors back to the module's existing error spelling
            // (IllegalTransition — same name) so callers are unaffected.
            //
            // Artifact verbs expose no --force flag; force is always false
            // at artifact call sites.
            const legal: bool = blk: {
                if (std.mem.eql(u8, from, "draft")) {
                    break :blk std.mem.eql(u8, to, "active");
                } else if (std.mem.eql(u8, from, "active")) {
                    break :blk std.mem.eql(u8, to, "draft") or
                        std.mem.eql(u8, to, "superseded") or
                        std.mem.eql(u8, to, "retired");
                } else if (std.mem.eql(u8, from, "superseded") or
                    std.mem.eql(u8, from, "retired"))
                {
                    // Terminal for all operator transitions.
                    break :blk false;
                } else {
                    return Error.UnknownStatus;
                }
            };
            if (!legal) return Error.IllegalTransition;
        },
        .handoff => {
            // TODO(status-matrix:handoff): pending → validated → consumed
            //   pending/validated → abandoned with --reason
        },
        .annotation => {
            // Status set: {active, resolved, dismissed, archived}.
            // Terminal set: {resolved, dismissed, archived} — only `active` is open.
            // Transitions: active → {resolved, dismissed, archived}. Terminal → anything
            // is blocked at the engine layer (annotation.transition checks isTerminal
            // before calling here). This stub accepts all transitions; the terminal
            // guard lives in annotation.zig and enforces the constraint.
            // TODO(status-matrix:annotation): raise IllegalTransition on terminal→open
            //   when engine.planning.annotation is refactored to rely solely on the
            //   policy layer instead of its own isTerminal guard.
        },
    }
}

// ---- tests ----

test "task arm: legal edges among active states are accepted" {
    try check(.task, "todo", "doing", false);
    try check(.task, "todo", "blocked", false);
    try check(.task, "todo", "cancelled", false);
    try check(.task, "doing", "todo", false);
    try check(.task, "doing", "blocked", false);
    try check(.task, "doing", "done", false);
    try check(.task, "doing", "cancelled", false);
    try check(.task, "blocked", "doing", false);
    try check(.task, "blocked", "done", false);
    try check(.task, "blocked", "cancelled", false);
}

test "task arm: terminal sources are refused by bare update" {
    try std.testing.expectError(error.IllegalTransition, check(.task, "done", "todo", false));
    try std.testing.expectError(error.IllegalTransition, check(.task, "done", "doing", false));
    try std.testing.expectError(error.IllegalTransition, check(.task, "done", "blocked", false));
    try std.testing.expectError(error.IllegalTransition, check(.task, "cancelled", "todo", false));
    try std.testing.expectError(error.IllegalTransition, check(.task, "cancelled", "doing", false));
    try std.testing.expectError(error.IllegalTransition, check(.task, "cancelled", "blocked", false));
}

test "task arm: illegal intermediate moves are refused" {
    // todo cannot jump straight to done without going through doing first
    // (not in the matrix)
    // Actually todo → done is NOT in the matrix per spec
    try std.testing.expectError(error.IllegalTransition, check(.task, "todo", "done", false));
}

test "task arm: identity transition is a no-op (not refused)" {
    try check(.task, "todo", "todo", false);
    try check(.task, "doing", "doing", false);
    try check(.task, "blocked", "blocked", false);
    try check(.task, "done", "done", false);
    try check(.task, "cancelled", "cancelled", false);
}

test "task arm: force=true bypasses the matrix for terminal sources" {
    // force=true lets the caller move out of terminal without refusal;
    // the call site records the task_reopens audit row.
    try check(.task, "done", "todo", true);
    try check(.task, "done", "doing", true);
    try check(.task, "done", "blocked", true);
    try check(.task, "cancelled", "todo", true);
    try check(.task, "cancelled", "doing", true);
}

test "task arm: force=true also bypasses non-terminal illegal moves" {
    // force is a full bypass — it doesn't discriminate.
    try check(.task, "todo", "done", true);
}

test "task arm: unknown status returns UnknownStatus" {
    try std.testing.expectError(error.UnknownStatus, check(.task, "inbox", "doing", false));
}

test "plan arm: legal edges are accepted" {
    // draft → active
    try check(.plan, "draft", "active", false);
    // active → {paused, done, abandoned}
    try check(.plan, "active", "paused", false);
    try check(.plan, "active", "done", false);
    try check(.plan, "active", "abandoned", false);
    // paused → active
    try check(.plan, "paused", "active", false);
}

test "plan arm: skip-ahead moves are refused" {
    // draft → done/abandoned/paused (skipping active) are all illegal.
    try std.testing.expectError(error.IllegalTransition, check(.plan, "draft", "done", false));
    try std.testing.expectError(error.IllegalTransition, check(.plan, "draft", "abandoned", false));
    try std.testing.expectError(error.IllegalTransition, check(.plan, "draft", "paused", false));
}

test "plan arm: terminal sources are refused" {
    // done → anything is illegal.
    try std.testing.expectError(error.IllegalTransition, check(.plan, "done", "draft", false));
    try std.testing.expectError(error.IllegalTransition, check(.plan, "done", "active", false));
    try std.testing.expectError(error.IllegalTransition, check(.plan, "done", "paused", false));
    // abandoned → anything is illegal.
    try std.testing.expectError(error.IllegalTransition, check(.plan, "abandoned", "draft", false));
    try std.testing.expectError(error.IllegalTransition, check(.plan, "abandoned", "active", false));
}

test "plan arm: identity transition is a no-op" {
    try check(.plan, "draft", "draft", false);
    try check(.plan, "active", "active", false);
    try check(.plan, "paused", "paused", false);
    try check(.plan, "done", "done", false);
    try check(.plan, "abandoned", "abandoned", false);
}

test "plan arm: unknown status returns UnknownStatus" {
    try std.testing.expectError(error.UnknownStatus, check(.plan, "cancelled", "active", false));
}

test "question arm: open to answered/wontfix accepted" {
    try check(.question, "open", "answered", false);
    try check(.question, "open", "wontfix", false);
}

test "question arm: terminal sources refused" {
    try std.testing.expectError(error.IllegalTransition, check(.question, "answered", "open", false));
    try std.testing.expectError(error.IllegalTransition, check(.question, "wontfix", "open", false));
    try std.testing.expectError(error.IllegalTransition, check(.question, "answered", "wontfix", false));
}

test "question arm: identity no-op" {
    try check(.question, "open", "open", false);
    try check(.question, "answered", "answered", false);
}

test "scenario arm: legal edges are accepted" {
    // draft → {ready, retired}
    try check(.scenario, "draft", "ready", false);
    try check(.scenario, "draft", "retired", false);
    // ready → {verified, failing, retired}
    try check(.scenario, "ready", "verified", false);
    try check(.scenario, "ready", "failing", false);
    try check(.scenario, "ready", "retired", false);
    // verified → {failing, retired}
    try check(.scenario, "verified", "failing", false);
    try check(.scenario, "verified", "retired", false);
    // failing → {verified, retired}
    try check(.scenario, "failing", "verified", false);
    try check(.scenario, "failing", "retired", false);
}

test "scenario arm: illegal edges are refused" {
    // draft cannot jump directly to verified or failing
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "draft", "verified", false));
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "draft", "failing", false));
    // ready cannot go back to draft
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "ready", "draft", false));
    // verified cannot go back to ready or draft
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "verified", "ready", false));
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "verified", "draft", false));
    // failing cannot go back to ready or draft
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "failing", "ready", false));
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "failing", "draft", false));
}

test "scenario arm: retired is terminal — all moves refused" {
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "retired", "draft", false));
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "retired", "ready", false));
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "retired", "verified", false));
    try std.testing.expectError(error.IllegalTransition, check(.scenario, "retired", "failing", false));
}

test "scenario arm: identity transition is a no-op" {
    try check(.scenario, "draft", "draft", false);
    try check(.scenario, "ready", "ready", false);
    try check(.scenario, "verified", "verified", false);
    try check(.scenario, "failing", "failing", false);
    try check(.scenario, "retired", "retired", false);
}

test "scenario arm: unknown status returns UnknownStatus" {
    try std.testing.expectError(error.UnknownStatus, check(.scenario, "active", "verified", false));
    try std.testing.expectError(error.UnknownStatus, check(.scenario, "published", "retired", false));
}

test "annotation enum arm: stub accepts open and terminal transitions" {
    // active → each terminal state is the expected lifecycle path.
    try check(.annotation, "active", "resolved", false);
    try check(.annotation, "active", "dismissed", false);
    try check(.annotation, "active", "archived", false);
    // Terminal → anything: the stub is permissive; the real guard lives in
    // annotation.transition (isTerminal check). These pass through the policy
    // layer for now; once the matrix is implemented they should raise
    // IllegalTransition.
    try check(.annotation, "resolved", "active", false);
    try check(.annotation, "dismissed", "active", false);
    try check(.annotation, "archived", "active", false);
}

// ---- decision arm tests ----

test "decision arm: legal edges are accepted" {
    // proposed → {accepted, superseded, withdrawn}
    try check(.decision, "proposed", "accepted", false);
    try check(.decision, "proposed", "superseded", false);
    try check(.decision, "proposed", "withdrawn", false);
    // accepted → {superseded, withdrawn}
    try check(.decision, "accepted", "superseded", false);
    try check(.decision, "accepted", "withdrawn", false);
}

test "decision arm: terminal sources are refused" {
    // superseded is terminal — no outgoing edges.
    try std.testing.expectError(error.IllegalTransition, check(.decision, "superseded", "proposed", false));
    try std.testing.expectError(error.IllegalTransition, check(.decision, "superseded", "accepted", false));
    try std.testing.expectError(error.IllegalTransition, check(.decision, "superseded", "withdrawn", false));
    // withdrawn is terminal — no outgoing edges.
    try std.testing.expectError(error.IllegalTransition, check(.decision, "withdrawn", "proposed", false));
    try std.testing.expectError(error.IllegalTransition, check(.decision, "withdrawn", "accepted", false));
    try std.testing.expectError(error.IllegalTransition, check(.decision, "withdrawn", "superseded", false));
}

test "decision arm: illegal non-terminal moves are refused" {
    // accepted cannot go back to proposed.
    try std.testing.expectError(error.IllegalTransition, check(.decision, "accepted", "proposed", false));
}

test "decision arm: identity transition is a no-op" {
    try check(.decision, "proposed", "proposed", false);
    try check(.decision, "accepted", "accepted", false);
    try check(.decision, "superseded", "superseded", false);
    try check(.decision, "withdrawn", "withdrawn", false);
}

test "decision arm: unknown status returns UnknownStatus" {
    try std.testing.expectError(error.UnknownStatus, check(.decision, "open", "accepted", false));
    try std.testing.expectError(error.UnknownStatus, check(.decision, "draft", "proposed", false));
}

// ---- artifact arm tests ----

test "artifact arm: legal edges are accepted" {
    // draft → active
    try check(.artifact, "draft", "active", false);
    // active → {draft, superseded, retired}
    try check(.artifact, "active", "draft", false);
    try check(.artifact, "active", "superseded", false);
    try check(.artifact, "active", "retired", false);
}

test "artifact arm: terminal sources are refused" {
    // superseded is terminal — no outgoing edges.
    try std.testing.expectError(error.IllegalTransition, check(.artifact, "superseded", "draft", false));
    try std.testing.expectError(error.IllegalTransition, check(.artifact, "superseded", "active", false));
    try std.testing.expectError(error.IllegalTransition, check(.artifact, "superseded", "retired", false));
    // retired is terminal — no outgoing edges.
    try std.testing.expectError(error.IllegalTransition, check(.artifact, "retired", "draft", false));
    try std.testing.expectError(error.IllegalTransition, check(.artifact, "retired", "active", false));
    try std.testing.expectError(error.IllegalTransition, check(.artifact, "retired", "superseded", false));
}

test "artifact arm: illegal non-terminal moves are refused" {
    // draft cannot jump to superseded or retired directly.
    try std.testing.expectError(error.IllegalTransition, check(.artifact, "draft", "superseded", false));
    try std.testing.expectError(error.IllegalTransition, check(.artifact, "draft", "retired", false));
}

test "artifact arm: identity transition is a no-op" {
    try check(.artifact, "draft", "draft", false);
    try check(.artifact, "active", "active", false);
    try check(.artifact, "superseded", "superseded", false);
    try check(.artifact, "retired", "retired", false);
}

test "artifact arm: unknown status returns UnknownStatus" {
    try std.testing.expectError(error.UnknownStatus, check(.artifact, "published", "active", false));
    try std.testing.expectError(error.UnknownStatus, check(.artifact, "open", "draft", false));
}
