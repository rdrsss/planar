//! state.zig — Planner state-read helpers for `planar-execute`.
//!
//! Shells out to the `planar` CLI (resolved via PATH, the installed binary)
//! and parses the captured stdout JSON into minimal typed Zig structs.
//!
//! ## Capability boundary
//!
//! This module holds NO SQLite handle and imports NO db/engine/runtime module.
//! Every state read is a subprocess call (`std.process.run`) followed by
//! `std.json.parseFromSlice` with `ignore_unknown_fields = true`.  The
//! structs declare only the fields the Planner and brief compiler consume.
//!
//! ## Memory ownership
//!
//! Each `planShow` / `planNext` call returns a `std.json.Parsed(T)` wrapper.
//! The caller owns the memory and MUST call `.deinit()` when done.
//!
//! `testSpecStatus` returns a `TestSpecStatus` struct.  Its slice fields are
//! allocated with the supplied allocator; the caller MUST call
//! `TestSpecStatus.deinit(allocator)` when done.
//!
//! ## Subprocess stdout limit
//!
//! `planar plan next` can return large JSON for plans with many tasks.  We
//! cap stdout at 4 MiB (well above any realistic plan) so an unusually large
//! output does not OOM the parent.  `planar plan show` and `planar test-spec
//! status` are far smaller; they use a 256 KiB cap.
//!
//! ## Error mapping
//!
//! All subprocess and parse errors are mapped into the `StateError` set.
//! No error is silently swallowed.

const std = @import("std");
const Io = std.Io;

// ---------------------------------------------------------------------------
// Error set
// ---------------------------------------------------------------------------

/// All errors this module can surface.  Callers inspect these rather than
/// catching `anyerror` so the reviewer brief can cite them precisely.
pub const StateError = error{
    /// `std.process.run` itself failed (could not spawn the child, broken
    /// pipe reading stdout, etc.).  Treat as a transient / configuration error.
    SubprocessFailed,
    /// The child exited with a non-zero status code.  `planar` exits non-zero
    /// on not-found, bad flags, DB errors, etc.
    SubprocessNonZero,
    /// `std.json.parseFromSlice` rejected the output (not valid JSON, or a
    /// required field is missing / wrong type).
    ParseFailed,
    /// `planar test-spec status --json` emitted zero parseable lines.
    TestSpecNoOutput,
    /// Allocator returned OOM while building the `TestSpecStatus` result.
    OutOfMemory,
};

// ---------------------------------------------------------------------------
// Typed structs — minimal; ignore_unknown_fields covers the rest.
// ---------------------------------------------------------------------------

/// Minimal view of `planar plan show <id> --json`.
///
/// Real shape (captured 2026-06-03):
///   {"id":492,"scope_kind":"association","scope_id":1,"title":"...","slug":"...",
///    "summary":null,"status":"active","parent_plan_id":null,
///    "created_at":"...","updated_at":"..."}
///
/// Fields declared: only those the Planner / brief compiler consumes.
pub const PlanShow = struct {
    id: u64,
    title: []const u8,
    status: []const u8,
    /// `slug` is always present in practice (set on plan creation);
    /// declared optional for defensive parsing in case an older DB row
    /// pre-dates the slug column.
    slug: ?[]const u8 = null,
    /// Non-null for child milestone plans; null for top-level anchor plans.
    parent_plan_id: ?u64 = null,
};

/// A single task entry inside `planar plan next <id> --json`'s task arrays.
///
/// The verb returns tasks in four arrays: `available`, `claimed`, `stale`,
/// `blocked`.  `available` and `blocked` emit bare task objects with this
/// shape (captured 2026-06-03):
///   {"id":3173,"scope_kind":"association","scope_id":1,"plan_id":496,
///    "parent_task_id":null,"title":"...","body":"...","slug":"m3-...",
///    "status":"todo","priority":100,"next_action":"...","due_at":null,
///    "created_at":"...","updated_at":"..."}
///
/// `claimed` and `stale` wrap each entry as `{task: Task, claim: ClaimRow}`
/// — see `ClaimedTaskEntry`.
///
/// Fields declared: id, plan_id, title, slug, status.
pub const TaskEntry = struct {
    id: u64,
    plan_id: u64,
    title: []const u8,
    slug: ?[]const u8 = null,
    status: []const u8,
};

