//! integration_tests/planar_execute_context_test.zig —
//!   `ctx.context([stage])` host function (plan 585 task 3903).
//!
//! Black-box integration gate: runs Lua workflows under `--mock-worker` and
//! asserts the `ctx.context` host function behaves correctly as an observable
//! CLI surface.
//!
//! ## What these tests cover
//!
//!   1. `ctx.context()` (no args) returns an empty Lua table for a fresh run
//!      (no records in the harness's own run row). Workflow exits 0.
//!   2. `ctx.context("plan")` (stage filter) returns an empty table with no
//!      records; still exits 0 (no crash on empty result).
//!   3. `ctx.context()` when called without `--plan` (active_run is null)
//!      degrades gracefully to an empty table; exits 0.
//!   4. Passing a non-string argument raises a Lua error (exit 1). The error
//!      message mentions "ctx.context".
//!   5. The capability boundary: `planar-execute` stays DB-handle-free.
//!      `ctx.context` shells `planar-agent context list` and parses its JSON;
//!      it never opens SQLite directly (verified by capability_boundary_test;
//!      here we verify the end-to-end behaviour via the black-box run).
//!
//! ## Why no "reads seeded records" test
//!
//! `ctx.context` queries by the workflow_runs row opened by the harness's
//! OWN `planar-agent run start` call. Pre-seeding records in that row
//! requires knowing the harness's run_db_id before the run starts, which
//! is not possible without a separate mechanism. The JSON→Lua mapping for
//! non-empty results is pinned by the unit tests in state.zig (task 3903).
//! The end-to-end add→list path is already covered by
//! planar_agent_context_test.zig. The integration tests here focus on the
//! host-function surface and its degradation contract.
//!
//! ## Hermeticity
//!
//!   - Fixture DB seeded via the real CLI (harness Suite injects PLANAR_DB).
//!   - PATH is prepended with the freshly-built bin dir so bare `planar-agent`
//!     subprocess calls from `hostContext` resolve to `./bin/planar-agent`.
//!   - `--mock-worker` is used so no real `claude -p` spawn is attempted.
//!   - PLANAR_EXECUTE_LIVE_AGENT is NOT set — ctx.context must work without it.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Binary resolution helpers (mirrors eligible_test pattern)
// ---------------------------------------------------------------------------

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

fn resolveExecuteBin() []const u8 {
    return envValue("PLANAR_EXECUTE_BIN") orelse
        @panic("PLANAR_EXECUTE_BIN is not set. Run via: make test-integration");
}

/// binDir returns the directory holding the freshly-built binaries, derived
/// from PLANAR_BIN's dirname. ctx.context's bare-name `planar-agent` subprocess
/// call resolves via PATH; prepending this dir guarantees the child is the
/// just-built binary.
fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: make test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

// ---------------------------------------------------------------------------
// Runner: spawn planar-execute with fixture-DB env + prepended PATH
// ---------------------------------------------------------------------------

const RunResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,

    fn deinit(self: RunResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }

    fn exitCode(self: RunResult) u32 {
        return switch (self.term) {
            .exited => |code| code,
            else => 255,
        };
    }
};

