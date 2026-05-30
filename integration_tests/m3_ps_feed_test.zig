//! integration_tests/m3_ps_feed_test.zig
//!
//! End-to-end integration tests for the M3 read surface added in cycles 8-9
//! of plan 467 (task 3063 — m3-ps-feed-integration-tests).
//!
//! Assertions covered (per the task brief):
//!   1. `planar-watch ps` text output contains "activity:" column.
//!   2. `planar-watch ps` text output contains "worktree:" column.
//!   3. `planar-watch ps` text output contains "last_hb:" column.
//!   4. `planar-watch ps --json` carries `latest_action` field on each active
//!      claim (key present; null is OK when no action exists).
//!   5. `planar-watch ps --json` `latest_action` carries {kind, summary,
//!      started_at} shape when an action exists (after heartbeat --status).
//!   6. `planar-watch feed --tail 5` returns at most 5 events.
//!   7. `planar-watch ps --group-by role` text contains role section headers
//!      in the `[group: <key>]` format.
//!   8. `planar-watch ps --json --group-by role` carries a `groups: {...}`
//!      envelope.
//!
//! Each test walks a realistic CLI workflow per CLAUDE.md §Integration test
//! methodology. No raw SQL; all fixture state is built via the CLI binaries.

const std = @import("std");
const harness = @import("harness");

// ==========================================================================
// Binary resolvers — same pattern as planar_watch_test.zig.
// ==========================================================================

fn resolveWatchBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_WATCH_BIN=")) {
            return s["PLANAR_WATCH_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_WATCH_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_AGENT_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

fn runBin(
    suite: *const harness.Suite,
    bin: []const u8,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;

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
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM injecting PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runBin spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunBin(
    suite: *const harness.Suite,
    bin: []const u8,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runBin(suite, bin, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "mustRunBin failed (bin={s}, term={any}):\nstdout: {s}\nstderr: {s}\n",
            .{ bin, res.term, res.stdout, res.stderr },
        );
        @panic("mustRunBin failed");
    }
    return res.stdout;
}

fn mustRunWatch(suite: *const harness.Suite, args: []const []const u8) []u8 {
    return mustRunBin(suite, resolveWatchBin(), args);
}

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    return mustRunBin(suite, resolveAgentBin(), args);
}

fn seedPlanWithTask(suite: *const harness.Suite, plan_slug: []const u8, task_title: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", plan_slug, "--json", plan_slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    const task_out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, task_title });
    gpa.free(task_out);
    return plan_id_arg;
}

fn extractIntField(json_text: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json_text, key) orelse return null;
    var i = idx + key.len;
    while (i < json_text.len and (json_text[i] == ' ' or json_text[i] == ':' or json_text[i] == '\t')) i += 1;
    var end = i;
    while (end < json_text.len and json_text[end] >= '0' and json_text[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json_text[i..end], 10) catch null;
}

fn extractStringField(gpa: std.mem.Allocator, json_text: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json_text, prefix) orelse return error.FieldNotFound;
    const i = idx + prefix.len;
    var end = i;
    while (end < json_text.len and json_text[end] != '"') end += 1;
    if (end == json_text.len) return error.UnterminatedString;
    return try gpa.dupe(u8, json_text[i..end]);
}

// ==========================================================================
// Test 1+2+3: ps text output columns (activity, worktree, last_hb)
// ==========================================================================

test "m3-ps: text output contains activity, worktree, and last_hb columns" {
    // A single active claim is enough to assert all three text columns.
    // We use --worktree to exercise the worktree column.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "m3-ps-text-cols", "m3-text-task");
    defer gpa.free(pid_arg);

    // Pull with an explicit worktree path so worktree column is non-empty.
    const worktree_path = "/tmp/.worktrees/m3-feature-branch";
    const pull_out = mustRunAgent(&suite, &.{
        "pull",                pid_arg,
        "--no-locality-probe", "--worktree",
        worktree_path,         "--role",
        "coder",               "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim_token");
    defer gpa.free(token);

    const ps_text = mustRunWatch(&suite, &.{"ps"});
    defer gpa.free(ps_text);

    // Assertion 1: activity column present.
    if (std.mem.indexOf(u8, ps_text, "activity:") == null) {
        std.debug.print("ps text missing 'activity:' column:\n{s}\n", .{ps_text});
        return error.MissingActivityColumn;
    }

    // Assertion 2: worktree column present.
    if (std.mem.indexOf(u8, ps_text, "worktree:") == null) {
        std.debug.print("ps text missing 'worktree:' column:\n{s}\n", .{ps_text});
        return error.MissingWorktreeColumn;
    }

    // Assertion 3: last_hb column present.
    if (std.mem.indexOf(u8, ps_text, "last_hb:") == null) {
        std.debug.print("ps text missing 'last_hb:' column:\n{s}\n", .{ps_text});
        return error.MissingLastHbColumn;
    }

    // The worktree basename ("m3-feature-branch") should appear since the
    // full path "/tmp/.worktrees/m3-feature-branch" is longer than 40 chars
    // (it's 36 chars total but basename logic always extracts the final segment).
    if (std.mem.indexOf(u8, ps_text, "m3-feature-branch") == null) {
        std.debug.print("ps text missing worktree basename 'm3-feature-branch':\n{s}\n", .{ps_text});
        return error.MissingWorktreeBasename;
    }

    // Clean up.
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "text cols done", "--json" }));
}