/// Wrapper for entries in `planar plan next --json`'s `claimed` and `stale`
/// arrays.
///
/// The emitter (handlers/plan/next.zig ~L218-261) writes each entry as:
///   { "task": <Task>, "claim": <ClaimRow> }
///
/// `claim` is kept as `std.json.Value` so the Planner can forward stale-claim
/// metadata to M3/M4 brief compilation without requiring a fully typed
/// `ClaimRow` now.  The value is owned by the `Parsed(PlanNext)` arena.
/// `claim` may be `null` (the emitter writes `"claim":null` when no claim row
/// is present on the row), so `std.json.Value` covers both cases.
pub const ClaimedTaskEntry = struct {
    task: TaskEntry,
    claim: std.json.Value,
};

/// The `summary` sub-object inside `planar plan next <id> --json`.
///
/// Shape: {"available":2,"claimed":0,"stale":0,"blocked":0,"done":0}
pub const PlanNextSummary = struct {
    available: u64,
    claimed: u64,
    stale: u64,
    blocked: u64,
    done: u64,
};

/// Minimal view of `planar plan next <id> --json`.
///
/// Real shape (confirmed from handlers/plan/next.zig emitter 2026-06-03):
///   { "plan_id": int,
///     "available": [Task],
///     "claimed":   [{ "task": Task, "claim": ClaimRow }],
///     "stale":     [{ "task": Task, "claim": ClaimRow }],
///     "blocked":   [Task],
///     "summary":   { "available": int, "claimed": int, "stale": int,
///                    "blocked": int, "done": int, "note"?: string } }
///
/// `available` and `blocked` are bare Task arrays.
/// `claimed` and `stale` are wrapper arrays — each element is a
/// `ClaimedTaskEntry { task: Task, claim: ClaimRow|null }`.
pub const PlanNext = struct {
    plan_id: u64,
    available: []TaskEntry,
    claimed: []ClaimedTaskEntry,
    stale: []ClaimedTaskEntry,
    blocked: []TaskEntry,
    summary: PlanNextSummary,
};

/// One per-plan line from `planar test-spec status <id> --json` (NDJSON).
///
/// All lines except the last have `plan_id` (not `anchor_plan_id`).
/// Shape: {"plan_id":494,"title":"M1 — ...","total_tasks":18,
///         "tasks_with_slug":7,"tasks_covered":1,"happy":1,"empty":0,
///         "error":0,"edge":0,"other":0}
pub const PlanCoverage = struct {
    plan_id: u64,
    title: []const u8,
    total_tasks: u64,
    tasks_with_slug: u64,
    tasks_covered: u64,
    happy: u64,
    empty: u64,
    @"error": u64,
    edge: u64,
    other: u64,
};

/// The anchor-summary line (the last NDJSON line from `planar test-spec status`).
///
/// Shape: {"anchor_plan_id":492,"total_tasks":60,"tasks_with_slug":47,
///         "tasks_covered":3,"total_scenarios":6}
pub const TestSpecAnchor = struct {
    anchor_plan_id: u64,
    total_tasks: u64,
    tasks_with_slug: u64,
    tasks_covered: u64,
    total_scenarios: u64,
};

/// Result of `testSpecStatus`.  Owns all heap memory; call `.deinit(allocator)`.
///
/// `per_plan` is a slice of per-milestone coverage rows (one per child plan).
/// `anchor` is the aggregate summary at the end of the NDJSON stream.
pub const TestSpecStatus = struct {
    per_plan: []PlanCoverage,
    anchor: TestSpecAnchor,

    /// Free all heap-owned memory (the `per_plan` slice and the `title` strings
    /// within each `PlanCoverage`).  The `TestSpecAnchor` has no heap strings.
    pub fn deinit(self: *TestSpecStatus, allocator: std.mem.Allocator) void {
        for (self.per_plan) |row| {
            allocator.free(row.title);
        }
        allocator.free(self.per_plan);
    }
};

// ---------------------------------------------------------------------------
// Internal subprocess helper
// ---------------------------------------------------------------------------

