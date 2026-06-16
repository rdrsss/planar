//! integration_tests/propagate_tree_workflow_test.zig
//!
//! Black-box integration tests for `workflows/propagate_tree.lua` (plan 638, M3).
//!
//! These tests exercise the workflow end-to-end via the compiled
//! `planar-execute` binary, a real isolated `PLANAR_DB`, an in-process HTTP
//! server acting as the Jira remote, and the harness `planar` binary on PATH.
//!
//! ## What these tests prove
//!
//! ### Test 1 (task 4117): walk structure + continue-past-error + per-entity trace
//!
//! Seed an anchor plan + one task. Configure the fake Jira server to return
//! 500 for the FIRST request (the anchor plan entity) and 201 for the rest.
//! Run the workflow and assert:
//!   - Exit 0 (walk completes — no abort on single-entity failure).
//!   - flow.result: ok=false, run_status="error", at least one error entity.
//!   - Run journal: walk-start, one entity-propagated event per entity
//!     (including one with op="error" and one with op="created"), walk-done.
//!
//! ### Test 2 (task 4118): resumability end-to-end proof
//!
//! Same fixture. Run 1: server fails on the task entity (index 1), succeeds
//! on the anchor (index 0). Assert:
//!   - 1 created (anchor), 1 error (task), run_status="error".
//!   - After run 1: `ext propagate-one --dry-run` on the anchor returns
//!     "skipped" (already has an external_links row).
//!   - After run 1: `ext propagate-one --dry-run` on the task returns
//!     "planned" (no link yet).
//!
//! Run 2: new healthy server, same system slug. Assert:
//!   - ok=true, run_status="completed", no errors.
//!   - After run 2: both entities return "skipped" (no duplicate links).
//!   - Run 2 journal: no entity-propagated events with op="error".
//!
//! The combination of these two checks (run 1 leaves anchor linked + task
//! unlinked; run 2 links only task and skips anchor) is the resumability
//! proof. `ext propagate-one`'s idempotency guard (external_links row check)
//! is the mechanism; the workflow's pcall loop is what ensures the walk
//! continues past the failure.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// JSON shapes
// =========================================================================

const PlanJSON = struct {
    id: i64 = 0,
    status: []const u8 = "",
};

const TaskJSON = struct {
    id: i64 = 0,
};

const ShowRun = struct {
    run_uid: []const u8 = "",
    status: []const u8 = "",
    events: []const ShowEvent = &.{},
};

const ShowEvent = struct {
    seq: i64 = 0,
    kind: []const u8 = "",
    payload: ?std.json.Value = null,
};

/// flow.result from the workflow's stdout.
const FlowResult = struct {
    ok: bool = false,
    created: i64 = 0,
    skipped: i64 = 0,
    errors: []const []const u8 = &.{},
    run_uid: []const u8 = "",
    run_status: []const u8 = "",
};

// =========================================================================
// Env resolution
// =========================================================================

fn resolveEnv(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key ++ "=")) return s[(key ++ "=").len..];
    }
    @panic(key ++ " is not set. Run via: make test-integration");
}

// =========================================================================
// RunResult and runExecute — spawn planar-execute with a custom env
// =========================================================================

const RunResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,
    fn deinit(self: RunResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }
};

/// EnvKV is a key-value pair for injecting extra env vars.
const EnvKV = struct { key: []const u8, value: []const u8 };

/// runExecute spawns `planar-execute <args...>` with the isolated DB and PATH,
/// plus any additional env vars in `extra_env`.
fn runExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    extra_env: []const EnvKV,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveEnv("PLANAR_EXECUTE_BIN"));
    for (args) |a| try argv.append(gpa, a);

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", db_path);
    try env_map.put("PLANAR_CONFIG_PATH", "/nonexistent-planar-config.toml");
    try env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1");

    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    for (extra_env) |kv| {
        try env_map.put(kv.key, kv.value);
    }

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .cwd = .{ .path = cwd },
        .environ_map = &env_map,
    });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr, .gpa = gpa };
}

fn repoRootFromBin(allocator: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return allocator.dupe(u8, d3);
}

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n\r\t");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .ignore_unknown_fields = true,
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nparseJSON({s}) failed: {s}\nbuf: {s}\n", .{ @typeName(T), @errorName(e), buf });
        @panic("parseJSON failed");
    };
    return parsed.value;
}

