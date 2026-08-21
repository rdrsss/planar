//! integration_tests/planar_agent_context_test.zig — black-box scenario tests
//! for `planar-agent context add`, `context list`, `context resolve`, and
//! `context capsule`.
//!
//! Per CLAUDE.md § integration test methodology: each test walks a realistic
//! worker workflow through many verbs, asserts post-state via JSON output,
//! and does NOT rely only on exit code 0.
//!
//! Covered scenarios:
//!
//!   - Scenario A (happy path add + list): acquire a run-associated claim,
//!     `context add --claim … --kind finding --body "…"`, then
//!     `context list --run <id>` asserts the stamped row (run_id, stage,
//!     session_id, claim_id, kind, status=active).
//!
//!   - Scenario B (capsule with compiled-from): add two raw records, then
//!     add a capsule with --compiled-from pointing to their ids; list asserts
//!     the compiled_from field is non-null.
//!
//!   - Scenario C (resolve single record): add a record, resolve it to
//!     consumed; list --status consumed asserts the record appears.
//!
//!   - Scenario D (resolve bulk stage sweep): add two records in stage=plan,
//!     one in stage=code; resolve --run … --stage plan --status superseded;
//!     list --stage plan --status active asserts empty; list --stage code
//!     still has the active record.
//!
//!   - Scenario E (context add on non-run claim errors clearly): acquire a
//!     claim WITHOUT --run; `context add` must fail with a non-zero exit and
//!     a message mentioning run_id.
//!
//!   - Scenario F (Q603 order: resolve then capsule): add three raw records,
//!     bulk-resolve the stage to consumed, then write a capsule via
//!     `context capsule --run … --stage … --body … --compiled-from …`; asserts
//!     (1) raw records are consumed, (2) capsule is active, (3) compiled_from
//!     carries provenance ids, (4) claim_id is null on the capsule.

const std = @import("std");
const harness = @import("harness");

/// Resolve the planar-agent binary from PLANAR_AGENT_BIN env var.
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

/// Run planar-agent with the suite's PLANAR_DB injected. Returns RunResult (caller frees).
fn runAgent(suite: *const harness.Suite, args: []const []const u8) harness.Suite.RunResult {
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
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

/// Assert exit 0, return stdout (caller owns).
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

/// Seed an init + plan + task, return (plan_id_str, task_id) caller frees plan_id_str.
const SeedResult = struct {
    plan_arg: []u8,
    task_id: i64,
};

fn seedPlanWithTask(suite: *const harness.Suite, slug: []const u8) SeedResult {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");

    const task_json = suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "--json", "context test task" });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");

    return .{ .plan_arg = plan_arg, .task_id = task_id };
}

