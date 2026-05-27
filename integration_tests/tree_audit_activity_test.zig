//! integration_tests/tree_audit_activity_test.zig
//!
//! Black-box coverage for the M5 fold-ins (plan 85):
//!
//!   - `planar tree --json` includes `activity_summary` on entity rows
//!     once an `agent_actions` / `agent_work_claims` row exists for the
//!     entity (task:tree-activity-summary).
//!   - `planar tree` and `planar tree --json` produce CLEAN output (no
//!     empty activity placeholders) when no activity has been recorded
//!     (task:activity-summary-empty-degrade).
//!   - `planar audit trail <kind:id>` appends an "Agent activity" text
//!     section + an `agent_activity` JSON sub-object when activity exists
//!     (task:audit-trail-agent-activity).
//!   - `planar audit trail <kind:id>` does NOT print "Agent activity:"
//!     when nothing is recorded (task:activity-summary-empty-degrade).
//!
//! And methodology / vendor-surface namespace purity:
//!
//!   - The canonical agent docs (`agents/methodology.md`, `orchestrator.md`,
//!     `coder.md`, `reviewer.md`) reference `planar-agent pull` /
//!     `complete` / `fail` / `release` / `block` and do NOT reference
//!     any `planar agent <verb>` subcommand (task:methodology-claim-
//!     ritual, task:vendor-surfaces-claim-aware,
//!     task:orchestrator-parallelism-aware).
//!   - The orchestrator surfaces specifically reference `planar-agent
//!     peek` (the parallelism-aware dry-run before dispatch).
//!   - The docs do NOT reference `planar-agent ps` / `log` / `tail`
//!     (those live on `planar-watch`, coming in M8).

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// Helpers
// =========================================================================

const PlanJSON = struct { id: i64 };
const TaskJSON = struct { id: i64 };

fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic("PLANAR_AGENT_BIN not set; run via `make test-integration`");
}

fn runAgent(suite: *const harness.Suite, args: []const []const u8) harness.Suite.RunResult {
    const gpa = suite.allocator;
    const bin = resolveAgentBin();
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, bin) catch @panic("OOM");
    for (args) |a| argv_list.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-agent failed (term={any}): {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

// =========================================================================
// tree — empty degrade
// =========================================================================

test "tree --json on a plan with NO agent activity: no activity_summary keys" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tree-no-act", "--json", "TREE_NO_ACT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "no-act-task",
    });

    const raw = suite.mustRun(&.{ "tree", "--depth", "3", "--json" });
    defer gpa.free(raw);
    // No activity has been recorded — the `activity_summary` JSON key
    // MUST NOT appear (silent degrade). This pins the
    // task:activity-summary-empty-degrade contract for tree.
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"activity_summary\"") == null);
}

test "tree text on a plan with NO agent activity: no 'activity:' sub-line" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tree-no-act-text", "--json", "TREE_NO_ACT_TEXT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "no-act-task-text",
    });

    const raw = suite.mustRun(&.{ "tree", "--depth", "3" });
    defer gpa.free(raw);
    // No "activity:" sub-line: the text renderer degrades silently when
    // the entity has no agent_actions / agent_work_claims rows.
    try std.testing.expect(std.mem.indexOf(u8, raw, "activity:") == null);
}

// =========================================================================
// tree — happy path: activity_summary present on claimed task
// =========================================================================

test "tree --json on a plan with active claim: activity_summary present on task row" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tree-active", "--json", "TREE_ACT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "claimable-task",
    });

    // Seed an active claim via planar-agent pull. pull writes both an
    // agent_actions row AND an agent_work_claims row.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--role", "coder", "--json" }));

    const raw = suite.mustRun(&.{ "tree", "--depth", "3", "--json" });
    defer gpa.free(raw);
    // The key MUST appear at least once now (on the claimed task row).
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"activity_summary\"") != null);
    // Latest action_kind is the role `pull` recorded — `coder`.
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"latest_action_kind\":\"coder\"") != null);
    // Active claim count for the task is 1.
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"active_claim_count\":1") != null);
}

test "tree text on a plan with active claim: 'activity:' sub-line present" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tree-active-text", "--json", "TREE_ACT_TEXT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "claimable-task-text",
    });
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--role", "coder", "--json" }));

    const raw = suite.mustRun(&.{ "tree", "--depth", "3" });
    defer gpa.free(raw);
    try std.testing.expect(std.mem.indexOf(u8, raw, "activity: coder") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "[claims:1]") != null);
}

// =========================================================================
// audit trail — empty degrade
// =========================================================================

test "audit trail on a task with NO agent activity: no 'Agent activity:' section" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "at-no-act", "--json", "AT_NO_ACT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "no-act-trail-task",
    });
    const tid_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    const raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", tid_str });
    defer gpa.free(raw);
    // Section header MUST NOT appear when there's no agent activity.
    try std.testing.expect(std.mem.indexOf(u8, raw, "Agent activity:") == null);

    const json_raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", "--json", tid_str });
    defer gpa.free(json_raw);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"agent_activity\"") == null);
}

// =========================================================================
// audit trail — happy path
// =========================================================================

test "audit trail on a task with agent activity: 'Agent activity:' section + agent_activity JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "at-with-act", "--json", "AT_WITH_ACT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "trail-with-act-task",
    });
    const tid_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    // pull → claim + action → still active.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--role", "coder", "--json" }));

    const raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", tid_str });
    defer gpa.free(raw);
    try std.testing.expect(std.mem.indexOf(u8, raw, "Agent activity:") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "actions (") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "claims (") != null);

    const json_raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", "--json", tid_str });
    defer gpa.free(json_raw);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"agent_activity\":{\"actions\":[") != null);
    // The vendor recorded by pull (no --vendor-session) is the default
    // `planar` adapter; assert presence rather than exact value to
    // stay resilient to the default-vendor convention.
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"claims\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"action_kind\":\"coder\"") != null);
}