// =========================================================================
// FakeJira — in-process Jira HTTP server
//
// Listens on 127.0.0.1:0 (OS assigns ephemeral port). Runs a background
// thread that accepts one connection at a time and responds to POST requests.
// The `fail_on_request_index` field controls which request (0-based) returns
// 500; null means always succeed.
//
// Each successful POST responds with {"key":"TEST-<N>"} so the client's
// JSON parse succeeds and propagate-one records an external_links row.
// =========================================================================

const FakeJira = struct {
    gpa: std.mem.Allocator,
    io: std.Io,
    /// Which 0-based request index should return 500 (null = always succeed).
    fail_on_request_index: ?usize,
    server: std.Io.net.Server,
    /// Ephemeral port the server is actually listening on.
    port: u16,
    thread: std.Thread,
    /// Atomic stop flag. Set to 1 via `deinit()` to stop the accept loop.
    stop_flag: std.atomic.Value(u32),

    const Self = @This();

    fn init(gpa: std.mem.Allocator, io: std.Io, fail_on_request_index: ?usize) !*Self {
        const self = try gpa.create(Self);
        self.* = .{
            .gpa = gpa,
            .io = io,
            .fail_on_request_index = fail_on_request_index,
            .server = undefined,
            .port = 0,
            .thread = undefined,
            .stop_flag = .init(0),
        };

        // Listen on 127.0.0.1:0; OS assigns an ephemeral port.
        var addr = std.Io.net.IpAddress{ .ip4 = std.Io.net.Ip4Address.loopback(0) };
        self.server = try std.Io.net.IpAddress.listen(&addr, io, .{});
        self.port = self.server.socket.address.getPort();

        self.thread = try std.Thread.spawn(.{}, runLoop, .{self});
        return self;
    }

    fn deinit(self: *Self) void {
        self.stop_flag.store(1, .release);
        // Closing the server socket causes the blocked accept() to return an
        // error, which causes runLoop to exit.
        self.server.deinit(self.io);
        self.thread.join();
        self.gpa.destroy(self);
    }

    fn runLoop(self: *Self) void {
        var req_index: usize = 0;
        while (self.stop_flag.load(.acquire) == 0) {
            const stream = self.server.accept(self.io) catch break;
            self.handleOne(stream, req_index) catch {};
            req_index += 1;
        }
    }

    fn handleOne(self: *Self, stream: std.Io.net.Stream, req_index: usize) !void {
        defer stream.close(self.io);

        var in_buf: [8192]u8 = undefined;
        var out_buf: [4096]u8 = undefined;

        var rdr = stream.reader(self.io, &in_buf);
        var wtr = stream.writer(self.io, &out_buf);

        var hs = std.http.Server.init(&rdr.interface, &wtr.interface);
        var req = hs.receiveHead() catch return;

        // Do NOT manually drain the body here. req.respond() calls
        // respondUnflushed() which calls discardBody() internally, and that
        // function drives the HTTP reader state machine via readerExpectContinue.
        // Pre-draining via rdr.interface.discard bypasses the state machine
        // and leaves r.state in an inconsistent state, causing an assertion
        // failure inside discardBody.

        const should_fail = if (self.fail_on_request_index) |idx| req_index == idx else false;

        if (should_fail) {
            try req.respond("Internal Server Error", .{
                .status = .internal_server_error,
                .extra_headers = &.{.{ .name = "Content-Type", .value = "text/plain" }},
            });
        } else {
            var body_buf: [64]u8 = undefined;
            const body = try std.fmt.bufPrint(&body_buf, "{{\"key\":\"TEST-{d}\"}}", .{req_index + 1});
            try req.respond(body, .{
                .status = .created,
                .extra_headers = &.{.{ .name = "Content-Type", .value = "application/json" }},
            });
        }
    }
};

// =========================================================================
// Fixture helper: seed an anchor plan + one task linked via derives-from.
// =========================================================================

fn seedPlanWithTask(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    plan_slug: []const u8,
) struct { plan_id: []u8, task_id: []u8 } {
    const gpa = suite.allocator;

    const plan_buf = suite.mustRun(&.{
        "plan", "create", "--json", "--slug", plan_slug, plan_slug,
    });
    defer gpa.free(plan_buf);
    const plan = parseJSON(PlanJSON, arena, plan_buf);
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task_buf = suite.mustRun(&.{ "task", "add", "--json", "task-one" });
    defer gpa.free(task_buf);
    const task = parseJSON(TaskJSON, arena, task_buf);
    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    // Link task to plan via derives-from entity link (walkTree uses entity_links).
    const plan_ref = std.fmt.allocPrint(arena, "plan:{s}", .{plan_id}) catch @panic("OOM");
    const link_out = suite.mustRun(&.{
        "task", "link", task_id, plan_ref, "--relationship", "derives-from",
    });
    gpa.free(link_out);

    return .{ .plan_id = plan_id, .task_id = task_id };
}