/// Start a workflow run. Returns (run_db_id, run_identifier_str) — caller frees run_identifier_str.
fn startRun(suite: *const harness.Suite, plan_arg: []const u8, suffix: []const u8) struct { run_db_id: i64, run_id_str: []u8 } {
    const gpa = suite.allocator;
    const self_pid_str = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid_str);
    const run_identifier = std.fmt.allocPrint(gpa, "ctx-{s}-{d}", .{ suffix, std.c.getpid() }) catch @panic("OOM");
    defer gpa.free(run_identifier);

    const out = mustRunAgent(suite, &.{
        "run",         "start",
        "--plan",      plan_arg,
        "--workflow",  "test-wf",
        "--run-id",    run_identifier,
        "--pid",       self_pid_str,
        "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(out);

    const run_db_id = extractIntField(out, "\"run_id\":") orelse @panic("no run_id in run start");
    return .{
        .run_db_id = run_db_id,
        .run_id_str = std.fmt.allocPrint(gpa, "{d}", .{run_db_id}) catch @panic("OOM"),
    };
}

/// Acquire a claim on the given task via `pull`, stamped with run_id and stage.
/// Returns claim_token (caller frees).
fn pullWithRun(suite: *const harness.Suite, plan_arg: []const u8, run_id_str: []const u8, stage: []const u8) []u8 {
    const gpa = suite.allocator;
    const out = mustRunAgent(suite, &.{
        "pull",
        plan_arg,
        "--no-locality-probe",
        "--run",
        run_id_str,
        "--stage",
        stage,
        "--json",
    });
    defer gpa.free(out);

    // Extract claim_token from the pull JSON.
    const token_key = "\"claim_token\":\"";
    const idx = std.mem.indexOf(u8, out, token_key) orelse @panic("no claim_token in pull output");
    const start = idx + token_key.len;
    const end = std.mem.indexOfScalarPos(u8, out, start, '"') orelse @panic("no closing quote for claim_token");
    return gpa.dupe(u8, out[start..end]) catch @panic("OOM");
}

fn extractIntField(s: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, s, key) orelse return null;
    var i = idx + key.len;
    while (i < s.len and (s[i] == ' ' or s[i] == ':' or s[i] == '\t')) i += 1;
    var end = i;
    while (end < s.len and s[end] >= '0' and s[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, s[i..end], 10) catch null;
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

// =========================================================================
// Scenario A — add → list asserts stamped row
// =========================================================================

test "scenario A: context add stamps run_id/stage/claim_id; context list returns the row" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedPlanWithTask(&suite, "ctx-add-list");
    defer gpa.free(seed.plan_arg);

    const run_info = startRun(&suite, seed.plan_arg, "add-list");
    defer gpa.free(run_info.run_id_str);

    // Acquire a claim with run_id and stage.
    const token = pullWithRun(&suite, seed.plan_arg, run_info.run_id_str, "plan");
    defer gpa.free(token);

    // context add.
    const add_out = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", token,
        "--kind",  "finding",
        "--body",  "test finding from scenario A",
        "--json",
    });
    defer gpa.free(add_out);

    try std.testing.expect(contains(add_out, "\"ok\":true"));
    try std.testing.expect(contains(add_out, "\"id\":"));
    const record_id = extractIntField(add_out, "\"id\":") orelse @panic("no id in add output");
    try std.testing.expect(record_id > 0);

    // context list.
    const list_out = mustRunAgent(&suite, &.{
        "context", "list",
        "--run",   run_info.run_id_str,
        "--json",
    });
    defer gpa.free(list_out);

    try std.testing.expect(contains(list_out, "\"ok\":true"));
    try std.testing.expect(contains(list_out, "\"records\":["));
    // The record id appears in the list.
    const id_fragment = std.fmt.allocPrint(gpa, "\"id\":{d}", .{record_id}) catch @panic("OOM");
    defer gpa.free(id_fragment);
    try std.testing.expect(contains(list_out, id_fragment));
    // Status should be active.
    try std.testing.expect(contains(list_out, "\"status\":\"active\""));
    // Kind should be finding.
    try std.testing.expect(contains(list_out, "\"kind\":\"finding\""));
    // Stage should be plan (from the claim).
    try std.testing.expect(contains(list_out, "\"stage\":\"plan\""));
}

// =========================================================================
// Scenario B — capsule with compiled-from asserts provenance
// =========================================================================

test "scenario B: context add capsule with --compiled-from records provenance" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedPlanWithTask(&suite, "ctx-capsule");
    defer gpa.free(seed.plan_arg);

    const run_info = startRun(&suite, seed.plan_arg, "capsule");
    defer gpa.free(run_info.run_id_str);

    const token = pullWithRun(&suite, seed.plan_arg, run_info.run_id_str, "plan");
    defer gpa.free(token);

    // Add two raw records.
    const add1 = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", token,
        "--kind",  "finding",
        "--body",  "raw finding 1",
        "--json",
    });
    defer gpa.free(add1);
    const id1 = extractIntField(add1, "\"id\":") orelse @panic("no id in add1");

    const add2 = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", token,
        "--kind",  "risk",
        "--body",  "raw risk 2",
        "--json",
    });
    defer gpa.free(add2);
    const id2 = extractIntField(add2, "\"id\":") orelse @panic("no id in add2");

    // Add a capsule with --compiled-from pointing to both raw record ids.
    const compiled_from = std.fmt.allocPrint(gpa, "{d},{d}", .{ id1, id2 }) catch @panic("OOM");
    defer gpa.free(compiled_from);

    const capsule_out = mustRunAgent(&suite, &.{
        "context",         "add",
        "--claim",         token,
        "--kind",          "capsule",
        "--body",          "compiled capsule",
        "--compiled-from", compiled_from,
        "--json",
    });
    defer gpa.free(capsule_out);

    try std.testing.expect(contains(capsule_out, "\"ok\":true"));
    const capsule_id = extractIntField(capsule_out, "\"id\":") orelse @panic("no id in capsule");

    // List and find the capsule with compiled_from.
    const list_out = mustRunAgent(&suite, &.{
        "context", "list",
        "--run",   run_info.run_id_str,
        "--kind",  "capsule",
        "--json",
    });
    defer gpa.free(list_out);

    try std.testing.expect(contains(list_out, "\"kind\":\"capsule\""));
    // compiled_from should be the provenance string (not null).
    try std.testing.expect(contains(list_out, compiled_from));
    _ = capsule_id;
}

