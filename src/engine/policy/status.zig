//! Policy: per-entity-kind status transition rules.
//!
//! Each entity kind owns a set of legal status values and a transition
//! matrix saying which → which moves are allowed. This module is the
//! single source of truth for questions like "can a done task be
//! reopened?" — the CRUD function in `engine.planning.task.update`
//! calls `check(.task, current, desired)` and trusts the answer.
//!
//! The matrix is stubbed below. Each entity adds its rows here when
//! it lands (or in a sibling file `policy/status_<kind>.zig` if the
//! per-kind logic grows past a switch arm). The signature is locked in
//! now so callers can wire it from day one without conditional compile.

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

/// Validate a status transition. Currently a permissive stub: every
/// transition is accepted. Real matrix lands per entity — see the
/// `// TODO(status-matrix:<kind>):` markers per arm.
pub fn check(
    kind: EntityKind,
    from: []const u8,
    to: []const u8,
) Error!void {
    switch (kind) {
        .plan => {
            // TODO(status-matrix:plan): draft → active → {paused, done, cancelled}
            //   active → paused → active, etc.
        },
        .task => {
            // TODO(status-matrix:task): inbox/draft → active → {done, cancelled, blocked}
            //   done/cancelled → active only via `task reopen` verb (with --reason)
        },
        .question => {
            // TODO(status-matrix:question): open → {answered, wontfix}
            //   answered is reversible only via `question reopen` (no such verb yet)
        },
        .scenario => {
            // TODO(status-matrix:scenario): draft → verified → retired
        },
        .decision => {
            // TODO(status-matrix:decision): proposed → accepted → superseded|withdrawn
        },
        .artifact => {
            // TODO(status-matrix:artifact): draft → published; rare lifecycle
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
    _ = from;
    _ = to;
}

// ---- tests ----

test "stub accepts any transition (per-kind matrix lands per entity)" {
    try check(.task, "draft", "active");
    try check(.task, "done", "active");
    try check(.plan, "active", "done");
    try check(.handoff, "pending", "consumed");
}

test "annotation enum arm: stub accepts open and terminal transitions" {
    // active → each terminal state is the expected lifecycle path.
    try check(.annotation, "active", "resolved");
    try check(.annotation, "active", "dismissed");
    try check(.annotation, "active", "archived");
    // Terminal → anything: the stub is permissive; the real guard lives in
    // annotation.transition (isTerminal check). These pass through the policy
    // layer for now; once the matrix is implemented they should raise
    // IllegalTransition.
    try check(.annotation, "resolved", "active");
    try check(.annotation, "dismissed", "active");
    try check(.annotation, "archived", "active");
}
