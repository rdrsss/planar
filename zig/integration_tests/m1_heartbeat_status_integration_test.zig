//! integration_tests/m1_heartbeat_status_integration_test.zig
//!
//! End-to-end verification that the M1 write surface (planar-agent
//! heartbeat --status) reaches the M3 read surface (planar-watch ps
//! latest_action.summary field) through the full stack.
//!
//! Scenario (task 3041 — m1-heartbeat-status-integration):
//!   1. planar-agent pull → captures claim_token.
//!   2. planar-agent heartbeat --claim <token> --status "running tests".
//!   3. planar-watch ps --json → the active claim's latest_action.summary
//!      equals "running tests".
//!   4. planar-agent complete → claim closed cleanly.
//!
//! Verifies the end-to-end contract from the test-spec line 30:
//!   "heartbeat --status text appears in ps activity column"

const std = @import("std");
const harness = @import("harness");

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
// Tests
// ==========================================================================

test "m1-heartbeat-status-integration: --status text reaches ps latest_action.summary" {
    // Full end-to-end: pull → heartbeat --status → ps --json asserts
    // latest_action.summary. Verifies the M1 write surface connects to
    // the M3 read surface through the real binary stack.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "hb-status-e2e", "hb-status-task");
    defer gpa.free(pid_arg);

    // Step 1: pull.
    const pull_out = mustRunAgent(&suite, &.{
        "pull", pid_arg, "--no-locality-probe", "--role", "coder", "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim_token");
    defer gpa.free(token);

    // Step 2: heartbeat with --status.
    const status_text = "running tests";
    const hb_out = mustRunAgent(&suite, &.{
        "heartbeat", "--claim", token, "--status", status_text, "--json",
    });
    defer gpa.free(hb_out);
    if (std.mem.indexOf(u8, hb_out, "\"ok\":true") == null) {
        std.debug.print("heartbeat --status failed:\n{s}\n", .{hb_out});
        return error.HeartbeatFailed;
    }

    // Step 3: planar-watch ps --json → active claim has latest_action.summary.
    const ps_out = mustRunWatch(&suite, &.{ "ps", "--json" });
    defer gpa.free(ps_out);

    // The summary text must appear in the JSON output.
    if (std.mem.indexOf(u8, ps_out, "\"latest_action\":") == null) {
        std.debug.print("ps --json missing latest_action field:\n{s}\n", .{ps_out});
        return error.MissingLatestAction;
    }
    if (std.mem.indexOf(u8, ps_out, status_text) == null) {
        std.debug.print(
            "ps --json latest_action.summary missing '{s}':\n{s}\n",
            .{ status_text, ps_out },
        );
        return error.MissingSummaryText;
    }

    // Also verify the text output surfaces the status in the activity column.
    const ps_text_out = mustRunWatch(&suite, &.{"ps"});
    defer gpa.free(ps_text_out);
    if (std.mem.indexOf(u8, ps_text_out, "activity:") == null) {
        std.debug.print("ps text output missing activity: column:\n{s}\n", .{ps_text_out});
        return error.MissingActivityColumn;
    }
    if (std.mem.indexOf(u8, ps_text_out, status_text) == null) {
        std.debug.print(
            "ps text output activity column missing '{s}':\n{s}\n",
            .{ status_text, ps_text_out },
        );
        return error.MissingActivityText;
    }

    // Step 4: complete the claim to leave a clean DB state.
    const complete_out = mustRunAgent(&suite, &.{
        "complete", "--claim", token, "--summary", "hb-status-e2e done", "--json",
    });
    defer gpa.free(complete_out);
    try std.testing.expect(std.mem.indexOf(u8, complete_out, "\"status\":\"completed\"") != null);
}

test "m1-heartbeat-status-integration: latest_action carries {kind, summary, started_at}" {
    // Verifies the JSON shape of latest_action on a ps claim row after
    // a heartbeat --status write.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "hb-shape-e2e", "hb-shape-task");
    defer gpa.free(pid_arg);

    const pull_out = mustRunAgent(&suite, &.{
        "pull", pid_arg, "--no-locality-probe", "--role", "coder", "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim_token");
    defer gpa.free(token);

    const hb_out = mustRunAgent(&suite, &.{
        "heartbeat", "--claim", token, "--status", "shape check", "--json",
    });
    defer gpa.free(hb_out);
    try std.testing.expect(std.mem.indexOf(u8, hb_out, "\"ok\":true") != null);

    const ps_out = mustRunWatch(&suite, &.{ "ps", "--json" });
    defer gpa.free(ps_out);

    // All three required fields must be present in the JSON output.
    const checks = [_][]const u8{
        "\"latest_action\":",
        "\"kind\":",
        "\"summary\":",
        "\"started_at\":",
    };
    for (checks) |needle| {
        if (std.mem.indexOf(u8, ps_out, needle) == null) {
            std.debug.print(
                "ps --json missing '{s}' in latest_action shape:\n{s}\n",
                .{ needle, ps_out },
            );
            return error.MissingLatestActionField;
        }
    }

    // Clean up.
    gpa.free(mustRunAgent(&suite, &.{
        "complete", "--claim", token, "--summary", "shape check done", "--json",
    }));
}