// =========================================================================
// Test 1 — Task 4117: walk structure + continue-past-error + per-entity trace
// =========================================================================

test "propagate_tree: walk completes past single-entity failure, per-entity trace events emitted" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("pt-walk");
    suite.addAssoc("pt-walk", null);

    const seeded = seedPlanWithTask(&suite, arena, "pt-walk-plan");

    // Fake server: fail on request index 0 (the anchor plan entity).
    const server = try FakeJira.init(gpa, std.testing.io, 0);
    defer server.deinit();

    const base_url = try std.fmt.allocPrint(arena, "http://127.0.0.1:{d}", .{server.port});

    // Register a Jira system with the harness token env var.
    const reg_out = suite.mustRunWith(&.{
        "ext",             "register",   "jira",
        "jira-pt-walk",    "--base-url", base_url,
        "--project",       "TEST",       "--auth-env",
        "PLANAR_PT_TOKEN",
    }, &.{.{ .key = "PLANAR_PT_TOKEN", .value = "test-token-walk" }});
    defer gpa.free(reg_out);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "propagate_tree.lua" });
    defer gpa.free(wf_path);

    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s},\"system\":\"jira-pt-walk\"}}", .{seeded.plan_id});
    defer gpa.free(args_json);

    const res = try runExecute(gpa, root, suite.absDbPath(), &.{
        .{ .key = "PLANAR_PT_TOKEN", .value = "test-token-walk" },
    }, &.{
        "run", wf_path, "--phase", "propagate", "--args", args_json,
    });
    defer res.deinit();

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("workflow stderr:\n{s}\nstdout:\n{s}\n", .{ res.stderr, res.stdout });
    }
    // The workflow exits 0 even when entities fail — the walk completes and
    // calls flow.result(); the engine exits 0 when the phase returns normally.
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    const result = parseJSON(FlowResult, arena, res.stdout);

    // Some entities failed → ok=false, run_status="error".
    try std.testing.expect(!result.ok);
    try std.testing.expectEqualStrings("error", result.run_status);
    try std.testing.expect(result.errors.len > 0);
    // The task entity (index 1) should have succeeded.
    try std.testing.expect(result.created >= 1);
    try std.testing.expect(result.run_uid.len > 0);

    // Verify the run journal via `run show --json`.
    const run_show_buf = suite.mustRunWith(&.{
        "run", "show", result.run_uid, "--json",
    }, &.{.{ .key = "PLANAR_PT_TOKEN", .value = "test-token-walk" }});
    defer gpa.free(run_show_buf);
    const run_show = parseJSON(ShowRun, arena, run_show_buf);

    try std.testing.expectEqualStrings("error", run_show.status);

    // Assert event structure.
    var found_walk_start = false;
    var found_walk_done = false;
    var entity_event_count: usize = 0;
    var found_error_event = false;
    var found_created_event = false;

    for (run_show.events) |ev| {
        if (std.mem.eql(u8, ev.kind, "walk-start")) found_walk_start = true;
        if (std.mem.eql(u8, ev.kind, "walk-done")) found_walk_done = true;
        if (std.mem.eql(u8, ev.kind, "entity-propagated")) {
            entity_event_count += 1;
            if (ev.payload) |payload| {
                if (payload == .object) {
                    if (payload.object.get("op")) |op_val| {
                        if (op_val == .string) {
                            if (std.mem.eql(u8, op_val.string, "error")) found_error_event = true;
                            if (std.mem.eql(u8, op_val.string, "created")) found_created_event = true;
                        }
                    }
                }
            }
        }
    }

    try std.testing.expect(found_walk_start);
    try std.testing.expect(found_walk_done);
    // One event per descendant: anchor plan + task = at least 2.
    try std.testing.expect(entity_event_count >= 2);
    // One error (anchor, request 0 failed) and one created (task, request 1 succeeded).
    try std.testing.expect(found_error_event);
    try std.testing.expect(found_created_event);
}

// =========================================================================
// Test 2 — Task 4118: resumability proof (two-run end-to-end)
// =========================================================================