// =========================================================================
// Scenario C — resolve single record → consumed; list by status asserts
// =========================================================================

test "scenario C: context resolve --id transitions active to consumed" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedPlanWithTask(&suite, "ctx-resolve-single");
    defer gpa.free(seed.plan_arg);

    const run_info = startRun(&suite, seed.plan_arg, "resolve-single");
    defer gpa.free(run_info.run_id_str);

    const token = pullWithRun(&suite, seed.plan_arg, run_info.run_id_str, "code");
    defer gpa.free(token);

    const add_out = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", token,
        "--kind",  "artifact",
        "--body",  "an artifact to resolve",
        "--json",
    });
    defer gpa.free(add_out);
    const rec_id = extractIntField(add_out, "\"id\":") orelse @panic("no id");
    const rec_id_str = std.fmt.allocPrint(gpa, "{d}", .{rec_id}) catch @panic("OOM");
    defer gpa.free(rec_id_str);

    // Before resolve: status is active.
    const before_list = mustRunAgent(&suite, &.{
        "context",  "list",
        "--run",    run_info.run_id_str,
        "--status", "active",
        "--json",
    });
    defer gpa.free(before_list);
    try std.testing.expect(contains(before_list, "\"status\":\"active\""));

    // Resolve to consumed.
    const resolve_out = mustRunAgent(&suite, &.{
        "context",  "resolve",
        "--id",     rec_id_str,
        "--status", "consumed",
        "--json",
    });
    defer gpa.free(resolve_out);
    try std.testing.expect(contains(resolve_out, "\"ok\":true"));
    try std.testing.expect(contains(resolve_out, "\"updated\":1"));
    try std.testing.expect(contains(resolve_out, "\"status\":\"consumed\""));

    // After resolve: the record appears in consumed list.
    const after_list = mustRunAgent(&suite, &.{
        "context",  "list",
        "--run",    run_info.run_id_str,
        "--status", "consumed",
        "--json",
    });
    defer gpa.free(after_list);
    try std.testing.expect(contains(after_list, "\"status\":\"consumed\""));

    // And the active list is now empty for this run.
    const active_list = mustRunAgent(&suite, &.{
        "context",  "list",
        "--run",    run_info.run_id_str,
        "--status", "active",
        "--json",
    });
    defer gpa.free(active_list);
    try std.testing.expect(contains(active_list, "\"records\":[]"));
}

// =========================================================================
// Scenario D — resolve bulk stage sweep
// =========================================================================

