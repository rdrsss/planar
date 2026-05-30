//! integration_tests/liveness_happy_path_test.zig — plan 493 S5.
//!
//! Regression fence for the normal claim-ritual path after the F1
//! widened predicate (`status in ('todo','doing')`).
//!
//! Test-spec scenario (artifact 266):
//!   S5 — `liveness-happy-path-unchanged`
//!
//! Asserts two invariants:
//!
//!   1. Exclusivity: a `doing` task that has an unexpired active claim is
//!      NOT re-selected by a subsequent `pull`. Guards against the F1
//!      predicate over-broadening to cover in-flight work.
//!
//!   2. Post-done non-reselection: after `complete`, a follow-up `pull`
//!      on the same plan selects a second fresh `todo` task — NOT the
//!      now-done one. Guards against the predicate over-broadening to
//!      include `done` tasks.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// planar-agent shell-out (mirrors liveness_repull_test.zig conventions)
// =========================================================================

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

fn runAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;
    const agent_bin = resolveAgentBin();

    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, agent_bin) catch @panic("OOM");
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
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-agent failed (term={any}): {s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

// =========================================================================
// Fixture helpers
// =========================================================================

/// Seed scratch DB with init + plan + one todo task. Returns the plan id
/// as a heap-allocated decimal string (caller frees via `gpa.free`).
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

/// Add a second task to an existing plan (no re-init). Returns task id as
/// a heap-allocated decimal string (caller frees via `gpa.free`).
fn addTaskToPlan(suite: *const harness.Suite, plan_id_arg: []const u8, task_title: []const u8) []u8 {
    const gpa = suite.allocator;
    const task_out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "--json", task_title });
    defer gpa.free(task_out);
    const task_id = extractIntField(task_out, "\"id\"") orelse @panic("no task id from add");
    return std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch @panic("OOM");
}

// =========================================================================
// S5a — Exclusivity invariant: in-flight task is NOT re-pulled.
// slug: liveness-happy-path-unchanged
// =========================================================================

test "S5a liveness-happy-path-unchanged: in-flight task with active claim is NOT re-selected (plan 493 S5)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed with ONE task. Pull it with a long TTL so the claim stays active
    // and unexpired for the duration of the test.
    const pid_arg = seedPlanWithTask(&suite, "ag-s5a-excl", "in-flight-task");
    defer gpa.free(pid_arg);

    // Pull task T — now doing with an unexpired active claim.
    const pull1 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "600", "--json" });
    defer gpa.free(pull1);

    if (std.mem.indexOf(u8, pull1, "\"no_work\":true") != null) {
        std.debug.print("S5a setup: pull returned no_work on a fresh todo task; pull1: {s}\n", .{pull1});
        @panic("S5a setup failed: pull should have selected the only todo task");
    }
    const first_task_id = extractIntField(pull1, "\"task\":{\"id\"") orelse @panic("no task.id on first pull");
    const first_token = extractStringField(gpa, pull1, "\"claim_token\":\"") catch @panic("no claim_token on first pull");
    defer gpa.free(first_token);

    // CONTRACT: a second pull on the same plan MUST return no_work.
    // The task is `doing` with an unexpired active claim — F1's widened
    // predicate must NOT re-select it. Pre-F1 this was also no_work (the
    // old predicate required `todo`); post-F1 the widened predicate must
    // still exclude it because the exclusion gate is the unexpired active
    // claim, not the status.
    const pull2 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "600", "--json" });
    defer gpa.free(pull2);

    if (std.mem.indexOf(u8, pull2, "\"no_work\":true") == null) {
        std.debug.print(
            "S5a RED: pull selected the in-flight task a second time!\nfirst_task_id={d}\npull2: {s}\n",
            .{ first_task_id, pull2 },
        );
        return error.ExclusivityViolation;
    }

    // Also verify peek returns no_work for the same reason.
    const peek_out = mustRunAgent(&suite, &.{ "peek", pid_arg, "--json" });
    defer gpa.free(peek_out);

    if (std.mem.indexOf(u8, peek_out, "\"no_work\":true") == null) {
        std.debug.print(
            "S5a RED: peek selected the in-flight task!\nfirst_task_id={d}\npeek_out: {s}\n",
            .{ first_task_id, peek_out },
        );
        return error.PeekExclusivityViolation;
    }

    // Release the claim so the suite's DB doesn't hold a dangling active claim.
    const release_out = mustRunAgent(&suite, &.{ "release", "--claim", first_token, "--json" });
    gpa.free(release_out);
}

// =========================================================================
// S5b — Post-done non-reselection: completed task is never re-selected.
// slug: liveness-happy-path-unchanged
// =========================================================================