/// spawnPlanar runs `planar <argv_tail...>` and returns the captured stdout.
///
/// The caller owns the returned slice and must free it with `allocator`.
/// Maps subprocess and non-zero-exit errors into `StateError`.
fn spawnPlanar(
    allocator: std.mem.Allocator,
    io: Io,
    argv_tail: []const []const u8,
    stdout_limit: usize,
) StateError![]u8 {
    // Build argv: ["planar"] ++ argv_tail.
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(allocator);
    argv.append(allocator, "planar") catch return StateError.OutOfMemory;
    for (argv_tail) |arg| argv.append(allocator, arg) catch return StateError.OutOfMemory;

    const result = std.process.run(allocator, io, .{
        .argv = argv.items,
        .stdout_limit = Io.Limit.limited(stdout_limit),
        .stderr_limit = Io.Limit.limited(4096),
    }) catch return StateError.SubprocessFailed;

    // Free stderr immediately (not used by callers).
    allocator.free(result.stderr);

    // Map non-zero exit to an error.  Free stdout before returning the error.
    const exit_ok = result.term == .exited and result.term.exited == 0;
    if (!exit_ok) {
        allocator.free(result.stdout);
        return StateError.SubprocessNonZero;
    }

    return result.stdout;
}

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

/// planShow shells `planar plan show <plan_id> --json` and returns a
/// `std.json.Parsed(PlanShow)`.
///
/// The caller owns the memory and MUST call `.deinit()` on the returned value.
pub fn planShow(
    allocator: std.mem.Allocator,
    io: Io,
    plan_id: u64,
) StateError!std.json.Parsed(PlanShow) {
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{plan_id}) catch return StateError.SubprocessFailed;

    const stdout = try spawnPlanar(allocator, io, &.{ "plan", "show", id_str, "--json" }, 256 * 1024);
    defer allocator.free(stdout);

    return std.json.parseFromSlice(PlanShow, allocator, stdout, .{
        .ignore_unknown_fields = true,
    }) catch return StateError.ParseFailed;
}

/// planNext shells `planar plan next <plan_id> --json` and returns a
/// `std.json.Parsed(PlanNext)`.
///
/// The caller owns the memory and MUST call `.deinit()` on the returned value.
/// The `available`, `claimed`, `stale`, and `blocked` slices in the result
/// are owned by the `Parsed` wrapper.
pub fn planNext(
    allocator: std.mem.Allocator,
    io: Io,
    plan_id: u64,
) StateError!std.json.Parsed(PlanNext) {
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{plan_id}) catch return StateError.SubprocessFailed;

    // 4 MiB cap: plan next can return many task entries for large plans.
    const stdout = try spawnPlanar(allocator, io, &.{ "plan", "next", id_str, "--json" }, 4 * 1024 * 1024);
    defer allocator.free(stdout);

    return std.json.parseFromSlice(PlanNext, allocator, stdout, .{
        .ignore_unknown_fields = true,
    }) catch return StateError.ParseFailed;
}

