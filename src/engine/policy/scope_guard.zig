//! Policy: refuse mutations that cross scope boundaries.
//!
//! Mirrors the rule documented in docs/concepts.md#cross-scope-guard.
//! Every guarded data-plane mutation runs `check(entity.scope, write_scope)`
//! before touching the DB. The matrix:
//!
//!   entity_scope = null  → entity is global; any write is allowed.
//!   write_scope  = null  → resolver couldn't pin a scope; refuse to
//!                          mutate any entity that has one.
//!   else                 → must match exactly.
//!
//! Some verbs (entity_links add/remove, task touches add/remove) are
//! deliberately unguarded — they create cross-scope edges by design.
//! Those callers simply skip this check; the rule lives in the caller's
//! file, not here.

const std = @import("std");

/// Scope = nullable association slug. `null` means global / no scope.
/// Redeclared locally rather than imported from identity/scope.zig so
/// policy stays a leaf module with no engine-internal deps.
pub const Scope = ?[]const u8;

pub const Error = error{ScopeMismatch};

pub fn check(entity_scope: Scope, write_scope: Scope) Error!void {
    if (entity_scope == null) return;
    if (write_scope == null) return error.ScopeMismatch;
    if (!std.mem.eql(u8, entity_scope.?, write_scope.?)) return error.ScopeMismatch;
}

// ---- tests ----

test "allows write on a global entity regardless of write scope" {
    try check(null, null);
    try check(null, "acme");
}

test "refuses write when entity has a scope but resolver does not" {
    try std.testing.expectError(error.ScopeMismatch, check("acme", null));
}

test "refuses cross-scope writes" {
    try std.testing.expectError(error.ScopeMismatch, check("acme", "beta"));
}

test "allows matching scope" {
    try check("acme", "acme");
}