// =========================================================================
// Namespace purity — methodology + vendor surfaces
// =========================================================================

/// Resolve a path relative to the repository root by walking upward
/// from `PLANAR_BIN`'s parent until `agents/methodology.md` is found.
/// `make test-integration` exports `PLANAR_BIN=<repo>/bin/planar` so
/// the repo root is exactly two levels above the binary file. The
/// search loop is defensive in case the binary moves out of `<repo>/
/// bin/`. Returns an absolute path the caller owns.
fn resolveRepoPath(gpa: std.mem.Allocator, rel: []const u8) ![]u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var bin_path: ?[]const u8 = null;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_BIN=")) {
            bin_path = s["PLANAR_BIN=".len..];
            break;
        }
    }
    if (bin_path == null) return error.NoPlanarBin;

    var cwd: []const u8 = bin_path.?;
    var depth: usize = 0;
    while (depth < 12) : (depth += 1) {
        cwd = std.fs.path.dirname(cwd) orelse return error.RepoRootNotFound;
        const probe = try std.fs.path.join(gpa, &.{ cwd, "agents", "methodology.md" });
        defer gpa.free(probe);
        // Probe via std.Io.Dir.openFileAbsolute — accepts an absolute
        // path. Successful open ⇒ this is the repo root.
        var file = std.Io.Dir.openFileAbsolute(std.testing.io, probe, .{}) catch continue;
        file.close(std.testing.io);
        return try std.fs.path.join(gpa, &.{ cwd, rel });
    }
    return error.RepoRootNotFound;
}

fn readRepoFile(gpa: std.mem.Allocator, rel: []const u8) ![]u8 {
    const abs = try resolveRepoPath(gpa, rel);
    defer gpa.free(abs);
    var file = try std.Io.Dir.openFileAbsolute(std.testing.io, abs, .{});
    defer file.close(std.testing.io);
    var reader = file.reader(std.testing.io, &.{});
    return try reader.interface.allocRemaining(gpa, std.Io.Limit.limited(1 * 1024 * 1024));
}

test "methodology docs reference `planar-agent pull` and the canonical terminal verbs" {
    const gpa = std.testing.allocator;
    const files = [_][]const u8{
        "agents/methodology.md",
        "agents/orchestrator.md",
        "agents/coder.md",
        "agents/reviewer.md",
    };
    // The methodology file MUST mention `planar-agent pull` and each
    // of complete / fail / release / block. The supporting role docs
    // MUST reference `planar-agent` at least once each.
    {
        const body = try readRepoFile(gpa, files[0]);
        defer gpa.free(body);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent pull") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent heartbeat") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent complete") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent fail") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent release") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent block") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent reconcile") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent abort") != null);
    }
    for (files[1..]) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent") != null);
    }
}

test "methodology + role docs contain ZERO `planar agent <verb>` references" {
    const gpa = std.testing.allocator;
    const files = [_][]const u8{
        "agents/methodology.md",
        "agents/orchestrator.md",
        "agents/coder.md",
        "agents/reviewer.md",
        "agents/test-coder.md",
        "skills/src/pl-orchestrator.md",
        "skills/src/pl-coder.md",
        "skills/src/pl-reviewer.md",
        "skills/src/pl-test-coder.md",
    };
    // Forbidden literal verb references. Each scan looks for the
    // exact substring; phrases like "no `planar agent` subcommand"
    // intentionally do NOT match these (the verb name follows a
    // space-then-letter in the forbidden patterns).
    const forbidden = [_][]const u8{
        "planar agent claim",
        "planar agent ps",
        "planar agent log",
        "planar agent tail",
        "planar agent reconcile",
        "planar agent abort",
        "planar agent start",
        "planar agent end",
        "planar agent pull",
        "planar agent peek",
        "planar agent complete",
        "planar agent fail",
        "planar agent release",
        "planar agent block",
        "planar agent heartbeat",
    };
    for (files) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        for (forbidden) |needle| {
            if (std.mem.indexOf(u8, body, needle)) |hit| {
                std.debug.print(
                    "namespace-purity violation in {s}: found '{s}' at offset {d}\n",
                    .{ rel, needle, hit },
                );
                return error.NamespaceImpurity;
            }
        }
    }
}

test "orchestrator surfaces reference `planar-agent peek` for parallelism-aware dispatch" {
    const gpa = std.testing.allocator;
    const files = [_][]const u8{
        "agents/orchestrator.md",
        "skills/src/pl-orchestrator.md",
    };
    for (files) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent peek") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "parallelism-aware") != null);
    }
}

test "methodology + role docs do NOT reference `planar-agent ps` / log / tail (those live on planar-watch)" {
    const gpa = std.testing.allocator;
    const files = [_][]const u8{
        "agents/methodology.md",
        "agents/orchestrator.md",
        "agents/coder.md",
        "agents/reviewer.md",
        "skills/src/pl-orchestrator.md",
        "skills/src/pl-coder.md",
        "skills/src/pl-reviewer.md",
    };
    const forbidden = [_][]const u8{
        "planar-agent ps",
        "planar-agent log",
        "planar-agent tail",
    };
    for (files) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        for (forbidden) |needle| {
            if (std.mem.indexOf(u8, body, needle)) |hit| {
                std.debug.print(
                    "binary-boundary violation in {s}: found '{s}' at offset {d}\n",
                    .{ rel, needle, hit },
                );
                return error.BinaryBoundaryViolation;
            }
        }
    }
}