/// testSpecStatus shells `planar test-spec status <plan_id> --json` and
/// returns a `TestSpecStatus`.
///
/// The output is NDJSON: one `PlanCoverage` line per child milestone plan,
/// followed by one `TestSpecAnchor` summary line (identified by the
/// `anchor_plan_id` key).
///
/// Memory: `title` strings in `per_plan` are allocator-owned copies.
/// The caller MUST call `result.deinit(allocator)` when done.
pub fn testSpecStatus(
    allocator: std.mem.Allocator,
    io: Io,
    plan_id: u64,
) StateError!TestSpecStatus {
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{plan_id}) catch return StateError.SubprocessFailed;

    const stdout = try spawnPlanar(allocator, io, &.{ "test-spec", "status", id_str, "--json" }, 256 * 1024);
    defer allocator.free(stdout);

    // Parse NDJSON: split on newlines, classify each line.
    // A line with "anchor_plan_id" is the summary; all others are PlanCoverage rows.
    var per_plan = std.ArrayList(PlanCoverage).empty;
    errdefer {
        for (per_plan.items) |row| allocator.free(row.title);
        per_plan.deinit(allocator);
    }
    var maybe_anchor: ?TestSpecAnchor = null;

    var lines = std.mem.splitScalar(u8, stdout, '\n');
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");
        if (line.len == 0) continue;

        // Determine line type by checking for the anchor key.
        if (std.mem.indexOf(u8, line, "\"anchor_plan_id\"") != null) {
            // Anchor summary line.
            const parsed_anchor = std.json.parseFromSlice(TestSpecAnchor, allocator, line, .{
                .ignore_unknown_fields = true,
            }) catch return StateError.ParseFailed;
            defer parsed_anchor.deinit();
            maybe_anchor = parsed_anchor.value;
        } else {
            // Per-plan coverage line.
            const parsed_row = std.json.parseFromSlice(PlanCoverage, allocator, line, .{
                .ignore_unknown_fields = true,
            }) catch return StateError.ParseFailed;
            defer parsed_row.deinit();

            // Deep-copy the title string so the PlanCoverage we store outlives
            // the parsed_row arena.
            const title_copy = allocator.dupe(u8, parsed_row.value.title) catch return StateError.OutOfMemory;
            errdefer allocator.free(title_copy);

            const row: PlanCoverage = .{
                .plan_id = parsed_row.value.plan_id,
                .title = title_copy,
                .total_tasks = parsed_row.value.total_tasks,
                .tasks_with_slug = parsed_row.value.tasks_with_slug,
                .tasks_covered = parsed_row.value.tasks_covered,
                .happy = parsed_row.value.happy,
                .empty = parsed_row.value.empty,
                .@"error" = parsed_row.value.@"error",
                .edge = parsed_row.value.edge,
                .other = parsed_row.value.other,
            };
            per_plan.append(allocator, row) catch return StateError.OutOfMemory;
        }
    }

    if (maybe_anchor == null and per_plan.items.len == 0) return StateError.TestSpecNoOutput;

    const anchor = maybe_anchor orelse TestSpecAnchor{
        .anchor_plan_id = plan_id,
        .total_tasks = 0,
        .tasks_with_slug = 0,
        .tasks_covered = 0,
        .total_scenarios = 0,
    };

    return TestSpecStatus{
        .per_plan = try per_plan.toOwnedSlice(allocator),
        .anchor = anchor,
    };
}

// ---------------------------------------------------------------------------
// Unit tests — fixture-parse only, no live planar binary required.
// ---------------------------------------------------------------------------

test "PlanShow: parse real fixture — known fields decoded, extras ignored" {
    // Real JSON captured from `planar plan show 492 --json` on 2026-06-03.
    // Extra fields (scope_kind, scope_id, summary, created_at, updated_at)
    // must be silently ignored (ignore_unknown_fields = true).
    const fixture =
        \\{"id":492,"scope_kind":"association","scope_id":1,
        \\"title":"Autonomous workflow harness (planar-execute)",
        \\"slug":"orchestrate-harness","summary":null,"status":"active",
        \\"parent_plan_id":null,"created_at":"2026-05-29T21:21:10.629Z",
        \\"updated_at":"2026-06-03T00:15:44.069Z"}
    ;
    const parsed = try std.json.parseFromSlice(PlanShow, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(u64, 492), parsed.value.id);
    try std.testing.expectEqualStrings("Autonomous workflow harness (planar-execute)", parsed.value.title);
    try std.testing.expectEqualStrings("active", parsed.value.status);
    try std.testing.expectEqualStrings("orchestrate-harness", parsed.value.slug.?);
    try std.testing.expectEqual(@as(?u64, null), parsed.value.parent_plan_id);
}

test "PlanShow: parent_plan_id non-null (child milestone)" {
    // A milestone plan with parent_plan_id set.  The slug may be non-null too.
    const fixture =
        \\{"id":495,"scope_kind":"association","scope_id":1,
        \\"title":"M2 — Host-function surface, sandbox, brief compiler",
        \\"slug":"m2-host-function-surface-sandbox-brief-compiler",
        \\"summary":null,"status":"draft","parent_plan_id":492,
        \\"created_at":"2026-06-03T00:15:21.626Z","updated_at":"2026-06-03T00:15:21.626Z"}
    ;
    const parsed = try std.json.parseFromSlice(PlanShow, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(u64, 495), parsed.value.id);
    try std.testing.expectEqual(@as(?u64, 492), parsed.value.parent_plan_id);
    try std.testing.expectEqualStrings("draft", parsed.value.status);
}

test "PlanShow: ignore_unknown_fields — extra keys do not cause ParseFailed" {
    // A fixture with extra unknown fields.  Must parse without error.
    const fixture =
        \\{"id":1,"title":"T","status":"active",
        \\"future_field":"will-be-added-later","another_unknown":42}
    ;
    const parsed = try std.json.parseFromSlice(PlanShow, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(u64, 1), parsed.value.id);
    try std.testing.expectEqualStrings("active", parsed.value.status);
}

