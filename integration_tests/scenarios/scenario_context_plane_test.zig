//! integration_tests/scenarios/scenario_context_plane_test.zig
//!
//! Plan 585 — End-to-end context plane scenario: accumulate → read → compose.
//!
//! This scenario walks the full context-plane loop using real CLI binaries:
//!
//!   1. Seed a plan + two tasks.
//!   2. `planar-agent run start`      — open a workflow_runs row.
//!   3. `planar-agent pull --run --stage`  — claim a task carrying run/stage.
//!   4. `planar-agent context add --claim` — write a finding and a risk record.
//!      The claim token is the only envelope; run_id/stage are stamped server-side.
//!   5. `planar-agent context list --run`   — assert both records appear.
//!   6. `planar-agent context add --kind capsule --compiled-from`
//!      — write a capsule with provenance pointing at the two raw records.
//!   7. `planar-agent context resolve --run --stage --status consumed`
//!      — bulk-mark the raw records consumed.
//!   8. `planar-watch run show <id> --json`
//!      — assert the run row + all three context_records (including capsule) appear.
//!   9. `planar-execute run --mock-worker --plan <id>`
//!      — run a Lua workflow that calls ctx.context("plan") and ctx.brief({...}).
//!      Assert: exit 0, workflow printed the correct record count, brief section
//!      header "Prior-stage context" is reflected in workflow log output.
//!
//! Decisions anchored:
//!   444 — run row owned by planar-agent; planar-execute DB-handle-free.
//!   445 — context_records is working memory, distinct from session_entries.
//!   446 — cleanup is lifecycle (consumed/superseded), never deletion.
//!   447 — claim is the worker-side correlation key; no new envelope.
//!   450 — agent_work_claims carries run_id/stage; context add stamps from claim.
//!
//! Coverage vs task 3932 (live ctx.brief path):
//!   - Step 9 exercises ctx.brief in --mock-worker mode with a plan that has a
//!     live run row + context records. It asserts exit 0 and that the workflow
//!     reached the ctx.brief call (record count logged). It does NOT assert the
//!     exact rendered brief body from the host function — that would require
//!     capturing the worker's stdin, which in mock mode is not exposed.
//!   - The unit tests in brief.zig and the existing planar_execute_context_test.zig
//!     cover the JSON→Lua→BriefInputs→rendered-brief path exhaustively.
//!   - Task 3932 can be satisfied by the brief.zig unit tests alone, OR by a
//!     dedicated ctx.brief live-render test added here. Recommend closing 3932
//!     pointing at brief.zig unit tests unless an end-to-end rendered-brief
//!     string assertion is specifically required.

const std = @import("std");
const harness = @import("harness");

// ============================================================================
// Binary resolution helpers
// ============================================================================

fn envValue(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key) and s.len > key.len and s[key.len] == '=') {
            return s[key.len + 1 ..];
        }
    }
    return null;
}

fn resolveAgentBin() []const u8 {
    return envValue("PLANAR_AGENT_BIN") orelse
        @panic("PLANAR_AGENT_BIN is not set. Run via: make test-integration");
}

fn resolveWatchBin() []const u8 {
    return envValue("PLANAR_WATCH_BIN") orelse
        @panic("PLANAR_WATCH_BIN is not set. Run via: make test-integration");
}

fn resolveExecuteBin() []const u8 {
    return envValue("PLANAR_EXECUTE_BIN") orelse
        @panic("PLANAR_EXECUTE_BIN is not set. Run via: make test-integration");
}

/// binDir returns the directory holding the freshly-built binaries. Used to
/// prepend PATH so that bare `planar-agent` subprocess calls inside ctx.context
/// and ctx.brief resolve to the just-built binary.
fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: make test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

// ============================================================================
// Run helpers: planar-agent, planar-watch, planar-execute
// ============================================================================

fn runBin(
    suite: *const harness.Suite,
    bin: []const u8,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;

    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    argv.append(gpa, bin) catch @panic("OOM argv");
    for (args) |a| argv.append(gpa, a) catch @panic("OOM argv item");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM env_map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runBin spawn ({s}): {s}", .{ bin, @errorName(e) });

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const gpa = suite.allocator;
    const res = runBin(suite, resolveAgentBin(), args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-agent failed (term={any}):\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("mustRunAgent: non-zero exit");
    }
    return res.stdout;
}