test "S5b liveness-happy-path-unchanged: completed task is not re-selected; next todo is (plan 493 S5)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed with task T1 (first). Then add task T2 (second). Pull T1 first.
    const pid_arg = seedPlanWithTask(&suite, "ag-s5b-done", "task-T1");
    defer gpa.free(pid_arg);
    const t2_id_arg = addTaskToPlan(&suite, pid_arg, "task-T2");
    defer gpa.free(t2_id_arg);

    // Pull -> heartbeat -> complete the full ritual for T1.
    const pull1 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "600", "--json" });
    defer gpa.free(pull1);

    if (std.mem.indexOf(u8, pull1, "\"no_work\":true") != null) {
        std.debug.print("S5b setup: first pull returned no_work; pull1: {s}\n", .{pull1});
        @panic("S5b setup failed: expected T1 to be selected");
    }
    const t1_id = extractIntField(pull1, "\"task\":{\"id\"") orelse @panic("no task.id on T1 pull");
    const t1_token = extractStringField(gpa, pull1, "\"claim_token\":\"") catch @panic("no claim_token on T1 pull");
    defer gpa.free(t1_token);

    // Heartbeat (routine mid-task pulse).
    const hb_out = mustRunAgent(&suite, &.{ "heartbeat", "--claim", t1_token, "--ttl", "600", "--json" });
    gpa.free(hb_out);

    // Complete T1. This flips task -> done, claim -> completed atomically.
    const complete_out = mustRunAgent(&suite, &.{ "complete", "--claim", t1_token, "--json" });
    defer gpa.free(complete_out);

    // Assert T1 is `done` post-complete.
    const t1_id_arg = std.fmt.allocPrint(gpa, "{d}", .{t1_id}) catch @panic("OOM");
    defer gpa.free(t1_id_arg);
    const t1_show = suite.mustRun(&.{ "task", "show", "--json", t1_id_arg });
    defer gpa.free(t1_show);
    if (std.mem.indexOf(u8, t1_show, "\"status\":\"done\"") == null) {
        std.debug.print(
            "S5b: T1 did not transition to done after complete; t1_show: {s}\n",
            .{t1_show},
        );
        @panic("S5b: complete did not flip T1 to done");
    }
    // Assert the claim is `completed`.
    if (std.mem.indexOf(u8, complete_out, "\"status\":\"completed\"") == null) {
        std.debug.print(
            "S5b: claim status is not completed after complete; complete_out: {s}\n",
            .{complete_out},
        );
        @panic("S5b: complete did not flip claim to completed");
    }

    // CONTRACT: the next pull MUST select T2 (the remaining todo task),
    // NOT T1 (which is now done). Guards against the F1 predicate
    // accidentally including `done` tasks in its widened `status in
    // ('todo','doing')` expression.
    const pull2 = mustRunAgent(&suite, &.{ "pull", pid_arg, "--no-locality-probe", "--ttl", "60", "--json" });
    defer gpa.free(pull2);

    if (std.mem.indexOf(u8, pull2, "\"no_work\":true") != null) {
        std.debug.print(
            "S5b RED: second pull returned no_work -- T2 was not selected.\npull2: {s}\n",
            .{pull2},
        );
        return error.SecondTodoNotSelected;
    }

    const t2_selected_id = extractIntField(pull2, "\"task\":{\"id\"") orelse @panic("no task.id on second pull");
    const t2_id_parsed = std.fmt.parseInt(i64, t2_id_arg, 10) catch @panic("bad t2_id_arg");

    if (t2_selected_id == t1_id) {
        std.debug.print(
            "S5b RED: second pull re-selected done task T1 (id={d}) instead of T2.\n",
            .{t1_id},
        );
        return error.DoneTaskReselected;
    }
    if (t2_selected_id != t2_id_parsed) {
        std.debug.print(
            "S5b: second pull selected unexpected task {d} (expected T2={d}).\n",
            .{ t2_selected_id, t2_id_parsed },
        );
        return error.UnexpectedTaskSelected;
    }

    // Release T2's claim to leave the DB clean.
    const t2_token = extractStringField(gpa, pull2, "\"claim_token\":\"") catch @panic("no claim_token on T2 pull");
    defer gpa.free(t2_token);
    const release_out = mustRunAgent(&suite, &.{ "release", "--claim", t2_token, "--json" });
    gpa.free(release_out);
}

// =========================================================================
// JSON helpers (mirrored from liveness_repull_test.zig)
// =========================================================================

fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

fn extractStringField(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse return error.FieldNotFound;
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    if (end == json.len) return error.UnterminatedString;
    return try gpa.dupe(u8, json[i..end]);
}
