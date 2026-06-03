//! integration_tests/entity_create_feed_test.zig — end-to-end test for the
//! entity-create feed integration (plan 467 Phase 1, task 3042).
//!
//! Scenario: an agent pulls a task, runs `planar question add` under that
//! claim (with PLANAR_VENDOR + PLANAR_VENDOR_SESSION_ID set so the session
//! resolver finds the active claim), and `planar-watch feed --json` surfaces
//! an action event for the question-create with the claim's id and vendor.
//!
//! Covered assertions:
//!   - `planar-watch feed --json` includes an `action_ended` event whose
//!     `action.entity_kind` = "question" for the newly created question.
//!   - The event's `action.claim_id` matches the claim id from pull.
//!   - The event's `action.vendor` matches the vendor used at pull time.
//!   - The event's `action.summary` contains "created question".
//!   - A well-formed `planar-agent complete` terminates the claim cleanly.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// Binary resolvers — same pattern as planar_watch_test.zig.
// =========================================================================

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

/// Run `bin` with `args` and inject PLANAR_DB from the suite. Additional
/// env vars may be supplied via `extra_env` (key=value pairs to overlay).
fn runBinWith(
    suite: *const harness.Suite,
    bin: []const u8,
    args: []const []const u8,
    extra_env: []const [2][]const u8,
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
    // Mirror harness.execWith: disable the worktree gate so planning
    // verbs (e.g. `question add`) run when the suite itself is executed
    // from a worktree checkout. Without this the gate refuses them and
    // the test crashes only in worktree-based dev (CI runs non-worktree).
    env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1") catch @panic("OOM injecting GATE flag");
    for (extra_env) |kv| {
        env_map.put(kv[0], kv[1]) catch @panic("OOM injecting extra env");
    }

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runBinWith spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunBinWith(
    suite: *const harness.Suite,
    bin: []const u8,
    args: []const []const u8,
    extra_env: []const [2][]const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runBinWith(suite, bin, args, extra_env);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "mustRunBinWith failed (bin={s}, term={any}):\nstdout: {s}\nstderr: {s}\n",
            .{ bin, res.term, res.stdout, res.stderr },
        );
        @panic("mustRunBinWith failed");
    }
    return res.stdout;
}

fn mustRunWatch(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    return mustRunBinWith(suite, resolveWatchBin(), args, &.{});
}

fn mustRunAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    return mustRunBinWith(suite, resolveAgentBin(), args, &.{});
}

/// Seed: init + plan + one todo task. Returns the plan id string (caller frees).
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

// =========================================================================
// Tests
// =========================================================================

test "entity-create feed: question add under active claim surfaces in planar-watch feed" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "ecf-q-feed", "ecf-task");
    defer gpa.free(pid_arg);

    // Step 1: pull a task as vendor "test-claude" so the session is identifiable.
    const vendor_name = "test-claude";
    const vsid = "ecf-session-1";
    const pull_out = mustRunBinWith(&suite, resolveAgentBin(), &.{
        "pull",     pid_arg,     "--no-locality-probe",
        "--vendor", vendor_name, "--vendor-session",
        vsid,       "--role",    "coder",
        "--json",
    }, &.{});
    defer gpa.free(pull_out);

    // Capture claim_token and claim id for later assertions.
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim_token");
    defer gpa.free(token);
    const claim_id = extractIntField(pull_out, "\"claim\":{\"id\"") orelse @panic("no claim.id");

    // Step 2: run `planar question add` with PLANAR_VENDOR + PLANAR_VENDOR_SESSION_ID
    // set to match the pull. The question.add handler resolves the session via
    // engine.runtime.session.ensureActive(vendor, vsid) and passes session_id to
    // question.create so the entity-create hook fires.
    const q_extra_env = [_][2][]const u8{
        .{ "PLANAR_VENDOR", vendor_name },
        .{ "PLANAR_VENDOR_SESSION_ID", vsid },
    };
    const q_out = mustRunBinWith(&suite, suite.bin, &.{
        "question", "add", "--plan", pid_arg, "--json", "ecf-test-question",
    }, &q_extra_env);
    defer gpa.free(q_out);

    // Confirm the question was created.
    try std.testing.expect(std.mem.indexOf(u8, q_out, "\"id\":") != null);

    // Step 3: planar-watch feed --json. Assert on action events.
    const feed_out = mustRunWatch(&suite, &.{ "feed", "--json" });
    defer gpa.free(feed_out);

    // The entity-create hook writes a heartbeat-like action row under the claim.
    // In the feed these appear as action_started / action_ended events.
    // Assert: at least one action_ended event has entity_kind=question + claim_id.
    const claim_id_needle = std.fmt.allocPrint(gpa, "\"claim_id\":{d}", .{claim_id}) catch @panic("OOM");
    defer gpa.free(claim_id_needle);

    // Check that the feed contains the entity-create action for the question.
    // The action row has entity_kind=question and claim_id=<claim_id>.
    // We also verify the vendor matches and summary contains the expected text.
    const has_question_entity = std.mem.indexOf(u8, feed_out, "\"entity_kind\":\"question\"") != null;
    const has_claim_ref = std.mem.indexOf(u8, feed_out, claim_id_needle) != null;
    const has_vendor = std.mem.indexOf(u8, feed_out, vendor_name) != null;
    const has_summary = std.mem.indexOf(u8, feed_out, "created question") != null;

    if (!has_question_entity) {
        std.debug.print(
            "feed missing entity_kind=question event:\n{s}\n",
            .{feed_out},
        );
        return error.MissingQuestionEntityEvent;
    }
    if (!has_claim_ref) {
        std.debug.print(
            "feed missing claim_id={d} in action events:\n{s}\n",
            .{ claim_id, feed_out },
        );
        return error.MissingClaimIdInFeed;
    }
    if (!has_vendor) {
        std.debug.print(
            "feed missing vendor={s}:\n{s}\n",
            .{ vendor_name, feed_out },
        );
        return error.MissingVendorInFeed;
    }
    if (!has_summary) {
        std.debug.print(
            "feed action missing 'created question' in summary:\n{s}\n",
            .{feed_out},
        );
        return error.MissingSummaryInFeed;
    }

    // Step 4: clean up — complete the claim.
    const complete_out = mustRunAgent(&suite, &.{
        "complete", "--claim", token, "--summary", "ecf test done", "--json",
    });
    defer gpa.free(complete_out);
    try std.testing.expect(std.mem.indexOf(u8, complete_out, "\"status\":\"completed\"") != null);
}