// ==========================================================================
// Test 4: ps --json carries latest_action field (null when no action)
// ==========================================================================

test "m3-ps: --json carries latest_action field (null when no action yet)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "m3-ps-json-null-action", "m3-null-action-task");
    defer gpa.free(pid_arg);

    // Pull but do NOT heartbeat with --status. The initial pull itself
    // may create an action row; latest_action key must be present.
    const pull_out = mustRunAgent(&suite, &.{
        "pull", pid_arg, "--no-locality-probe", "--role", "coder", "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim_token");
    defer gpa.free(token);

    const ps_out = mustRunWatch(&suite, &.{ "ps", "--json" });
    defer gpa.free(ps_out);

    // The key must be present; its value may be null or an object.
    if (std.mem.indexOf(u8, ps_out, "\"latest_action\":") == null) {
        std.debug.print("ps --json missing 'latest_action' key:\n{s}\n", .{ps_out});
        return error.MissingLatestActionKey;
    }

    // Clean up.
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "null action done", "--json" }));
}

// ==========================================================================
// Test 5: ps --json latest_action carries {kind, summary, started_at}
// ==========================================================================

test "m3-ps: --json latest_action shape has kind + summary + started_at when action exists" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "m3-ps-json-action-shape", "m3-action-shape-task");
    defer gpa.free(pid_arg);

    const pull_out = mustRunAgent(&suite, &.{
        "pull", pid_arg, "--no-locality-probe", "--role", "coder", "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim_token");
    defer gpa.free(token);

    // Write a --status action so latest_action is non-null.
    gpa.free(mustRunAgent(&suite, &.{
        "heartbeat", "--claim", token, "--status", "building zig", "--json",
    }));

    const ps_out = mustRunWatch(&suite, &.{ "ps", "--json" });
    defer gpa.free(ps_out);

    // Verify latest_action is non-null and carries the three required fields.
    if (std.mem.indexOf(u8, ps_out, "\"latest_action\":null") != null) {
        // latest_action is null even though a heartbeat --status was sent.
        // This would be a regression.
        std.debug.print("ps --json has latest_action=null after heartbeat --status:\n{s}\n", .{ps_out});
        return error.LatestActionShouldNotBeNull;
    }
    if (std.mem.indexOf(u8, ps_out, "\"latest_action\":") == null) {
        std.debug.print("ps --json missing latest_action field entirely:\n{s}\n", .{ps_out});
        return error.MissingLatestAction;
    }
    // All three required sub-fields.
    for ([_][]const u8{ "\"kind\":", "\"summary\":", "\"started_at\":" }) |needle| {
        if (std.mem.indexOf(u8, ps_out, needle) == null) {
            std.debug.print(
                "ps --json latest_action missing '{s}':\n{s}\n",
                .{ needle, ps_out },
            );
            return error.MissingLatestActionField;
        }
    }
    // The summary text must appear.
    if (std.mem.indexOf(u8, ps_out, "building zig") == null) {
        std.debug.print(
            "ps --json latest_action.summary missing 'building zig':\n{s}\n",
            .{ps_out},
        );
        return error.MissingSummaryInLatestAction;
    }

    // Clean up.
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "action shape done", "--json" }));
}

// ==========================================================================
// Test 6: feed --tail 5 returns at most 5 events
// ==========================================================================

test "m3-feed: --tail 5 returns at most 5 events" {
    // Generate more than 5 events then assert --tail 5 caps the result.
    // We create 6 questions under an active claim (each produces an
    // entity-create action event visible in the feed).
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "m3-feed-tail", "m3-tail-task");
    defer gpa.free(pid_arg);

    const vendor_name = "test-tail-vendor";
    const vsid = "tail-session-1";
    const pull_out = mustRunBin(&suite, resolveAgentBin(), &.{
        "pull",     pid_arg,     "--no-locality-probe",
        "--vendor", vendor_name, "--vendor-session",
        vsid,       "--role",    "coder",
        "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim_token");
    defer gpa.free(token);

    // Heartbeat a few times to generate extra action events beyond the 5 tail.
    // pull itself creates claim_acquired + action_started events.
    // Each heartbeat --status adds another action row.
    for (0..6) |idx| {
        const status = std.fmt.allocPrint(gpa, "status-{d}", .{idx}) catch @panic("OOM");
        defer gpa.free(status);
        gpa.free(mustRunAgent(&suite, &.{
            "heartbeat", "--claim", token, "--status", status, "--json",
        }));
    }

    // planar-watch feed --tail 5 --json: must return at most 5 lines.
    const feed_out = mustRunWatch(&suite, &.{ "feed", "--tail", "5", "--json" });
    defer gpa.free(feed_out);

    // Count non-empty JSON lines.
    var line_count: usize = 0;
    var line_it = std.mem.tokenizeAny(u8, feed_out, "\n");
    while (line_it.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t");
        if (trimmed.len == 0) continue;
        line_count += 1;
    }

    if (line_count > 5) {
        std.debug.print(
            "feed --tail 5 returned {d} lines (expected <=5):\n{s}\n",
            .{ line_count, feed_out },
        );
        return error.TailExceeded;
    }

    // Clean up.
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "tail test done", "--json" }));
}