test "scenario D: context resolve --run --stage bulk-marks plan-stage active records superseded" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Need two tasks so we can pull twice with different stages.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ctx-bulk", "--json", "ctx-bulk" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);

    // Add two tasks.
    _ = gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "task 1" }));
    _ = gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "task 2" }));

    const run_info = startRun(&suite, plan_arg, "bulk");
    defer gpa.free(run_info.run_id_str);

    // Pull two claims: first in stage=plan, second in stage=code.
    const token_plan = pullWithRun(&suite, plan_arg, run_info.run_id_str, "plan");
    defer gpa.free(token_plan);
    const token_code = pullWithRun(&suite, plan_arg, run_info.run_id_str, "code");
    defer gpa.free(token_code);

    // Add two records in stage=plan and one in stage=code.
    const add_p1 = mustRunAgent(&suite, &.{
        "context", "add", "--claim", token_plan, "--kind", "finding", "--body", "plan finding 1", "--json",
    });
    defer gpa.free(add_p1);
    const add_p2 = mustRunAgent(&suite, &.{
        "context", "add", "--claim", token_plan, "--kind", "risk", "--body", "plan risk 2", "--json",
    });
    defer gpa.free(add_p2);
    const add_c1 = mustRunAgent(&suite, &.{
        "context", "add", "--claim", token_code, "--kind", "artifact", "--body", "code artifact", "--json",
    });
    defer gpa.free(add_c1);

    // Bulk resolve the plan stage.
    const resolve_out = mustRunAgent(&suite, &.{
        "context",  "resolve",
        "--run",    run_info.run_id_str,
        "--stage",  "plan",
        "--status", "superseded",
        "--json",
    });
    defer gpa.free(resolve_out);
    try std.testing.expect(contains(resolve_out, "\"ok\":true"));
    try std.testing.expect(contains(resolve_out, "\"updated\":2"));

    // plan stage active records: should be empty.
    const plan_active = mustRunAgent(&suite, &.{
        "context",  "list",
        "--run",    run_info.run_id_str,
        "--stage",  "plan",
        "--status", "active",
        "--json",
    });
    defer gpa.free(plan_active);
    try std.testing.expect(contains(plan_active, "\"records\":[]"));

    // code stage active record: should still be active.
    const code_active = mustRunAgent(&suite, &.{
        "context",  "list",
        "--run",    run_info.run_id_str,
        "--stage",  "code",
        "--status", "active",
        "--json",
    });
    defer gpa.free(code_active);
    try std.testing.expect(contains(code_active, "\"status\":\"active\""));
    try std.testing.expect(contains(code_active, "\"kind\":\"artifact\""));
}

// =========================================================================
// Scenario E — context add on non-run claim errors clearly
// =========================================================================

test "scenario E: context add on a claim without run_id fails with clear error" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedPlanWithTask(&suite, "ctx-no-run");
    defer gpa.free(seed.plan_arg);

    // Acquire a claim WITHOUT --run (no run_id).
    const pull_out = mustRunAgent(&suite, &.{
        "pull", seed.plan_arg, "--no-locality-probe", "--json",
    });
    defer gpa.free(pull_out);

    try std.testing.expect(contains(pull_out, "\"no_work\":false"));
    const token_key = "\"claim_token\":\"";
    const idx = std.mem.indexOf(u8, pull_out, token_key) orelse @panic("no claim_token");
    const start = idx + token_key.len;
    const end = std.mem.indexOfScalarPos(u8, pull_out, start, '"') orelse @panic("no closing quote");
    const token = try gpa.dupe(u8, pull_out[start..end]);
    defer gpa.free(token);

    // context add with a non-run claim must fail.
    const res = runAgent(&suite, &.{
        "context", "add",
        "--claim", token,
        "--kind",  "finding",
        "--body",  "should fail",
        "--json",
    });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    // The error message must mention run_id.
    const has_run_id_mention = contains(res.stderr, "run_id") or contains(res.stdout, "run_id");
    try std.testing.expect(has_run_id_mention);
}

// =========================================================================
// Scenario F — Q603 order: resolve raw records first, then write capsule
//
// This is the canonical stage-close compaction flow (plan 585 task 3905):
//   1. Add raw records via context add.
//   2. Bulk-resolve the stage to 'consumed' (Q603 ORDER: FIRST).
//   3. Write the capsule via `context capsule` (Q603 ORDER: AFTER).
//   4. Assert:
//      a. All raw records are consumed.
//      b. The capsule row is active (not swept — it was inserted AFTER resolve).
//      c. The capsule's compiled_from carries the raw record ids.
//      d. The capsule's claim_id is null (decision 456: run-keyed).
// =========================================================================