fn mustRunWatch(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const gpa = suite.allocator;
    const res = runBin(suite, resolveWatchBin(), args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-watch failed (term={any}):\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("mustRunWatch: non-zero exit");
    }
    return res.stdout;
}

// ============================================================================
// Execute runner: sets PLANAR_DB + prepended PATH (no LIVE_AGENT)
// ============================================================================

const ExecResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,

    fn deinit(self: ExecResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }

    fn exitCode(self: ExecResult) u32 {
        return switch (self.term) {
            .exited => |code| code,
            else => 255,
        };
    }
};

fn runExecuteWithFixture(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    args: []const []const u8,
) !ExecResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", suite.absDbPath());

    // Prepend bin dir so bare "planar-agent" in ctx.context/ctx.brief resolves
    // to the freshly-built binary, not a stale ~/.planar/bin install.
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    // Explicitly do NOT set PLANAR_EXECUTE_LIVE_AGENT — mock mode only.
    _ = env_map.swapRemove("PLANAR_EXECUTE_LIVE_AGENT");

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .environ_map = &env_map,
    });
    return .{
        .term = result.term,
        .stdout = result.stdout,
        .stderr = result.stderr,
        .gpa = gpa,
    };
}

// ============================================================================
// Field extraction helpers
// ============================================================================

fn extractIntField(s: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, s, key) orelse return null;
    var i = idx + key.len;
    while (i < s.len and (s[i] == ' ' or s[i] == ':' or s[i] == '\t')) i += 1;
    var end = i;
    while (end < s.len and s[end] >= '0' and s[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, s[i..end], 10) catch null;
}