// ==========================================================================
// Test 7: ps --group-by role text contains role section headers
// ==========================================================================

test "m3-ps: --group-by role text output contains [group: <role>] headers" {
    // Seed two claims with different roles (coder + reviewer) under the
    // same plan. ps --group-by role must show two distinct [group: ...] headers.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Init once, create plan, add two tasks.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "m3-grp-role", "--json", "m3-grp-role" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const pid_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(pid_arg);

    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", pid_arg, "grp-coder-task" }));
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", pid_arg, "grp-reviewer-task" }));

    // Pull task 1 as coder, task 2 as reviewer. Use separate vendor-session
    // IDs so the claim resolver doesn't coalesce them.
    const pull_coder = mustRunAgent(&suite, &.{
        "pull",                pid_arg,
        "--no-locality-probe", "--role",
        "coder",               "--vendor-session",
        "grp-session-coder",   "--json",
    });
    defer gpa.free(pull_coder);
    const token_coder = extractStringField(gpa, pull_coder, "\"claim_token\":\"") catch @panic("no coder token");
    defer gpa.free(token_coder);

    const pull_reviewer = mustRunAgent(&suite, &.{
        "pull",                 pid_arg,
        "--no-locality-probe",  "--role",
        "reviewer",             "--vendor-session",
        "grp-session-reviewer", "--json",
    });
    defer gpa.free(pull_reviewer);
    const token_reviewer = extractStringField(gpa, pull_reviewer, "\"claim_token\":\"") catch @panic("no reviewer token");
    defer gpa.free(token_reviewer);

    const ps_text = mustRunWatch(&suite, &.{ "ps", "--group-by", "role" });
    defer gpa.free(ps_text);

    // Both role groups must appear as section headers.
    if (std.mem.indexOf(u8, ps_text, "[group: coder]") == null) {
        std.debug.print("ps --group-by role missing '[group: coder]' header:\n{s}\n", .{ps_text});
        return error.MissingCoderGroupHeader;
    }
    if (std.mem.indexOf(u8, ps_text, "[group: reviewer]") == null) {
        std.debug.print("ps --group-by role missing '[group: reviewer]' header:\n{s}\n", .{ps_text});
        return error.MissingReviewerGroupHeader;
    }

    // Clean up.
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token_coder, "--summary", "grp coder done", "--json" }));
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token_reviewer, "--summary", "grp reviewer done", "--json" }));
}

// ==========================================================================
// Test 8: ps --json --group-by role carries groups:{} envelope
// ==========================================================================

test "m3-ps: --json --group-by role output carries groups envelope" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "m3-grp-json", "--json", "m3-grp-json" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const pid_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(pid_arg);

    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", pid_arg, "json-grp-task" }));

    // A single claim is sufficient to assert the groups:{} envelope shape.
    const pull_out = mustRunAgent(&suite, &.{
        "pull",                pid_arg,
        "--no-locality-probe", "--role",
        "coder",               "--vendor-session",
        "json-grp-session",    "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim_token");
    defer gpa.free(token);

    const ps_json = mustRunWatch(&suite, &.{ "ps", "--json", "--group-by", "role" });
    defer gpa.free(ps_json);

    // The groups envelope must be present.
    if (std.mem.indexOf(u8, ps_json, "\"groups\":") == null) {
        std.debug.print("ps --json --group-by role missing 'groups' envelope:\n{s}\n", .{ps_json});
        return error.MissingGroupsEnvelope;
    }

    // The coder group key must appear.
    if (std.mem.indexOf(u8, ps_json, "\"coder\":[") == null) {
        std.debug.print("ps --json --group-by role missing 'coder' group key:\n{s}\n", .{ps_json});
        return error.MissingCoderGroupKey;
    }

    // ClaimRow fields must be present inside the group.
    if (std.mem.indexOf(u8, ps_json, "\"claim_token\":\"") == null) {
        std.debug.print("ps --json --group-by role missing claim_token inside group:\n{s}\n", .{ps_json});
        return error.MissingClaimTokenInGroup;
    }

    // latest_action field must be present on the grouped claim row.
    if (std.mem.indexOf(u8, ps_json, "\"latest_action\":") == null) {
        std.debug.print("ps --json --group-by role missing latest_action field:\n{s}\n", .{ps_json});
        return error.MissingLatestActionInGroup;
    }

    // Clean up.
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "json grp done", "--json" }));
}