test "propagate_tree: resumability — second run creates only the missing entity, skips already-linked" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("pt-resume");
    suite.addAssoc("pt-resume", null);

    const seeded = seedPlanWithTask(&suite, arena, "pt-resume-plan");

    // ---- Run 1: fail on the SECOND request (index 1 = task entity) --------
    // anchor plan (request 0): succeeds → external_links row written.
    // task entity (request 1): fails → no external_links row.

    const server1 = try FakeJira.init(gpa, std.testing.io, 1);
    defer server1.deinit();
    const base_url1 = try std.fmt.allocPrint(arena, "http://127.0.0.1:{d}", .{server1.port});

    const reg1_out = suite.mustRunWith(&.{
        "ext",             "register",   "jira",
        "jira-pt-res",     "--base-url", base_url1,
        "--project",       "TEST",       "--auth-env",
        "PLANAR_PT_TOKEN",
    }, &.{.{ .key = "PLANAR_PT_TOKEN", .value = "test-token-res" }});
    defer gpa.free(reg1_out);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "propagate_tree.lua" });
    defer gpa.free(wf_path);

    const args_json1 = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s},\"system\":\"jira-pt-res\"}}", .{seeded.plan_id});
    defer gpa.free(args_json1);

    const res1 = try runExecute(gpa, root, suite.absDbPath(), &.{
        .{ .key = "PLANAR_PT_TOKEN", .value = "test-token-res" },
    }, &.{
        "run", wf_path, "--phase", "propagate", "--args", args_json1,
    });
    defer res1.deinit();

    if (res1.term != .exited or res1.term.exited != 0) {
        std.debug.print("Run1 stderr:\n{s}\nstdout:\n{s}\n", .{ res1.stderr, res1.stdout });
    }
    try std.testing.expect(res1.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res1.term.exited);

    const result1 = parseJSON(FlowResult, arena, res1.stdout);

    // Run 1: 1 created (anchor), 1 error (task).
    try std.testing.expect(!result1.ok);
    try std.testing.expectEqualStrings("error", result1.run_status);
    try std.testing.expectEqual(@as(i64, 1), result1.created);
    try std.testing.expectEqual(@as(i64, 0), result1.skipped);
    try std.testing.expect(result1.errors.len == 1);

    // After run 1: the anchor plan must have an external_links row.
    // Proof: `ext propagate-one --dry-run` returns op="skipped" (not "planned").
    const plan_ref = try std.fmt.allocPrint(arena, "plan:{s}", .{seeded.plan_id});
    const anchor_probe = suite.mustRunWith(&.{
        "ext",    "propagate-one", "jira-pt-res",
        "--from", plan_ref,        "--dry-run",
        "--json",
    }, &.{.{ .key = "PLANAR_PT_TOKEN", .value = "test-token-res" }});
    defer gpa.free(anchor_probe);
    // The anchor link exists → propagate-one reports "skipped".
    if (std.mem.indexOf(u8, anchor_probe, "\"skipped\"") == null) {
        std.debug.print("anchor_probe after run1 (expected skipped):\n{s}\n", .{anchor_probe});
        try std.testing.expect(false);
    }

    // After run 1: the task entity must NOT have an external_links row.
    // Proof: `ext propagate-one --dry-run` returns op="planned" (not "skipped").
    const task_ref = try std.fmt.allocPrint(arena, "task:{s}", .{seeded.task_id});
    const task_probe1 = suite.mustRunWith(&.{
        "ext",    "propagate-one", "jira-pt-res",
        "--from", task_ref,        "--dry-run",
        "--json",
    }, &.{.{ .key = "PLANAR_PT_TOKEN", .value = "test-token-res" }});
    defer gpa.free(task_probe1);
    // No task link yet → propagate-one reports "planned" under dry-run.
    if (std.mem.indexOf(u8, task_probe1, "\"planned\"") == null) {
        std.debug.print("task_probe1 after run1 (expected planned):\n{s}\n", .{task_probe1});
        try std.testing.expect(false);
    }

    // ---- Run 2: healthy server — only the task entity should be created ----
    //
    // We re-use the same system slug ("jira-pt-res"). The second run of the
    // workflow will call `ext propagate-one` for each descendant. The anchor
    // already has an external_links row for system jira-pt-res, so it will
    // return op="skipped". The task has no row yet, so it will be created.
    //
    // To re-use the same system with a different server, we start a fresh
    // server but we cannot re-point an existing registered system's base_url
    // (no `ext update` verb). Instead, we update the jira-pt-res system's
    // registration by re-registering under a slightly different slug and
    // re-running.
    //
    // Actually the simplest correct approach: bring up server2 on a new port,
    // register "jira-pt-res2" pointing there, and run with that slug.
    // The resumability proof still holds: the anchor link was written under
    // "jira-pt-res" (system_id=X). "jira-pt-res2" is a different system
    // (system_id=Y), so from jira-pt-res2's perspective, neither entity is
    // linked yet. Both will be created fresh (2 created, 0 skipped).
    //
    // This means a pure single-system resumability proof requires re-running
    // with the same system slug against a server that now responds to the
    // previously-failed entity. We can achieve this by using the same server
    // but changing its fail policy — but that requires the server to be
    // stateful.
    //
    // Solution: use the SAME server (server1) but call the workflow again
    // now that server1's request counter is at 2. For a second run, request
    // index 2 is the anchor (it skips HTTP, returns "skipped") and request
    // index 3 would be the task... but because the anchor is already linked
    // it will short-circuit before the HTTP call. So only the task HTTP
    // call (now at request index 2) happens, which is NOT in the fail list.
    //
    // server1 fails only on index 1. The second run is:
    //   anchor: idempotent skip (no HTTP) → op="skipped", no request to server.
    //   task: HTTP call to server1 (request index 2, which is NOT index 1) → succeeds.
    //
    // This is the correct single-system resumability scenario.

    const args_json2 = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s},\"system\":\"jira-pt-res\"}}", .{seeded.plan_id});
    defer gpa.free(args_json2);

    const res2 = try runExecute(gpa, root, suite.absDbPath(), &.{
        .{ .key = "PLANAR_PT_TOKEN", .value = "test-token-res" },
    }, &.{
        "run", wf_path, "--phase", "propagate", "--args", args_json2,
    });
    defer res2.deinit();

    if (res2.term != .exited or res2.term.exited != 0) {
        std.debug.print("Run2 stderr:\n{s}\nstdout:\n{s}\n", .{ res2.stderr, res2.stdout });
    }
    try std.testing.expect(res2.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res2.term.exited);

    const result2 = parseJSON(FlowResult, arena, res2.stdout);

    // Run 2 expectations:
    //   - ok=true (no errors), run_status="completed".
    //   - 1 skipped (anchor already linked), 1 created (task).
    try std.testing.expect(result2.ok);
    try std.testing.expectEqualStrings("completed", result2.run_status);
    try std.testing.expectEqual(@as(usize, 0), result2.errors.len);
    try std.testing.expectEqual(@as(i64, 1), result2.skipped);
    try std.testing.expectEqual(@as(i64, 1), result2.created);

    // After run 2: both entities must return "skipped" (no duplicate links created).
    const anchor_probe2 = suite.mustRunWith(&.{
        "ext",    "propagate-one", "jira-pt-res",
        "--from", plan_ref,        "--dry-run",
        "--json",
    }, &.{.{ .key = "PLANAR_PT_TOKEN", .value = "test-token-res" }});
    defer gpa.free(anchor_probe2);
    if (std.mem.indexOf(u8, anchor_probe2, "\"skipped\"") == null) {
        std.debug.print("anchor_probe2 after run2 (expected skipped):\n{s}\n", .{anchor_probe2});
        try std.testing.expect(false);
    }

    const task_probe2 = suite.mustRunWith(&.{
        "ext",    "propagate-one", "jira-pt-res",
        "--from", task_ref,        "--dry-run",
        "--json",
    }, &.{.{ .key = "PLANAR_PT_TOKEN", .value = "test-token-res" }});
    defer gpa.free(task_probe2);
    if (std.mem.indexOf(u8, task_probe2, "\"skipped\"") == null) {
        std.debug.print("task_probe2 after run2 (expected skipped):\n{s}\n", .{task_probe2});
        try std.testing.expect(false);
    }

    // Verify the run 2 journal: status=completed, no entity-propagated with op="error".
    const run2_buf = suite.mustRunWith(&.{
        "run", "show", result2.run_uid, "--json",
    }, &.{.{ .key = "PLANAR_PT_TOKEN", .value = "test-token-res" }});
    defer gpa.free(run2_buf);
    const run2_show = parseJSON(ShowRun, arena, run2_buf);
    try std.testing.expectEqualStrings("completed", run2_show.status);

    for (run2_show.events) |ev| {
        if (std.mem.eql(u8, ev.kind, "entity-propagated")) {
            if (ev.payload) |payload| {
                if (payload == .object) {
                    if (payload.object.get("op")) |op_val| {
                        if (op_val == .string) {
                            // No errors in run 2.
                            try std.testing.expect(!std.mem.eql(u8, op_val.string, "error"));
                        }
                    }
                }
            }
        }
    }
}