/// runExecuteWithFixture spawns `planar-execute <args...>` with:
///   PLANAR_DB = suite's fixture DB
///   PATH = <bin_dir>:<original PATH>  (so bare "planar-agent" resolves to
///          the freshly-built binary)
/// PLANAR_EXECUTE_LIVE_AGENT is intentionally NOT set.
fn runExecuteWithFixture(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    args: []const []const u8,
) !RunResult {
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

    // Prepend freshly-built bin dir onto PATH so `planar-agent context list`
    // resolves to ./bin/planar-agent (not a stale ~/.planar/bin/planar-agent).
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

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

// ---------------------------------------------------------------------------
// Fixture seeding helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Workflow file helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Test 1: ctx.context() with no stage filter returns an empty table for a
// fresh run (no records in the harness's own run row). Workflow exits 0.
test "ctx.context(): empty result for fresh run — exits 0 (task 3903)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed a plan + task so --plan is valid.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ctx-exec-fresh", "--json", "ctx-exec-fresh" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "ctx.context test task" }));

    // Workflow: calls ctx.context() and prints the record count.
    // A fresh run (no prior workers, no records) must return count=0 and exit 0.
    const workflow_src =
        \\return {
        \\  meta = { name = "ctx-context-fresh", description = "ctx.context() fresh run test", phases = {} },
        \\  run = function(ctx)
        \\    local records = ctx.context()
        \\    print("count:" .. #records)
        \\    -- type must be a table (sequence)
        \\    print("type:" .. type(records))
        \\  end
        \\}
    ;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "ctx_fresh.lua", workflow_src);
    const wf_path = try workflowPath(tmp_abs, "ctx_fresh.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{
        wf_path,
        "--mock-worker",
        "--plan",
        plan_arg,
    });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print(
            "\nctx.context() fresh test stdout:\n{s}\nstderr:\n{s}\n",
            .{ res.stdout, res.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    // The return value must be a Lua table (sequence) with 0 elements.
    try std.testing.expect(contains(res.stdout, "count:0"));
    try std.testing.expect(contains(res.stdout, "type:table"));
}

// Test 2: ctx.context("plan") with a stage filter also returns an empty table
// for a fresh run. Verifies the stage-filter code path does not crash.
test "ctx.context(stage): stage-filtered empty result — exits 0 (task 3903)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ctx-exec-stg", "--json", "ctx-exec-stg" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "ctx.context stage test task" }));

    const workflow_src =
        \\return {
        \\  meta = { name = "ctx-context-stg", description = "ctx.context(stage) empty test", phases = {} },
        \\  run = function(ctx)
        \\    local plan_recs = ctx.context("plan")
        \\    local code_recs = ctx.context("code")
        \\    print("plan_count:" .. #plan_recs)
        \\    print("code_count:" .. #code_recs)
        \\    -- nil stage should also work
        \\    local all_recs = ctx.context()
        \\    print("all_count:" .. #all_recs)
        \\  end
        \\}
    ;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "ctx_stg.lua", workflow_src);
    const wf_path = try workflowPath(tmp_abs, "ctx_stg.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{
        wf_path,
        "--mock-worker",
        "--plan",
        plan_arg,
    });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print(
            "\nctx.context(stage) empty test stdout:\n{s}\nstderr:\n{s}\n",
            .{ res.stdout, res.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    try std.testing.expect(contains(res.stdout, "plan_count:0"));
    try std.testing.expect(contains(res.stdout, "code_count:0"));
    try std.testing.expect(contains(res.stdout, "all_count:0"));
}

// Test 3: ctx.context() degrades to an empty table when no --plan is given
// (active_run is null). Workflow must exit 0.
test "ctx.context(): null active_run degrades to empty table — exits 0 (task 3903)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // No plan init needed — we deliberately omit --plan.
    const workflow_src =
        \\return {
        \\  meta = { name = "ctx-context-norun", description = "ctx.context() no-run degradation test", phases = {} },
        \\  run = function(ctx)
        \\    -- active_run is null (no --plan), so ctx.context() must return {}
        \\    local records = ctx.context()
        \\    print("norun_count:" .. #records)
        \\    print("norun_type:" .. type(records))
        \\  end
        \\}
    ;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "ctx_norun.lua", workflow_src);
    const wf_path = try workflowPath(tmp_abs, "ctx_norun.lua", gpa);
    defer gpa.free(wf_path);

    // NOTE: no --plan flag → plan_id==0 → runStart not called → active_run is null.
    const res = try runExecuteWithFixture(gpa, &suite, &.{
        wf_path,
        "--mock-worker",
        // no --plan
    });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print(
            "\nctx.context() no-run test stdout:\n{s}\nstderr:\n{s}\n",
            .{ res.stdout, res.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    try std.testing.expect(contains(res.stdout, "norun_count:0"));
    try std.testing.expect(contains(res.stdout, "norun_type:table"));
}

// Test 4: wrong argument type raises a Lua error cleanly (exit 1).
// The error message must mention "ctx.context".
test "ctx.context(): non-string argument raises Lua error — exits 1 (task 3903)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "ctx-exec-typeerr", "--json", "ctx-exec-typeerr" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "type error task" }));

    // Workflow passes a number (wrong type) to ctx.context().
    const workflow_src =
        \\return {
        \\  meta = { name = "ctx-context-typeerr", description = "type error test", phases = {} },
        \\  run = function(ctx)
        \\    ctx.context(42)
        \\  end
        \\}
    ;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "ctx_typeerr.lua", workflow_src);
    const wf_path = try workflowPath(tmp_abs, "ctx_typeerr.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{
        wf_path,
        "--mock-worker",
        "--plan",
        plan_arg,
    });
    defer res.deinit();

    // A type-error in a host function propagates as a Lua runtime error → exit 1.
    try std.testing.expect(res.exitCode() != 0);
    // The error message must mention ctx.context (from the luaL_error call).
    const has_mention = contains(res.stderr, "ctx.context") or contains(res.stdout, "ctx.context");
    try std.testing.expect(has_mention);
}