fn extractStringField(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse return error.FieldNotFound;
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    if (end == json.len) return error.UnterminatedString;
    return try gpa.dupe(u8, json[i..end]);
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

// ============================================================================
// Workflow file helpers
// ============================================================================

fn writeWorkflow(tmp: *std.testing.TmpDir, name: []const u8, content: []const u8) !void {
    var f = try tmp.dir.createFile(std.testing.io, name, .{});
    defer f.close(std.testing.io);
    try f.writeStreamingAll(std.testing.io, content);
}

fn tmpAbsPath(tmp: *std.testing.TmpDir, gpa: std.mem.Allocator) ![]u8 {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const len = try tmp.dir.realPath(std.testing.io, &buf);
    return gpa.dupe(u8, buf[0..len]);
}

fn workflowPath(tmp_abs: []const u8, name: []const u8, gpa: std.mem.Allocator) ![]u8 {
    return std.fs.path.join(gpa, &.{ tmp_abs, name });
}

// ============================================================================
// Test: full accumulate → read → compose loop
// ============================================================================

test "scenario: context plane — accumulate → read → compose (plan 585)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // -------------------------------------------------------------------------
    // Step 0: Seed plan + tasks.
    //
    // Two tasks: one for the "plan" stage claim, one for the "code" stage
    // ctx.context / ctx.brief call (the execute test needs at least one task
    // available for --mock-worker to pull).
    // -------------------------------------------------------------------------
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ctx-plane-e2e", "--json", "Context plane e2e" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM plan_arg");
    defer gpa.free(plan_arg);

    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "plan stage task" }));
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "code stage task" }));

    // -------------------------------------------------------------------------
    // Step 1: planar-agent run start — open a workflow_runs row.
    //
    // Verifies decision 444: planar-agent is the writer of workflow_runs rows.
    // -------------------------------------------------------------------------
    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM pid");
    defer gpa.free(self_pid);
    const run_label = std.fmt.allocPrint(gpa, "ctx-e2e-{d}", .{std.c.getpid()}) catch @panic("OOM run_label");
    defer gpa.free(run_label);

    const start_out = mustRunAgent(&suite, &.{
        "run",         "start",
        "--plan",      plan_arg,
        "--workflow",  "ctx-plane-e2e-wf",
        "--run-id",    run_label,
        "--pid",       self_pid,
        "--repo-root", "/tmp/ctx-e2e",
        "--json",
    });
    defer gpa.free(start_out);

    // Post-state assertion: run_id present and > 0.
    const run_db_id = extractIntField(start_out, "\"run_id\":") orelse @panic("no run_id in run start output");
    try std.testing.expect(run_db_id > 0);
    try std.testing.expect(contains(start_out, "\"ok\":true"));
    try std.testing.expect(contains(start_out, "\"status\":\"running\""));

    const run_id_str = std.fmt.allocPrint(gpa, "{d}", .{run_db_id}) catch @panic("OOM run_id_str");
    defer gpa.free(run_id_str);

    // -------------------------------------------------------------------------
    // Step 2: planar-agent pull --run <id> --stage plan — claim a task with
    // run/stage stamped on the claim row.
    //
    // Verifies decision 450: claims carry run_id/stage for server-side stamping.
    // -------------------------------------------------------------------------
    const pull_out = mustRunAgent(&suite, &.{
        "pull",                plan_arg,
        "--no-locality-probe", "--run",
        run_id_str,            "--stage",
        "plan",                "--json",
    });
    defer gpa.free(pull_out);

    try std.testing.expect(contains(pull_out, "\"ok\":true"));
    const claim_token = try extractStringField(gpa, pull_out, "\"claim_token\":\"");
    defer gpa.free(claim_token);
    try std.testing.expect(claim_token.len > 0);

    // -------------------------------------------------------------------------
    // Step 3a: context add finding — worker uses only --claim.
    //
    // Verifies decision 447: claim token is the only envelope; run_id/stage/
    // session_id are stamped server-side.
    // -------------------------------------------------------------------------
    const add_finding = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", claim_token,
        "--kind",  "finding",
        "--body",  "Migration 00023 adds run_id/stage to agent_work_claims; apply before testing.",
        "--json",
    });
    defer gpa.free(add_finding);
    try std.testing.expect(contains(add_finding, "\"ok\":true"));
    const finding_id = extractIntField(add_finding, "\"id\":") orelse @panic("no id in context add finding output");
    try std.testing.expect(finding_id > 0);

    // -------------------------------------------------------------------------
    // Step 3b: context add risk.
    // -------------------------------------------------------------------------
    const add_risk = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", claim_token,
        "--kind",  "risk",
        "--body",  "Schema change requires running make test-integration twice for stability.",
        "--json",
    });
    defer gpa.free(add_risk);
    try std.testing.expect(contains(add_risk, "\"ok\":true"));
    const risk_id = extractIntField(add_risk, "\"id\":") orelse @panic("no id in context add risk output");
    try std.testing.expect(risk_id > 0);

    // -------------------------------------------------------------------------
    // Step 4: context list --run — assert both raw records appear as active.
    //
    // Verifies the accumulate side of the loop: records are queryable by run.
    // -------------------------------------------------------------------------
    const list_out = mustRunAgent(&suite, &.{
        "context", "list",
        "--run",   run_id_str,
        "--json",
    });
    defer gpa.free(list_out);

    try std.testing.expect(contains(list_out, "\"ok\":true"));
    try std.testing.expect(contains(list_out, "\"kind\":\"finding\""));
    try std.testing.expect(contains(list_out, "\"kind\":\"risk\""));
    try std.testing.expect(contains(list_out, "\"status\":\"active\""));
    try std.testing.expect(contains(list_out, "Migration 00023 adds run_id"));
    try std.testing.expect(contains(list_out, "Schema change requires running"));
    // Both records must carry stage=plan (stamped server-side from the claim).
    // There must be two stage=plan occurrences.
    {
        const first = std.mem.indexOf(u8, list_out, "\"stage\":\"plan\"") orelse @panic("stage=plan not found");
        const second = std.mem.indexOfPos(u8, list_out, first + 1, "\"stage\":\"plan\"");
        try std.testing.expect(second != null);
    }

    // -------------------------------------------------------------------------
    // Step 5: context add capsule with --compiled-from provenance.
    //
    // Verifies decision 446: compiled capsule carries provenance pointing back
    // to the raw records it distilled.
    // -------------------------------------------------------------------------
    const compiled_from_arg = std.fmt.allocPrint(gpa, "{d},{d}", .{ finding_id, risk_id }) catch @panic("OOM compiled_from");
    defer gpa.free(compiled_from_arg);

    const add_capsule = mustRunAgent(&suite, &.{
        "context",         "add",
        "--claim",         claim_token,
        "--kind",          "capsule",
        "--body",          "Plan stage summary: schema pre-req + double-run gate required.",
        "--compiled-from", compiled_from_arg,
        "--json",
    });
    defer gpa.free(add_capsule);
    try std.testing.expect(contains(add_capsule, "\"ok\":true"));

    // -------------------------------------------------------------------------
    // Step 6: context resolve --run --stage plan --status consumed.
    //
    // Verifies decision 446: cleanup is lifecycle (consumed), not deletion.
    // The capsule record is NOT marked consumed — only the raw records are.
    // -------------------------------------------------------------------------
    const resolve_out = mustRunAgent(&suite, &.{
        "context",  "resolve",
        "--run",    run_id_str,
        "--stage",  "plan",
        "--status", "consumed",
        "--json",
    });
    defer gpa.free(resolve_out);
    try std.testing.expect(contains(resolve_out, "\"ok\":true"));

    // After resolve: re-list the run; both finding and risk should be consumed,
    // capsule should still be active.
    const list_after = mustRunAgent(&suite, &.{
        "context", "list",
        "--run",   run_id_str,
        "--json",
    });
    defer gpa.free(list_after);

    // Decision 446: records are retained (not deleted) — all three still appear.
    try std.testing.expect(contains(list_after, "\"kind\":\"finding\""));
    try std.testing.expect(contains(list_after, "\"kind\":\"risk\""));
    try std.testing.expect(contains(list_after, "\"kind\":\"capsule\""));
    // The capsule is still active; the two raw records are consumed.
    try std.testing.expect(contains(list_after, "\"status\":\"consumed\""));
    try std.testing.expect(contains(list_after, "\"status\":\"active\""));

    // -------------------------------------------------------------------------
    // Step 7: planar-watch run show <id> --json
    //
    // Asserts the run row + all context_records are visible via the read-only
    // observability surface (decision 444 — planar-watch is the read-side view).
    // -------------------------------------------------------------------------
    const watch_out = mustRunWatch(&suite, &.{ "run", "show", run_id_str, "--json" });
    defer gpa.free(watch_out);

    // Run row fields.
    try std.testing.expect(contains(watch_out, "\"run\":"));
    try std.testing.expect(contains(watch_out, "\"status\":\"running\""));
    try std.testing.expect(contains(watch_out, "ctx-plane-e2e-wf"));

    // Context records block.
    try std.testing.expect(contains(watch_out, "\"context_records\":["));
    try std.testing.expect(contains(watch_out, "\"kind\":\"finding\""));
    try std.testing.expect(contains(watch_out, "\"kind\":\"risk\""));
    try std.testing.expect(contains(watch_out, "\"kind\":\"capsule\""));
    // Bodies are present.
    try std.testing.expect(contains(watch_out, "Migration 00023 adds run_id"));
    try std.testing.expect(contains(watch_out, "Schema change requires running"));
    try std.testing.expect(contains(watch_out, "Plan stage summary:"));
    // compiled_from is non-null on the capsule record.
    try std.testing.expect(contains(watch_out, "\"compiled_from\":"));

    // -------------------------------------------------------------------------
    // Step 8: planar-execute --mock-worker exercises ctx.context + ctx.brief.
    //
    // The Lua workflow:
    //   - Calls ctx.context("plan") and prints the count.
    //   - Calls ctx.brief({...}) to compile a brief string and prints its length.
    //   - Asserts that the returned brief contains "Prior-stage context" (the
    //     section header injected by compileBrief whenever a run is active).
    //
    // Under --mock-worker, ctx.context shells the real planar-agent binary and
    // queries the fixture DB. ctx.brief also shells planar-agent context list
    // and planar-agent schema. Both paths exercise the no-DB-handle design
    // (decision 444) against the actual binary surface.
    //
    // NOTE: task 3932 scope — this test asserts:
    //   (a) exit 0
    //   (b) ctx.context returns the correct record count (>= 1, since we wrote
    //       records including the active capsule)
    //   (c) ctx.brief was invoked without crashing (brief_len > 0)
    //   (d) the compiled brief contains "Prior-stage context" (the section header)
    //
    // It does NOT assert the exact body of the capsule in the rendered brief —
    // that level of fidelity is covered by the brief.zig unit tests (specifically
    // "compileBrief: context_capsule renders verbatim when present"). Task 3932
    // can be closed pointing at this test + the brief.zig unit tests.
    // -------------------------------------------------------------------------
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const workflow_src =
        \\return {
        \\  meta = {
        \\    name        = "ctx-plane-e2e",
        \\    description = "Context plane end-to-end scenario workflow.",
        \\    phases      = { "ContextRead" },
        \\  },
        \\  run = function(ctx)
        \\    ctx.phase("ContextRead")
        \\    -- Read ALL records for the current run (no stage filter).
        \\    local all_records = ctx.context()
        \\    ctx.log("all_count:" .. #all_records)
        \\    -- Read only plan-stage records.
        \\    local plan_records = ctx.context("plan")
        \\    ctx.log("plan_count:" .. #plan_records)
        \\    -- Compile a brief (auto-injects context records).
        \\    local b = ctx.brief({
        \\      problem_statement = "Scenario ctx.brief test.",
        \\      claim_token       = "test-token",
        \\      gates             = { "make build" },
        \\    })
        \\    ctx.log("brief_len:" .. tostring(#b))
        \\    -- Verify the "Prior-stage context" section was injected.
        \\    if string.find(b, "Prior%-stage context") then
        \\      ctx.log("has_context_section:yes")
        \\    else
        \\      ctx.log("has_context_section:NO")
        \\    end
        \\  end,
        \\}
    ;

    try writeWorkflow(&tmp, "ctx_e2e.lua", workflow_src);
    const wf_path = try workflowPath(tmp_abs, "ctx_e2e.lua", gpa);
    defer gpa.free(wf_path);

    const exec_res = try runExecuteWithFixture(gpa, &suite, &.{
        wf_path,
        "--mock-worker",
        "--plan",
        plan_arg,
    });
    defer exec_res.deinit();

    if (exec_res.exitCode() != 0) {
        std.debug.print(
            "\nplanar-execute failed in ctx-plane scenario:\nstdout:\n{s}\nstderr:\n{s}\n",
            .{ exec_res.stdout, exec_res.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), exec_res.exitCode());

    const exec_out = exec_res.stdout;

    // ctx.context() returned >= 1 record (the capsule is active; the two raw
    // records are consumed but still returned because list returns ALL statuses
    // by default).
    if (!contains(exec_out, "all_count:")) {
        std.debug.print(
            "ctx-plane scenario: all_count marker missing from stdout:\n{s}\n",
            .{exec_out},
        );
    }
    try std.testing.expect(contains(exec_out, "all_count:"));
    // Must be at least "all_count:1" (the capsule is active; finding + risk
    // are consumed but still returned by context list, which lists all statuses
    // by default). We check >=1 by asserting the value is not "all_count:0".
    try std.testing.expect(!contains(exec_out, "all_count:0"));

    // ctx.context("plan") also found records.
    try std.testing.expect(contains(exec_out, "plan_count:"));
    try std.testing.expect(!contains(exec_out, "plan_count:0"));

    // ctx.brief returned a non-empty brief.
    try std.testing.expect(contains(exec_out, "brief_len:"));
    try std.testing.expect(!contains(exec_out, "brief_len:0"));

    // The compiled brief must contain the "Prior-stage context" section header.
    // This confirms that ctx.brief auto-fetched the run's context records and
    // injected them into the brief (the section always renders when active_run
    // is non-null, per brief.zig's "non-empty case renders section header" contract).
    try std.testing.expect(contains(exec_out, "has_context_section:yes"));

    // -------------------------------------------------------------------------
    // Step 9: planar-agent run end — close the run row.
    //
    // Asserts that run end produces ok output and the run list reflects the
    // terminal status (planar-watch run list --status completed).
    // -------------------------------------------------------------------------
    const end_out = mustRunAgent(&suite, &.{
        "run",      "end",
        "--run-id", run_label,
        "--status", "completed",
        "--json",
    });
    defer gpa.free(end_out);
    try std.testing.expect(contains(end_out, "\"ok\":true"));

    // Post-close: planar-watch run list --status completed should include this run.
    const list_completed = mustRunWatch(&suite, &.{
        "run", "list", "--status", "completed", "--json",
    });
    defer gpa.free(list_completed);
    try std.testing.expect(contains(list_completed, "ctx-plane-e2e-wf"));
    try std.testing.expect(contains(list_completed, "\"status\":\"completed\""));
}
