//! refusal_guard.zig — Harness-level bright-line refusal guard
//! (plan 492 M10 task 3206, tech-spec 267 §1 "Quality spine / reconciliation").
//!
//! ## What this is
//!
//! A startup gate in `planar-execute` that refuses to run a plan/task touching
//! one of three risky surfaces under a workflow that omits a reviewer:
//!
//!   1. `migrations/*.sql`           (schema is the contract)
//!   2. a new top-level CLI verb     (the binary's command tree)
//!   3. `validate`/`invariant`/methodology singleton code
//!      (the gates and the doctrine itself)
//!
//! The "omits a reviewer" signal is trust-based: the workflow's `meta` table
//! gains an optional `reviewer = true/false` field. A workflow that declares
//! `meta.reviewer = true` is taken at its word (the author owns the doctrine
//! contract). The 3205 quality-spine built-ins will set it; ad-hoc workflows
//! that don't will refuse on risky plans by default.
//!
//! ## Why heuristics, not a static analysis
//!
//! The Lua script is dynamic — we cannot statically prove it dispatches a
//! reviewer for every cycle. The reviewer-cadence assertion is the author's
//! responsibility, not statically verifiable. Predicate-side we are
//! deliberately heuristic and prefer **false-positive** (over-block — the
//! operator overrides via `--bypass-reviewer-guard`) to **false-negative**
//! (silently unsafe).
//!
//! ## Skips
//!
//! - `--dry-run` mode never enters `run`, so there is no execution to guard;
//!   the caller MUST skip the guard for dry runs.
//! - No `--plan` ⇒ no tasks to inspect ⇒ the guard is a no-op.
//! - `--bypass-reviewer-guard` ⇒ explicit operator override (prints loud
//!   stderr warning at the caller; this module only reports "bypass").
//!
//! ## Failure mode
//!
//! `checkRefusal` is best-effort on read failure: if the plan/task reads
//! return a `StateError`, the guard treats the plan as **clean** rather than
//! crashing the run. The doctrine signal is the workflow's `meta.reviewer`
//! field; the touch-walk is an additional safety check, not the primary
//! contract. (A read failure on a risky touch would still be caught by the
//! reviewer that an author should be running anyway.)
//!
//! ## Why not import engine/planning/strategy.zig directly
//!
//! `planar-execute` has the no-DB-handle capability stance: it imports
//! nothing from `engine`. The migration predicate logic is small enough to
//! replicate inline; the source is cited so the two sites stay in sync.

const std = @import("std");
const Io = std.Io;

const state = @import("state.zig");

// ============================================================================
// Path predicates
// ============================================================================
//
// Each predicate is PURE and unit-testable. The docstring states exactly what
// is matched and what is intentionally NOT matched. Edge cases (false positives
// vs false negatives) are explicit.

/// isMigrationPath returns true when `path` is an sqlx-cli migration file
/// (`migrations/*.sql`).
///
/// MATCHES:
///   - "migrations/00016_foo.up.sql"
///   - "migrations/00001_foundation.down.sql"
///
/// DOES NOT MATCH:
///   - "migrations/README.md"         (not .sql)
///   - "src/migrations/x.sql"         (not at repo root)
///   - "tools/gen_migrations.zig"     (codegen, not a migration)
///
/// Cited from `src/engine/planning/strategy.zig` (`isMigrationPath`, M5 task
/// 3186). The two sites must agree on the predicate. We replicate inline
/// rather than import `engine.planning.strategy` because `planar-execute`
/// holds no engine import (capability stance).
pub fn isMigrationPath(path: []const u8) bool {
    const prefix = "migrations/";
    if (!std.mem.startsWith(u8, path, prefix)) return false;
    if (!std.mem.endsWith(u8, path, ".sql")) return false;
    return true;
}