test "scenario F: context capsule Q603 order — resolve first, capsule after (task 3905)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedPlanWithTask(&suite, "ctx-q603");
    defer gpa.free(seed.plan_arg);

    const run_info = startRun(&suite, seed.plan_arg, "q603");
    defer gpa.free(run_info.run_id_str);

    // Acquire a run-associated claim for the 'code' stage.
    const token = pullWithRun(&suite, seed.plan_arg, run_info.run_id_str, "code");
    defer gpa.free(token);

    // Add three raw records — finding, risk, followup.
    const add_f = mustRunAgent(&suite, &.{
        "context", "add",            "--claim", token, "--kind", "finding",
        "--body",  "a code finding", "--json",
    });
    defer gpa.free(add_f);
    const id_f = extractIntField(add_f, "\"id\":") orelse @panic("no id in finding add");

    const add_r = mustRunAgent(&suite, &.{
        "context", "add",         "--claim", token, "--kind", "risk",
        "--body",  "a code risk", "--json",
    });
    defer gpa.free(add_r);
    const id_r = extractIntField(add_r, "\"id\":") orelse @panic("no id in risk add");

    const add_fo = mustRunAgent(&suite, &.{
        "context", "add",             "--claim", token, "--kind", "followup",
        "--body",  "a code followup", "--json",
    });
    defer gpa.free(add_fo);
    const id_fo = extractIntField(add_fo, "\"id\":") orelse @panic("no id in followup add");

    // Pre-assert: all three are active.
    const pre_list = mustRunAgent(&suite, &.{
        "context", "list", "--run", run_info.run_id_str, "--stage", "code", "--status", "active", "--json",
    });
    defer gpa.free(pre_list);
    try std.testing.expect(contains(pre_list, "\"status\":\"active\""));

    // Q603 STEP 1: bulk-resolve active records in stage=code to consumed (FIRST).
    const resolve_out = mustRunAgent(&suite, &.{
        "context",  "resolve",  "--run",  run_info.run_id_str, "--stage", "code",
        "--status", "consumed", "--json",
    });
    defer gpa.free(resolve_out);
    try std.testing.expect(contains(resolve_out, "\"ok\":true"));
    try std.testing.expect(contains(resolve_out, "\"updated\":3"));

    // Verify: all three raw records are now consumed.
    const consumed_list = mustRunAgent(&suite, &.{
        "context",  "list",     "--run",  run_info.run_id_str, "--stage", "code",
        "--status", "consumed", "--json",
    });
    defer gpa.free(consumed_list);
    try std.testing.expect(contains(consumed_list, "\"status\":\"consumed\""));
    // No active records remain for this stage.
    const active_after = mustRunAgent(&suite, &.{
        "context",  "list",   "--run",  run_info.run_id_str, "--stage", "code",
        "--status", "active", "--json",
    });
    defer gpa.free(active_after);
    try std.testing.expect(contains(active_after, "\"records\":[]"));

    // Q603 STEP 4: write the capsule AFTER (not swept because it's inserted now).
    const compiled_from = std.fmt.allocPrint(gpa, "{d},{d},{d}", .{ id_f, id_r, id_fo }) catch @panic("OOM");
    defer gpa.free(compiled_from);
    const cap_out = mustRunAgent(&suite, &.{
        "context",         "capsule",
        "--run",           run_info.run_id_str,
        "--stage",         "code",
        "--body",          "[finding] a code finding\n[risk] a code risk\n[followup] a code followup",
        "--compiled-from", compiled_from,
        "--json",
    });
    defer gpa.free(cap_out);

    // Assert (b): capsule is active.
    try std.testing.expect(contains(cap_out, "\"ok\":true"));
    try std.testing.expect(contains(cap_out, "\"kind\":\"capsule\""));
    try std.testing.expect(contains(cap_out, "\"status\":\"active\""));
    // Assert (d): claim_id is null (decision 456: run-keyed).
    try std.testing.expect(contains(cap_out, "\"claim_id\":null"));

    const cap_id = extractIntField(cap_out, "\"id\":") orelse @panic("no id in capsule output");
    try std.testing.expect(cap_id > 0);

    // List all records for the run; assert capsule is active and appears.
    const final_list = mustRunAgent(&suite, &.{
        "context", "list", "--run", run_info.run_id_str, "--kind", "capsule", "--json",
    });
    defer gpa.free(final_list);
    try std.testing.expect(contains(final_list, "\"kind\":\"capsule\""));
    try std.testing.expect(contains(final_list, "\"status\":\"active\""));
    // Assert (c): compiled_from carries the raw record ids.
    try std.testing.expect(contains(final_list, compiled_from));
}