test "PlanShow: malformed JSON → error" {
    const fixture = "this is not json {{{";
    const result = std.json.parseFromSlice(PlanShow, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    try std.testing.expectError(error.SyntaxError, result);
}

test "PlanNext: parse real fixture — available tasks decoded" {
    // Real-shape fixture from `planar plan next 496 --json` (two available tasks).
    const fixture =
        \\{"plan_id":496,
        \\"available":[
        \\  {"id":3173,"scope_kind":"association","scope_id":1,"plan_id":496,
        \\   "parent_task_id":null,
        \\   "title":"git worktree add on a per-task epic-child branch","body":"...",
        \\   "slug":"m3-worktree-lifecycle","status":"todo","priority":100,
        \\   "next_action":"Implement per acceptance criteria.","due_at":null,
        \\   "created_at":"2026-06-03T00:15:21.630Z","updated_at":"2026-06-03T00:15:21.630Z"},
        \\  {"id":3174,"scope_kind":"association","scope_id":1,"plan_id":496,
        \\   "parent_task_id":null,
        \\   "title":"Startup reconcile pass","body":"...",
        \\   "slug":"m3-startup-reconcile","status":"todo","priority":100,
        \\   "next_action":"Implement per acceptance criteria.","due_at":null,
        \\   "created_at":"2026-06-03T00:15:21.630Z","updated_at":"2026-06-03T00:15:21.630Z"}
        \\],
        \\"claimed":[],"stale":[],"blocked":[],
        \\"summary":{"available":2,"claimed":0,"stale":0,"blocked":0,"done":0}}
    ;
    const parsed = try std.json.parseFromSlice(PlanNext, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(u64, 496), parsed.value.plan_id);
    try std.testing.expectEqual(@as(usize, 2), parsed.value.available.len);
    try std.testing.expectEqual(@as(usize, 0), parsed.value.claimed.len);
    try std.testing.expectEqual(@as(u64, 3173), parsed.value.available[0].id);
    try std.testing.expectEqualStrings("m3-worktree-lifecycle", parsed.value.available[0].slug.?);
    try std.testing.expectEqualStrings("todo", parsed.value.available[0].status);
    try std.testing.expectEqual(@as(u64, 496), parsed.value.available[0].plan_id);
    try std.testing.expectEqual(@as(u64, 2), parsed.value.summary.available);
    try std.testing.expectEqual(@as(u64, 0), parsed.value.summary.done);
}

test "PlanNext: empty available list" {
    // `planar plan next 492 --json` when anchor plan has no tasks directly.
    const fixture =
        \\{"plan_id":492,"available":[],"claimed":[],"stale":[],"blocked":[],
        \\"summary":{"available":0,"claimed":0,"stale":0,"blocked":0,"done":0}}
    ;
    const parsed = try std.json.parseFromSlice(PlanNext, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(u64, 492), parsed.value.plan_id);
    try std.testing.expectEqual(@as(usize, 0), parsed.value.available.len);
    try std.testing.expectEqual(@as(u64, 0), parsed.value.summary.available);
}

test "PlanNext: ignore_unknown_fields — extra keys in task and top-level ignored" {
    const fixture =
        \\{"plan_id":1,"available":[{"id":9,"plan_id":1,"title":"T","status":"todo",
        \\  "extra_future_key":"ignored","another":true}],
        \\"claimed":[],"stale":[],"blocked":[],
        \\"summary":{"available":1,"claimed":0,"stale":0,"blocked":0,"done":0},
        \\"top_level_extra":99}
    ;
    const parsed = try std.json.parseFromSlice(PlanNext, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(usize, 1), parsed.value.available.len);
    try std.testing.expectEqual(@as(u64, 9), parsed.value.available[0].id);
}

test "PlanNext: task with null slug" {
    // Tasks added via `planar task add` without a slug have null slug.
    const fixture =
        \\{"plan_id":1,"available":[
        \\  {"id":10,"plan_id":1,"title":"No slug","slug":null,"status":"todo"}],
        \\"claimed":[],"stale":[],"blocked":[],
        \\"summary":{"available":1,"claimed":0,"stale":0,"blocked":0,"done":0}}
    ;
    const parsed = try std.json.parseFromSlice(PlanNext, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(?[]const u8, null), parsed.value.available[0].slug);
}

test "PlanNext: malformed JSON → error" {
    const fixture = "{not valid json";
    const result = std.json.parseFromSlice(PlanNext, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    try std.testing.expectError(error.SyntaxError, result);
}

// RED-THEN-GREEN: populated claimed + stale wrapper arrays.
//
// This fixture would FAIL against the old `[]TaskEntry` struct (the parser
// cannot find `id`/`plan_id`/`title`/`status` at the wrapper level because
// they are nested under `.task`).  It passes after the `[]ClaimedTaskEntry`
// fix — confirming the bug is closed.
test "PlanNext: populated claimed and stale with real wrapper shape — task fields accessible" {
    // Shape from handlers/plan/next.zig ~L218-261:
    //   claimed / stale: [{ "task": <Task>, "claim": <ClaimRow> }]
    const fixture =
        \\{"plan_id":496,
        \\"available":[],
        \\"claimed":[
        \\  {"task":{"id":3173,"scope_kind":"association","scope_id":1,"plan_id":496,
        \\            "parent_task_id":null,"title":"git worktree add","body":"...",
        \\            "slug":"m3-worktree-lifecycle","status":"in_progress","priority":100,
        \\            "next_action":"Implement per acceptance criteria.","due_at":null,
        \\            "created_at":"2026-06-03T00:15:21.630Z","updated_at":"2026-06-03T01:00:00.000Z"},
        \\   "claim":{"id":7,"claim_token":"f58574d29b6e62ba1b9ced92420e784a",
        \\            "session_id":1,"entity_kind":"task","entity_id":3173,
        \\            "claim_scope":"repo","status":"active","vendor":"claude",
        \\            "vendor_session_id":null,"role":"coder","model":null,
        \\            "worktree_id":null,"worktree_path":null,
        \\            "repo_root":"/Users/mn/projects/github/rdrsss/planar",
        \\            "branch":"epic/p492-orchestrate-harness","head_sha_at_claim":"d9ce1df",
        \\            "dirty_at_claim":null,"purpose":null,"base_ref":null,
        \\            "claimed_at":"2026-06-03T01:00:00.000Z",
        \\            "last_heartbeat_at":"2026-06-03T01:05:00.000Z",
        \\            "lease_expires_at":"2026-06-03T01:10:00.000Z",
        \\            "released_at":null,"release_reason":null}}
        \\],
        \\"stale":[
        \\  {"task":{"id":3174,"scope_kind":"association","scope_id":1,"plan_id":496,
        \\            "parent_task_id":null,"title":"Startup reconcile pass","body":"...",
        \\            "slug":"m3-startup-reconcile","status":"in_progress","priority":100,
        \\            "next_action":"Implement per acceptance criteria.","due_at":null,
        \\            "created_at":"2026-06-03T00:15:21.630Z","updated_at":"2026-06-03T00:30:00.000Z"},
        \\   "claim":{"id":8,"claim_token":"aabbccdd11223344aabbccdd11223344",
        \\            "session_id":2,"entity_kind":"task","entity_id":3174,
        \\            "claim_scope":"repo","status":"stale","vendor":"claude",
        \\            "vendor_session_id":null,"role":"coder","model":null,
        \\            "worktree_id":null,"worktree_path":null,
        \\            "repo_root":"/Users/mn/projects/github/rdrsss/planar",
        \\            "branch":"epic/p492-orchestrate-harness","head_sha_at_claim":"d9ce1df",
        \\            "dirty_at_claim":null,"purpose":null,"base_ref":null,
        \\            "claimed_at":"2026-06-03T00:00:00.000Z",
        \\            "last_heartbeat_at":"2026-06-03T00:01:00.000Z",
        \\            "lease_expires_at":"2026-06-03T00:06:00.000Z",
        \\            "released_at":null,"release_reason":null}}
        \\],
        \\"blocked":[],
        \\"summary":{"available":0,"claimed":1,"stale":1,"blocked":0,"done":5}}
    ;
    const parsed = try std.json.parseFromSlice(PlanNext, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    // Top-level structure.
    try std.testing.expectEqual(@as(u64, 496), parsed.value.plan_id);
    try std.testing.expectEqual(@as(usize, 0), parsed.value.available.len);
    try std.testing.expectEqual(@as(usize, 1), parsed.value.claimed.len);
    try std.testing.expectEqual(@as(usize, 1), parsed.value.stale.len);
    try std.testing.expectEqual(@as(usize, 0), parsed.value.blocked.len);

    // Claimed wrapper: task fields accessible via .task sub-struct.
    const claimed_entry = parsed.value.claimed[0];
    try std.testing.expectEqual(@as(u64, 3173), claimed_entry.task.id);
    try std.testing.expectEqual(@as(u64, 496), claimed_entry.task.plan_id);
    try std.testing.expectEqualStrings("git worktree add", claimed_entry.task.title);
    try std.testing.expectEqualStrings("in_progress", claimed_entry.task.status);
    try std.testing.expectEqualStrings("m3-worktree-lifecycle", claimed_entry.task.slug.?);
    // Claim is present (non-null std.json.Value object).
    try std.testing.expect(claimed_entry.claim != .null);

    // Stale wrapper: task fields accessible via .task sub-struct.
    const stale_entry = parsed.value.stale[0];
    try std.testing.expectEqual(@as(u64, 3174), stale_entry.task.id);
    try std.testing.expectEqual(@as(u64, 496), stale_entry.task.plan_id);
    try std.testing.expectEqualStrings("Startup reconcile pass", stale_entry.task.title);
    try std.testing.expectEqualStrings("in_progress", stale_entry.task.status);
    try std.testing.expectEqualStrings("m3-startup-reconcile", stale_entry.task.slug.?);
    try std.testing.expect(stale_entry.claim != .null);

    // Summary counts.
    try std.testing.expectEqual(@as(u64, 1), parsed.value.summary.claimed);
    try std.testing.expectEqual(@as(u64, 1), parsed.value.summary.stale);
    try std.testing.expectEqual(@as(u64, 5), parsed.value.summary.done);
}

test "PlanNext: claimed entry with null claim field — tolerated by std.json.Value" {
    // The emitter writes "claim":null when r.claim is null on the NextWorkRow.
    // std.json.Value decodes null as .null; must not error.
    const fixture =
        \\{"plan_id":1,
        \\"available":[],"stale":[],"blocked":[],
        \\"claimed":[
        \\  {"task":{"id":20,"plan_id":1,"title":"No claim row","slug":null,"status":"in_progress"},
        \\   "claim":null}
        \\],
        \\"summary":{"available":0,"claimed":1,"stale":0,"blocked":0,"done":0}}
    ;
    const parsed = try std.json.parseFromSlice(PlanNext, std.testing.allocator, fixture, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(usize, 1), parsed.value.claimed.len);
    try std.testing.expectEqual(@as(u64, 20), parsed.value.claimed[0].task.id);
    try std.testing.expect(parsed.value.claimed[0].claim == .null);
}

test "testSpecStatus helper: NDJSON fixture — per-plan rows and anchor decoded" {
    // Simulates the full NDJSON output of `planar test-spec status 492 --json`.
    // Real fixture captured 2026-06-03, trimmed to 3 rows + anchor for brevity.
    const ndjson =
        \\{"plan_id":492,"title":"Autonomous workflow harness (planar-execute)","total_tasks":0,"tasks_with_slug":0,"tasks_covered":0,"happy":0,"empty":0,"error":0,"edge":0,"other":0}
        \\{"plan_id":494,"title":"M1 — Binary skeleton + embedded Lua runtime","total_tasks":18,"tasks_with_slug":7,"tasks_covered":1,"happy":1,"empty":0,"error":0,"edge":0,"other":0}
        \\{"plan_id":495,"title":"M2 — Host-function surface, sandbox, brief compiler","total_tasks":7,"tasks_with_slug":5,"tasks_covered":1,"happy":0,"empty":0,"error":0,"edge":1,"other":0}
        \\{"anchor_plan_id":492,"total_tasks":25,"tasks_with_slug":12,"tasks_covered":2,"total_scenarios":3}
    ;
    // Parse line-by-line (same logic as testSpecStatus, exercised without subprocess).
    var per_plan = std.ArrayList(PlanCoverage).empty;
    defer {
        for (per_plan.items) |row| std.testing.allocator.free(row.title);
        per_plan.deinit(std.testing.allocator);
    }
    var maybe_anchor: ?TestSpecAnchor = null;

    var lines = std.mem.splitScalar(u8, ndjson, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (line.len == 0) continue;
        if (std.mem.indexOf(u8, line, "\"anchor_plan_id\"") != null) {
            const p = try std.json.parseFromSlice(TestSpecAnchor, std.testing.allocator, line, .{
                .ignore_unknown_fields = true,
            });
            defer p.deinit();
            maybe_anchor = p.value;
        } else {
            const p = try std.json.parseFromSlice(PlanCoverage, std.testing.allocator, line, .{
                .ignore_unknown_fields = true,
            });
            defer p.deinit();
            const title_copy = try std.testing.allocator.dupe(u8, p.value.title);
            errdefer std.testing.allocator.free(title_copy);
            const row: PlanCoverage = .{
                .plan_id = p.value.plan_id,
                .title = title_copy,
                .total_tasks = p.value.total_tasks,
                .tasks_with_slug = p.value.tasks_with_slug,
                .tasks_covered = p.value.tasks_covered,
                .happy = p.value.happy,
                .empty = p.value.empty,
                .@"error" = p.value.@"error",
                .edge = p.value.edge,
                .other = p.value.other,
            };
            try per_plan.append(std.testing.allocator, row);
        }
    }

    // Validate per-plan rows.
    try std.testing.expectEqual(@as(usize, 3), per_plan.items.len);
    try std.testing.expectEqual(@as(u64, 492), per_plan.items[0].plan_id);
    try std.testing.expectEqualStrings("Autonomous workflow harness (planar-execute)", per_plan.items[0].title);
    try std.testing.expectEqual(@as(u64, 494), per_plan.items[1].plan_id);
    try std.testing.expectEqual(@as(u64, 1), per_plan.items[1].tasks_covered);
    try std.testing.expectEqual(@as(u64, 1), per_plan.items[1].happy);
    try std.testing.expectEqual(@as(u64, 495), per_plan.items[2].plan_id);
    try std.testing.expectEqual(@as(u64, 1), per_plan.items[2].edge);

    // Validate anchor.
    const anchor = maybe_anchor orelse return error.TestUnexpectedNull;
    try std.testing.expectEqual(@as(u64, 492), anchor.anchor_plan_id);
    try std.testing.expectEqual(@as(u64, 25), anchor.total_tasks);
    try std.testing.expectEqual(@as(u64, 2), anchor.tasks_covered);
    try std.testing.expectEqual(@as(u64, 3), anchor.total_scenarios);
}

test "testSpecStatus helper: ignore_unknown_fields on PlanCoverage and TestSpecAnchor" {
    // Proves both struct types tolerate unknown keys (schema drift protection).
    const coverage_line =
        \\{"plan_id":1,"title":"X","total_tasks":5,"tasks_with_slug":3,
        \\"tasks_covered":1,"happy":1,"empty":0,"error":0,"edge":0,"other":0,
        \\"future_coverage_field":"ignored"}
    ;
    const anchor_line =
        \\{"anchor_plan_id":1,"total_tasks":5,"tasks_with_slug":3,
        \\"tasks_covered":1,"total_scenarios":2,"future_anchor_field":true}
    ;

    const pc = try std.json.parseFromSlice(PlanCoverage, std.testing.allocator, coverage_line, .{
        .ignore_unknown_fields = true,
    });
    defer pc.deinit();
    try std.testing.expectEqual(@as(u64, 1), pc.value.plan_id);
    try std.testing.expectEqual(@as(u64, 1), pc.value.tasks_covered);

    const anch = try std.json.parseFromSlice(TestSpecAnchor, std.testing.allocator, anchor_line, .{
        .ignore_unknown_fields = true,
    });
    defer anch.deinit();
    try std.testing.expectEqual(@as(u64, 1), anch.value.anchor_plan_id);
    try std.testing.expectEqual(@as(u64, 2), anch.value.total_scenarios);
}

test "testSpecStatus helper: malformed coverage line → ParseFailed equivalent" {
    const bad = "not json at all";
    const result = std.json.parseFromSlice(PlanCoverage, std.testing.allocator, bad, .{
        .ignore_unknown_fields = true,
    });
    try std.testing.expectError(error.SyntaxError, result);
}