/// isTopLevelVerb returns true when `path` looks like a registration site for
/// a NEW top-level CLI verb on one of the family binaries.
///
/// Heuristic: any path under `src/cmd/<binary>/handlers/` (where new per-verb
/// handler code lives), OR `src/cmd/<binary>/cmd.zig` / `src/cmd/<binary>/main.zig`
/// (where the command-tree dispatch is wired). A diff touching these almost
/// always either ADDS or rewires a verb, which is the bright line: a new
/// top-level verb is a capability-boundary change.
///
/// MATCHES:
///   - "src/cmd/planar/handlers/foo/cmd.zig"
///   - "src/cmd/planar-agent/handlers/heartbeat.zig"
///   - "src/cmd/planar/main.zig"
///   - "src/cmd/planar-watch/cmd.zig"
///
/// DOES NOT MATCH:
///   - "src/engine/planning/foo.zig"  (engine-internal, no verb registration)
///   - "src/db/db.zig"                (storage layer)
///   - "vendor/sqlite/sqlite3.c"      (vendored)
///
/// **False-positive note:** an EDIT to an existing verb handler also matches.
/// That's the conservative choice (over-block — operator overrides). A new
/// vs. modified verb cannot be told apart from a path string alone; the
/// reviewer's job is to make that call.
pub fn isTopLevelVerb(path: []const u8) bool {
    // Match `src/cmd/<bin>/handlers/...` (any handler under any family binary).
    if (std.mem.startsWith(u8, path, "src/cmd/")) {
        // Strip the `src/cmd/<bin>/` prefix and look at the tail.
        const tail = path["src/cmd/".len..];
        // Find the next `/` — everything before it is the binary name.
        const slash = std.mem.indexOfScalar(u8, tail, '/') orelse return false;
        const rest = tail[slash + 1 ..];
        if (std.mem.startsWith(u8, rest, "handlers/")) return true;
        // Top-level command-tree wiring files inside the binary.
        if (std.mem.eql(u8, rest, "cmd.zig")) return true;
        if (std.mem.eql(u8, rest, "main.zig")) return true;
        // src/cmd/<bin>/handlers.zig (some binaries put handlers in a single file).
        if (std.mem.eql(u8, rest, "handlers.zig")) return true;
    }
    return false;
}

/// Canonical methodology / invariant singleton files. A diff touching any of
/// these is a doctrine-level change that MUST go through a reviewer.
///
/// Mirrors `singleton_files` in `src/engine/planning/strategy.zig` (the
/// parallelizability rule-4 list) — those two lists must stay in sync.
const invariant_singleton_files = [_][]const u8{
    "agents/methodology.md",
    "CLAUDE.md",
    "AGENTS.md",
    "docs/cli-reference.md",
    "docs/architecture.md",
};

/// isInvariantCode returns true when `path` is either:
///
///   - A canonical methodology / invariant singleton file
///     (`agents/methodology.md`, `CLAUDE.md`, `AGENTS.md`,
///     `docs/cli-reference.md`, `docs/architecture.md`).
///   - A path containing the substring `validate` or `invariant`
///     (validators, invariant locks, lint gates).
///
/// MATCHES:
///   - "agents/methodology.md"            (singleton)
///   - "CLAUDE.md" / "AGENTS.md"          (singleton)
///   - "src/cli/validate.zig"             (substring "validate")
///   - "tools/cli_usage_lint.zig"         — does NOT match by substring;
///     edits to the linter itself should be flagged separately if needed
///     (it touches no "validate"/"invariant" substring). Operator can
///     declare a path-touch on `tools/cli_usage_lint.zig` and the reviewer
///     will be the gate.
///   - "integration_tests/capability_boundary_test.zig" — substring
///     "boundary" is not matched; this is intentional: we keep the
///     substring list narrow ("validate", "invariant") to limit false
///     positives. A capability-boundary test edit is reviewer-worthy but
///     not bright-line-refusal-worthy.
///
/// DOES NOT MATCH:
///   - "src/engine/planning/strategy.zig" (substring not present)
///   - "src/cmd/planar/main.zig"          (isTopLevelVerb catches this)
///
/// **False-positive note:** any file with "validate" in the path name will
/// match (e.g. a hypothetical `src/cmd/foo/validate_helper.zig`). That's
/// the conservative direction: over-block, operator overrides.
pub fn isInvariantCode(path: []const u8) bool {
    for (invariant_singleton_files) |s| {
        if (std.mem.eql(u8, path, s)) return true;
    }
    if (std.mem.indexOf(u8, path, "validate") != null) return true;
    if (std.mem.indexOf(u8, path, "invariant") != null) return true;
    return false;
}

// ============================================================================
// Refusal-result types
// ============================================================================

/// Which of the 3 path predicates a risky touch tripped. Reported back so the
/// caller can name the predicate in the operator-facing stderr message.
pub const Predicate = enum {
    migration,
    new_top_level_verb,
    invariant_code,

    pub fn name(self: Predicate) []const u8 {
        return switch (self) {
            .migration => "migrations/*.sql",
            .new_top_level_verb => "new top-level CLI verb",
            .invariant_code => "validate / invariant / methodology singleton",
        };
    }
};

