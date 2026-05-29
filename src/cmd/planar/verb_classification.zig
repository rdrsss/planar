//! verb_classification — central worktree-scope policy for `planar`.
//!
//! Plan 297 M3 forbids "planning" verbs from running inside a git
//! worktree (per `agents/methodology.md § Scope inside worktrees` and
//! the canonical tech-spec table). This module is the single source of
//! truth for the per-verb planning-vs-execution classification consulted
//! by the runtime gate.
//!
//! Design notes:
//!
//! - **Centralized, not per-handler-tag.** Adding a verb in a separate
//!   PR shouldn't require remembering to set a tag on the verb's
//!   `cli.Cmd`; centralizing in one file makes the policy auditable in
//!   one place and trips reviewers' eyes when a new verb lands.
//! - **Defaults to planning (refused).** Unknown / unmapped verbs land
//!   in the `planning` bucket. Worst case for a misclassified read verb
//!   is operator-recoverable ("cd to the parent checkout and re-run").
//!   The opposite default (allow unknowns) would silently let new
//!   planning verbs through the gate.
//! - **Prefix matching on path tokens.** The cli's `path` slice is
//!   already a `[]const []const u8` of resolved subcommand names; we
//!   match on it directly, no string manipulation.

const std = @import("std");

/// Two policy buckets. The runtime gate refuses `planning` from inside
/// a worktree; `execution_or_read` is always allowed.
pub const Class = enum {
    /// Refused from worktree cwd. Encompasses planning entity mutations
    /// AND `task done` (coders must use `planar-agent complete`).
    planning,
    /// Allowed from worktree cwd. Reads, the resume/handoff loop, the
    /// audit / capture / sync / workbench / workspace family, the
    /// orchestrator's read-side, and every leaf `* show` / `* list`.
    execution_or_read,
};

/// Classify a resolved verb path. The path is the slice the parser
/// matched against the comptime tree (e.g. `&.{"plan", "create"}`).
///
/// The decision matrix mirrors the tech spec verb-classification table
/// (`docs/.../142-worktree-management-tech-spec.md § Verb classification`):
///
///  - `init`                                 → planning
///  - `plan {create, update, done, ...}`     → planning
///  - `plan {show, list, next, view, diff}`  → execution_or_read
///  - `task {add, update, done, touches, ...}` → planning
///  - `task {show, list, view, diff}`        → execution_or_read
///  - `question {add, answer, wontfix, link}`→ planning
///  - `question {show, list}`                → execution_or_read
///  - `decision {add, accept, reject}`       → planning
///  - `artifact {add, update, link}`         → planning
///  - `scenario {add, update}`               → planning
///  - `spec {draft, ingest}`                 → planning
///  - `link | unlink | links {add, remove}`  → planning
///  - `association {add, update, ...}`       → planning  (alias: `assoc`)
///  - `promote | demote`                     → planning
///  - `resume | dashboard | handoff *`       → execution_or_read
///  - `capture * | audit * | health`         → execution_or_read
///  - `workbench {pull, push, status, sync, resolve}` → execution_or_read
///  - `workspace * | config * | templates *` → execution_or_read
///  - `tree | search | scope * | version | completion | doc *`
///                                            → execution_or_read
///  - `local * | skills *`                   → execution_or_read
///  - `test-spec *`                          → execution_or_read
///  - `import | synthesize`                  → execution_or_read
///  - `annotate *`                           → planning
///  - `ext *`                                → planning (propagation
///                                            mutates external state)
///  - `sync *`                               → execution_or_read
///                                            (read-side helper; writes
///                                            on conflicts are operator-
///                                            interactive)
///
/// Anything not listed → `planning` (safe default).
pub fn classify(path: []const []const u8) Class {
    if (path.len == 0) return .planning;
    const top = path[0];

    // Top-level execution_or_read verbs (no subverbs to inspect, or
    // every subverb is read-only).
    if (eq(top, "resume") or
        eq(top, "dashboard") or
        eq(top, "health") or
        eq(top, "tree") or
        eq(top, "search") or
        eq(top, "version") or
        eq(top, "completion") or
        eq(top, "import") or
        eq(top, "synthesize"))
    {
        return .execution_or_read;
    }

    // Subverb groups whose entire surface is execution_or_read.
    if (eq(top, "handoff") or
        eq(top, "capture") or
        eq(top, "audit") or
        eq(top, "workspace") or
        eq(top, "config") or
        eq(top, "templates") or
        eq(top, "scope") or
        eq(top, "doc") or
        eq(top, "local") or
        eq(top, "skills") or
        eq(top, "test-spec") or
        eq(top, "sync"))
    {
        return .execution_or_read;
    }

    // workbench: pull / push / status / sync / resolve are
    // execution_or_read. The methodology section calls these out
    // explicitly. Anything else (none today) defaults to planning.
    if (eq(top, "workbench")) {
        if (path.len >= 2) {
            const sub = path[1];
            if (eq(sub, "pull") or eq(sub, "push") or eq(sub, "status") or
                eq(sub, "sync") or eq(sub, "resolve"))
            {
                return .execution_or_read;
            }
        }
        return .planning;
    }

    // Per-domain groups where reads are allowed and writes are refused.
    // The classifier checks the subverb against an allowlist of read-
    // shape leaves. Any subverb NOT on the allowlist (write / mutation)
    // is planning.
    if (eq(top, "plan") or eq(top, "task") or
        eq(top, "question") or eq(top, "scenario") or
        eq(top, "decision") or eq(top, "artifact") or
        eq(top, "association") or eq(top, "assoc"))
    {
        if (path.len >= 2 and isReadLeaf(path[1])) {
            return .execution_or_read;
        }
        // `task done` is deliberately planning-class even though it
        // looks terminal — coders must use `planar-agent complete`.
        // (Falls through here because `done` is not in `isReadLeaf`.)
        return .planning;
    }

    // ext: propagate / sync / push are mutations to external systems —
    // planning intent. Read-side verbs (status, show) are reads.
    if (eq(top, "ext")) {
        if (path.len >= 2 and isReadLeaf(path[1])) return .execution_or_read;
        return .planning;
    }

    // annotate: every subverb mutates. Refused.
    if (eq(top, "annotate")) return .planning;

    // Singletons that mutate top-level planning state.
    if (eq(top, "init") or eq(top, "promote") or eq(top, "demote") or
        eq(top, "link") or eq(top, "unlink") or eq(top, "links"))
    {
        return .planning;
    }

    // Fall through: anything we haven't seen yet — refuse. A new
    // top-level verb that is genuinely a read should be added here in
    // the same PR that introduces it.
    return .planning;
}