test "entity-create feed: question add WITHOUT active claim produces no entity-create action" {
    // Verifies D3 (always-fire when claim exists, silent skip when no claim).
    // When run from a plain CLI session (no PLANAR_VENDOR_SESSION_ID that
    // maps to an active claim), no entity-create action row should appear.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ecf-noclaim", "--json", "ecf-noclaim" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const pid_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(pid_arg);

    // No active claim in the DB for this vendor/session pair.
    // The hook resolves session_id but latestActiveClaimForSession returns null.
    const q_extra_env = [_][2][]const u8{
        .{ "PLANAR_VENDOR", "test-cli-no-claim" },
        .{ "PLANAR_VENDOR_SESSION_ID", "noclaim-session-x" },
    };
    const q_out = mustRunBinWith(&suite, suite.bin, &.{
        "question", "add", "--plan", pid_arg, "--json", "ecf-no-claim-question",
    }, &q_extra_env);
    defer gpa.free(q_out);
    // Question was created.
    try std.testing.expect(std.mem.indexOf(u8, q_out, "\"id\":") != null);

    // The feed should not have any entity_kind=question action (no claim was active).
    const feed_out = mustRunWatch(&suite, &.{ "feed", "--json" });
    defer gpa.free(feed_out);

    // No claim_acquired events either (no pull happened). So no
    // entity-create action rows for question should appear.
    const question_action_count = countOccurrences(feed_out, "\"entity_kind\":\"question\"");
    if (question_action_count != 0) {
        std.debug.print(
            "expected 0 question entity-create actions, got {d}:\n{s}\n",
            .{ question_action_count, feed_out },
        );
        return error.UnexpectedQuestionEntityAction;
    }
}

fn countOccurrences(haystack: []const u8, needle: []const u8) usize {
    var count: usize = 0;
    var i: usize = 0;
    while (i + needle.len <= haystack.len) {
        if (std.mem.startsWith(u8, haystack[i..], needle)) {
            count += 1;
            i += needle.len;
        } else {
            i += 1;
        }
    }
    return count;
}

test "heartbeat --status: payload > 256 bytes is rejected with exit 2" {
    // This tests the task 3038 length cap in a realistic end-to-end scenario:
    // a live claim + an over-length --status → must exit non-zero (code 2)
    // with an actionable error message.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "hb-cap-e2e", "hb-cap-task");
    defer gpa.free(pid_arg);

    const pull_out = mustRunAgent(&suite, &.{
        "pull", pid_arg, "--no-locality-probe", "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    // Construct a 257-byte status string (one byte over the cap).
    var long_status: [257]u8 = undefined;
    @memset(&long_status, 'x');

    const res = runBinWith(&suite, resolveAgentBin(), &.{
        "heartbeat", "--claim", token, "--status", &long_status,
    }, &.{});
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 2), res.term.exited);
    // The error message must mention the byte count and the cap.
    if (std.mem.indexOf(u8, res.stderr, "257") == null) {
        std.debug.print("expected byte count '257' in error:\n{s}\n", .{res.stderr});
        return error.MissingByteCountInError;
    }
    if (std.mem.indexOf(u8, res.stderr, "256") == null) {
        std.debug.print("expected cap '256' in error:\n{s}\n", .{res.stderr});
        return error.MissingCapInError;
    }

    // Claim must still be valid — ROLLBACK happened, heartbeat lease unchanged.
    // A subsequent heartbeat without --status must succeed.
    const hb_ok = mustRunAgent(&suite, &.{ "heartbeat", "--claim", token, "--json" });
    defer gpa.free(hb_ok);
    try std.testing.expect(std.mem.indexOf(u8, hb_ok, "\"ok\":true") != null);

    // Clean up.
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "cap test done", "--json" }));
}

test "heartbeat --status: exactly 256 bytes is accepted" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_arg = seedPlanWithTask(&suite, "hb-cap-ok", "hb-ok-task");
    defer gpa.free(pid_arg);

    const pull_out = mustRunAgent(&suite, &.{
        "pull", pid_arg, "--no-locality-probe", "--json",
    });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);

    // Exactly 256 bytes — must succeed.
    var ok_status: [256]u8 = undefined;
    @memset(&ok_status, 'y');

    const hb_out = mustRunBinWith(&suite, resolveAgentBin(), &.{
        "heartbeat", "--claim", token, "--status", &ok_status, "--json",
    }, &.{});
    defer gpa.free(hb_out);
    try std.testing.expect(std.mem.indexOf(u8, hb_out, "\"ok\":true") != null);

    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "256b test done", "--json" }));
}