/// One risky touch identified by the guard. All strings borrow into the
/// caller-supplied `state.PlanNext` / `state.TaskTouchesList` Parsed
/// wrappers — the caller MUST keep those alive while inspecting this value
/// (the current code path consumes the value immediately to format a message
/// and discards before the wrappers free; no out-of-band ownership).
pub const RiskyTouch = struct {
    task_id: u64,
    task_slug: []const u8,
    repo: []const u8,
    path: []const u8,
    predicate: Predicate,
};

/// classifyPath returns the FIRST predicate that matches `path`, or null if
/// the path is clean. Ordering: migration > new top-level verb > invariant
/// code. Two predicates rarely match the same path, but when they do (e.g. a
/// hypothetical "migrations/validate.sql"), the migration label is more
/// specific.
pub fn classifyPath(path: []const u8) ?Predicate {
    if (isMigrationPath(path)) return .migration;
    if (isTopLevelVerb(path)) return .new_top_level_verb;
    if (isInvariantCode(path)) return .invariant_code;
    return null;
}

// ============================================================================
// Plan traversal
// ============================================================================

/// planHasRiskyTouch walks the open tasks on `plan_id` (via `planar plan next
/// <id> --json`) and inspects each task's declared file-level touches (via
/// `planar task touches list <task> --json`). Returns the first risky touch
/// found, or null if the plan is clean.
///
/// "Open tasks" = available + claimed + stale + blocked. Done tasks are not
/// inspected — they're already shipped, the guard is about prospective work.
///
/// Read failures (subprocess non-zero, JSON parse error) are treated as
/// **clean** for that task (the guard is best-effort; a true risky touch
/// would still be caught by the reviewer the author should be running). The
/// only error this function surfaces upward is OOM.
///
/// Memory: the returned `RiskyTouch` borrows into the per-task `Parsed`
/// wrapper that is freed before this function returns. To carry the strings
/// outward, we deep-copy them into `allocator` and return owned slices. The
/// caller MUST call `.deinit(allocator)` on the returned `?OwnedRiskyTouch`.
pub const OwnedRiskyTouch = struct {
    task_id: u64,
    task_slug: []const u8,
    repo: []const u8,
    path: []const u8,
    predicate: Predicate,

    pub fn deinit(self: *OwnedRiskyTouch, allocator: std.mem.Allocator) void {
        allocator.free(self.task_slug);
        allocator.free(self.repo);
        allocator.free(self.path);
    }
};

pub fn planHasRiskyTouch(
    allocator: std.mem.Allocator,
    io: Io,
    plan_id: u64,
) std.mem.Allocator.Error!?OwnedRiskyTouch {
    var next = state.planNext(allocator, io, plan_id) catch return null;
    defer next.deinit();

    // Walk every open task across all four arrays. `available`/`blocked` are
    // bare TaskEntry; `claimed`/`stale` wrap each as { task, claim }.
    for (next.value.available) |t| {
        if (try checkTask(allocator, io, t)) |r| return r;
    }
    for (next.value.blocked) |t| {
        if (try checkTask(allocator, io, t)) |r| return r;
    }
    for (next.value.claimed) |c| {
        if (try checkTask(allocator, io, c.task)) |r| return r;
    }
    for (next.value.stale) |c| {
        if (try checkTask(allocator, io, c.task)) |r| return r;
    }
    return null;
}

/// checkTask reads one task's declared touches and returns the first risky
/// one (deep-copied into `allocator`) or null. Read failures classify the
/// task as clean (see planHasRiskyTouch's docstring).
fn checkTask(
    allocator: std.mem.Allocator,
    io: Io,
    t: state.TaskEntry,
) std.mem.Allocator.Error!?OwnedRiskyTouch {
    var touches = state.taskTouchesList(allocator, io, t.id) catch return null;
    defer touches.deinit();

    for (touches.value.paths) |row| {
        const predicate = classifyPath(row.path) orelse continue;
        // Deep-copy strings so they survive the wrapper free.
        const slug_owned = try allocator.dupe(u8, t.slug orelse "");
        errdefer allocator.free(slug_owned);
        const repo_owned = try allocator.dupe(u8, row.repo);
        errdefer allocator.free(repo_owned);
        const path_owned = try allocator.dupe(u8, row.path);
        return OwnedRiskyTouch{
            .task_id = t.id,
            .task_slug = slug_owned,
            .repo = repo_owned,
            .path = path_owned,
            .predicate = predicate,
        };
    }
    return null;
}