fn isReadLeaf(sub: []const u8) bool {
    return eq(sub, "show") or
        eq(sub, "list") or
        eq(sub, "view") or
        eq(sub, "diff") or
        eq(sub, "next") or
        eq(sub, "tree") or
        eq(sub, "review") or
        eq(sub, "status") or
        eq(sub, "log");
}

inline fn eq(a: []const u8, b: []const u8) bool {
    return std.mem.eql(u8, a, b);
}

// =========================================================================
// Tests
// =========================================================================

test "classify: empty path defaults to planning" {
    try std.testing.expectEqual(Class.planning, classify(&.{}));
}

test "classify: top-level reads are execution_or_read" {
    const reads = [_][]const []const u8{
        &.{"resume"},
        &.{"dashboard"},
        &.{"health"},
        &.{"tree"},
        &.{"search"},
        &.{"version"},
    };
    for (reads) |p| {
        try std.testing.expectEqual(Class.execution_or_read, classify(p));
    }
}

test "classify: plan mutations are planning" {
    try std.testing.expectEqual(Class.planning, classify(&.{ "plan", "create" }));
    try std.testing.expectEqual(Class.planning, classify(&.{ "plan", "update" }));
    try std.testing.expectEqual(Class.planning, classify(&.{ "plan", "link" }));
}

test "classify: plan reads are execution_or_read" {
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "plan", "show" }));
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "plan", "list" }));
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "plan", "next" }));
}

test "classify: task done is planning (coders use planar-agent complete)" {
    try std.testing.expectEqual(Class.planning, classify(&.{ "task", "done" }));
}

test "classify: task touches add/remove is planning" {
    try std.testing.expectEqual(Class.planning, classify(&.{ "task", "touches", "add" }));
    try std.testing.expectEqual(Class.planning, classify(&.{ "task", "touches", "remove" }));
}

test "classify: workbench reads + sync are execution_or_read" {
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "workbench", "pull" }));
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "workbench", "push" }));
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "workbench", "status" }));
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "workbench", "sync" }));
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "workbench", "resolve" }));
}

test "classify: unknown top-level verb defaults to planning" {
    try std.testing.expectEqual(Class.planning, classify(&.{"never-heard-of-this"}));
}

test "classify: init / promote / demote / link / unlink are planning" {
    try std.testing.expectEqual(Class.planning, classify(&.{"init"}));
    try std.testing.expectEqual(Class.planning, classify(&.{"promote"}));
    try std.testing.expectEqual(Class.planning, classify(&.{"demote"}));
    try std.testing.expectEqual(Class.planning, classify(&.{"link"}));
    try std.testing.expectEqual(Class.planning, classify(&.{"unlink"}));
    try std.testing.expectEqual(Class.planning, classify(&.{ "links", "add" }));
}

test "classify: question / decision / artifact mutations are planning" {
    try std.testing.expectEqual(Class.planning, classify(&.{ "question", "add" }));
    try std.testing.expectEqual(Class.planning, classify(&.{ "decision", "add" }));
    try std.testing.expectEqual(Class.planning, classify(&.{ "artifact", "update" }));
    try std.testing.expectEqual(Class.planning, classify(&.{ "artifact", "link" }));
}

test "classify: scope subverbs are execution_or_read" {
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "scope", "show" }));
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "scope", "suggest" }));
}

test "classify: ext propagate is planning, ext status is read" {
    try std.testing.expectEqual(Class.planning, classify(&.{ "ext", "propagate" }));
    try std.testing.expectEqual(Class.execution_or_read, classify(&.{ "ext", "status" }));
}