// ============================================================================
// Top-level decision
// ============================================================================

/// Result of the guard check; one of:
///
///   .pass         — guard had no reason to refuse (workflow declared
///                   `meta.reviewer = true`, OR no plan was given, OR
///                   the plan has no risky touches).
///   .bypassed     — operator passed `--bypass-reviewer-guard`. Caller
///                   should print a LOUD stderr warning and proceed.
///   .refused      — guard found a risky touch under a reviewer-less
///                   workflow. Caller MUST print the diagnostic, free the
///                   carried RiskyTouch, and exit non-zero.
pub const RefusalResult = union(enum) {
    pass,
    bypassed,
    refused: OwnedRiskyTouch,

    pub fn deinit(self: *RefusalResult, allocator: std.mem.Allocator) void {
        switch (self.*) {
            .refused => |*r| r.deinit(allocator),
            else => {},
        }
    }
};

/// Inputs to the guard. Kept separate from the body so the caller (handleRun)
/// can build them once near the top of the handler.
pub const Inputs = struct {
    /// True iff the workflow declared `meta.reviewer = true`. Defaults to
    /// false when the field is absent or the wrong type.
    reviewer_declared: bool,
    /// The `--plan` flag value. 0 means absent → no-op.
    plan_id: u64,
    /// True iff the operator passed `--bypass-reviewer-guard`.
    bypass: bool,
};

/// checkRefusal orchestrates the decision:
///
///   1. If reviewer_declared = true ⇒ pass (author owns the contract).
///   2. Else if bypass = true ⇒ bypassed (caller prints the warning).
///   3. Else if plan_id = 0 ⇒ pass (no plan, no tasks, nothing to inspect).
///   4. Else walk the plan; refuse on the first risky touch, pass otherwise.
///
/// Returns a `RefusalResult`. The caller MUST call `.deinit(allocator)` on
/// the result when done (the `.refused` variant carries heap-owned strings).
///
/// `allocator` and `io` are only used by step 4. Steps 1–3 do not allocate or
/// shell.
pub fn checkRefusal(
    allocator: std.mem.Allocator,
    io: Io,
    inputs: Inputs,
) std.mem.Allocator.Error!RefusalResult {
    if (inputs.reviewer_declared) return .pass;
    if (inputs.bypass) return .bypassed;
    if (inputs.plan_id == 0) return .pass;

    if (try planHasRiskyTouch(allocator, io, inputs.plan_id)) |risky| {
        return RefusalResult{ .refused = risky };
    }
    return .pass;
}

// ============================================================================
// Unit tests — pure predicates
// ============================================================================

const testing = std.testing;

test "isMigrationPath: classifies correctly" {
    try testing.expect(isMigrationPath("migrations/00016_foo.up.sql"));
    try testing.expect(isMigrationPath("migrations/00001_foundation.down.sql"));
    try testing.expect(!isMigrationPath("migrations/README.md"));
    try testing.expect(!isMigrationPath("src/migrations/x.sql"));
    try testing.expect(!isMigrationPath("tools/gen_migrations.zig"));
    try testing.expect(!isMigrationPath(""));
    try testing.expect(!isMigrationPath("migrations.sql"));
}

test "isTopLevelVerb: matches handlers/ + cmd.zig + main.zig under src/cmd/<bin>/" {
    // Handler files under any family binary.
    try testing.expect(isTopLevelVerb("src/cmd/planar/handlers/foo/cmd.zig"));
    try testing.expect(isTopLevelVerb("src/cmd/planar-agent/handlers/heartbeat.zig"));
    try testing.expect(isTopLevelVerb("src/cmd/planar-watch/handlers/ps.zig"));
    try testing.expect(isTopLevelVerb("src/cmd/planar-execute/handlers/foo.zig"));
    // Top-level wiring files.
    try testing.expect(isTopLevelVerb("src/cmd/planar/main.zig"));
    try testing.expect(isTopLevelVerb("src/cmd/planar/cmd.zig"));
    try testing.expect(isTopLevelVerb("src/cmd/planar-agent/cmd.zig"));
    try testing.expect(isTopLevelVerb("src/cmd/planar/handlers.zig"));
}

test "isTopLevelVerb: rejects non-verb-registration paths" {
    try testing.expect(!isTopLevelVerb("src/engine/planning/strategy.zig"));
    try testing.expect(!isTopLevelVerb("src/db/db.zig"));
    try testing.expect(!isTopLevelVerb("vendor/sqlite/sqlite3.c"));
    try testing.expect(!isTopLevelVerb("docs/cli-reference.md"));
    try testing.expect(!isTopLevelVerb(""));
    try testing.expect(!isTopLevelVerb("src/cmd/"));
    try testing.expect(!isTopLevelVerb("src/cmd/planar/"));
    // `src/cmd/<bin>/exit.zig` is internal runtime scaffolding, NOT a verb.
    try testing.expect(!isTopLevelVerb("src/cmd/planar/exit.zig"));
    try testing.expect(!isTopLevelVerb("src/cmd/planar/runtime.zig"));
}

test "isInvariantCode: matches singletons + validate/invariant substrings" {
    // Canonical singletons.
    try testing.expect(isInvariantCode("agents/methodology.md"));
    try testing.expect(isInvariantCode("CLAUDE.md"));
    try testing.expect(isInvariantCode("AGENTS.md"));
    try testing.expect(isInvariantCode("docs/cli-reference.md"));
    try testing.expect(isInvariantCode("docs/architecture.md"));
    // Substring matches.
    try testing.expect(isInvariantCode("src/cli/validate.zig"));
    try testing.expect(isInvariantCode("integration_tests/some_validate_test.zig"));
    try testing.expect(isInvariantCode("src/engine/invariant_check.zig"));
    try testing.expect(isInvariantCode("docs/invariants.md"));
}

test "isInvariantCode: rejects unrelated paths" {
    try testing.expect(!isInvariantCode("src/engine/planning/strategy.zig"));
    try testing.expect(!isInvariantCode("src/cmd/planar/main.zig"));
    try testing.expect(!isInvariantCode("README.md"));
    try testing.expect(!isInvariantCode(""));
    // capability_boundary_test is reviewer-worthy but intentionally NOT
    // bright-line-refusal-worthy (narrow substring list).
    try testing.expect(!isInvariantCode("integration_tests/capability_boundary_test.zig"));
    // tools/cli_usage_lint.zig — no "validate"/"invariant" substring.
    try testing.expect(!isInvariantCode("tools/cli_usage_lint.zig"));
}

test "classifyPath: returns first matching predicate or null" {
    try testing.expectEqual(@as(?Predicate, .migration), classifyPath("migrations/00020_foo.up.sql"));
    try testing.expectEqual(@as(?Predicate, .new_top_level_verb), classifyPath("src/cmd/planar/handlers/foo.zig"));
    try testing.expectEqual(@as(?Predicate, .invariant_code), classifyPath("agents/methodology.md"));
    try testing.expectEqual(@as(?Predicate, .invariant_code), classifyPath("src/cli/validate.zig"));
    try testing.expectEqual(@as(?Predicate, null), classifyPath("src/engine/planning/strategy.zig"));
    try testing.expectEqual(@as(?Predicate, null), classifyPath(""));
}

test "Predicate.name: stable operator-facing labels" {
    try testing.expectEqualStrings("migrations/*.sql", Predicate.migration.name());
    try testing.expectEqualStrings("new top-level CLI verb", Predicate.new_top_level_verb.name());
    try testing.expectEqualStrings("validate / invariant / methodology singleton", Predicate.invariant_code.name());
}

test "checkRefusal: reviewer_declared short-circuits to pass" {
    // Allocator + io are unused in the short-circuit path; pass undefined.
    // Use std.testing.allocator anyway in case future refactors allocate.
    var res = try checkRefusal(testing.allocator, undefined, .{
        .reviewer_declared = true,
        .plan_id = 999, // would have triggered a plan walk if reviewer = false
        .bypass = false,
    });
    defer res.deinit(testing.allocator);
    try testing.expect(res == .pass);
}

test "checkRefusal: bypass returns .bypassed when reviewer not declared" {
    var res = try checkRefusal(testing.allocator, undefined, .{
        .reviewer_declared = false,
        .plan_id = 999,
        .bypass = true,
    });
    defer res.deinit(testing.allocator);
    try testing.expect(res == .bypassed);
}

test "checkRefusal: no plan and no reviewer ⇒ pass (no-op)" {
    var res = try checkRefusal(testing.allocator, undefined, .{
        .reviewer_declared = false,
        .plan_id = 0,
        .bypass = false,
    });
    defer res.deinit(testing.allocator);
    try testing.expect(res == .pass);
}
