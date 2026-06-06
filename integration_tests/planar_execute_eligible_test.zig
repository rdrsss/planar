//! integration_tests/planar_execute_eligible_test.zig —
//!   `ctx.eligible(plan_id)` host function (plan 492 M5 task 3185).
//!
//! Black-box integration gate: seeds a fixture DB via the real `planar` CLI,
//! seeds disjoint-path touches (parallel-eligible) and overlapping-path
//! touches (serialized), then runs a Lua workflow that calls `ctx.eligible(plan_id)`
//! and emits the eligible ids via `ctx.log`. Asserts:
//!   - eligible ids match the expected disjoint-touch tasks;
//!   - the workflow exits 0 even without PLANAR_EXECUTE_LIVE_AGENT;
//!   - ctx.eligible works ungated (read-only, no spawn gate required).
//!
//! ## Hermeticity
//!
//!   - Fixture DB seeded via the real `planar` CLI (harness Suite injects
//!     PLANAR_DB = temp fixture DB).
//!   - `planar-execute` run with PLANAR_DB = fixture DB and PATH prepended
//!     with the freshly-built bin dir so its bare-name `planar` subprocess
//!     call (from `state.recommendStrategy`) resolves to `./bin/planar` (the
//!     binary that HAS `plan recommend-strategy`), not a stale ~/.planar/bin/planar.
//!   - PLANAR_EXECUTE_LIVE_AGENT is NOT set — ctx.eligible must work without it.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Binary resolution helpers (mirrors doctor test pattern)
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
        @panic("PLANAR_EXECUTE_BIN is not set. Run via: zig build test-integration");
}

/// binDir returns the directory holding the freshly-built binaries, derived
/// from PLANAR_BIN's dirname. ctx.eligible's bare-name `planar` subprocess
/// call resolves via PATH; prepending this dir guarantees the child is the
/// just-built binary (with plan recommend-strategy), not a stale install.
fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: zig build test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

// ---------------------------------------------------------------------------
// Runner: spawn planar-execute with the fixture-DB env + prepended PATH
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
///   PLANAR_DB = suite's fixture DB (so planar-execute's child `planar` reads
///               the seeded fixture, not the operator's real DB)
///   PATH = <bin_dir>:<original PATH>  (so bare "planar" resolves to the
///          freshly-built binary with recommend-strategy)
/// PLANAR_EXECUTE_LIVE_AGENT is intentionally NOT set (read-only ctx.eligible
/// must work without the spawn gate).
fn runExecuteWithFixture(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

    // Build env map from host env.
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    // Pin PLANAR_DB to the fixture DB.
    try env_map.put("PLANAR_DB", suite.absDbPath());

    // Prepend freshly-built bin dir onto PATH so `planar plan recommend-strategy`
    // resolves to ./bin/planar (not a stale ~/.planar/bin/planar). This is the
    // crux: the stale binary does NOT have the recommend-strategy verb.
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    // Explicitly strip PLANAR_EXECUTE_LIVE_AGENT to verify ctx.eligible works
    // without the spawn gate (it is a read-only verb).
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
// Fixture seeding helpers (mirrors plan_recommend_strategy_test pattern)
// ---------------------------------------------------------------------------

const PlanJSON = struct { id: i64 };
const TaskJSON = struct { id: i64 };
const QuestionJSON = struct { id: i64 };

fn addTask(suite: *harness.Suite, arena: std.mem.Allocator, pid: []const u8, title: []const u8) i64 {
    const t = suite.mustRunJSON(TaskJSON, arena, &.{ "task", "add", "--plan", pid, "--json", title });
    return t.id;
}

fn touchPath(suite: *harness.Suite, repo_slug: []const u8, task_id: i64, path: []const u8) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    const out = suite.mustRun(&.{ "task", "touches", "add", tid, repo_slug, "--path", path });
    gpa.free(out);
}

fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("elig-repo");
    const assoc_slug = "elig-org";
    const cr = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" });
    suite.allocator.free(cr);
    const root = suite.tmpAbsPath();
    const ad = suite.mustRun(&.{ "assoc", "add", assoc_slug, root });
    suite.allocator.free(ad);

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", assoc_slug, "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "elig-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found in assoc members", .{});
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "ctx.eligible: workflow sees parallel-eligible and serialized tasks (task 3185)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // ---- 1. Seed the fixture DB.
    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "elig-test", "--json", "ELIG_TEST",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Two genuinely-disjoint tasks → parallel_eligible.
    const t_a = addTask(&suite, arena, pid, "alpha");
    touchPath(&suite, repo, t_a, "src/alpha.zig");
    const t_b = addTask(&suite, arena, pid, "beta");
    touchPath(&suite, repo, t_b, "src/beta.zig");

    // Two tasks that overlap on the same path → both serialized (rule 2).
    const t_o1 = addTask(&suite, arena, pid, "overlap-one");
    touchPath(&suite, repo, t_o1, "src/shared.zig");
    const t_o2 = addTask(&suite, arena, pid, "overlap-two");
    touchPath(&suite, repo, t_o2, "src/shared.zig");

    // ---- 2. Write a workflow that calls ctx.eligible and logs the eligible ids.
    //
    // The workflow:
    //   - Calls ctx.eligible(plan_id) with plan_id from ctx.args[1].
    //   - Logs "eligible_count:<n>" so we can assert from stdout.
    //   - Logs "eligible_id:<id>" for each eligible task.
    //   - Logs "fan_out:<bool>".
    //   - Logs "serialized_count:<n>".
    //
    // ctx.log writes to the host-side HostState.calls but does NOT appear on
    // stdout in the default run path. We need to capture output another way.
    // Solution: the workflow uses `error()` to embed the data in the run error,
    // OR we use the `ctx.phase` to log — but neither is in stdout.
    //
    // Simpler: the workflow returns normally but we inspect HostState.calls from
    // the outside. However, from the black-box integration test we cannot read
    // HostState.calls. Instead, the workflow uses Lua's built-in `print`
    // (which writes to stdout — it IS available in the sandbox) to emit the
    // data we assert on. `print` is left open in the sandbox per openSandboxedLibs.
    const workflow_src =
        \\return {
        \\  meta = { name = "elig-test", description = "eligible test", phases = {} },
        \\  run = function(ctx)
        \\    local plan_id = tonumber(ctx.args[1])
        \\    local r = ctx.eligible(plan_id)
        \\    -- emit results on stdout for the integration test to assert on
        \\    print("eligible_count:" .. #r.eligible)
        \\    print("fan_out:" .. tostring(r.fan_out_available))
        \\    print("serialized_count:" .. #r.serialized)
        \\    for _, t in ipairs(r.eligible) do
        \\      print("eligible_id:" .. t.id)
        \\    end
        \\    for _, t in ipairs(r.serialized) do
        \\      print("serialized_id:" .. t.id)
        \\      for _, ex in ipairs(t.excluded_by) do
        \\        print("excluded_rule:" .. ex.rule)
        \\      end
        \\    end
        \\  end
        \\}
    ;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "elig.lua", workflow_src);
    const wf_path = try workflowPath(tmp_abs, "elig.lua", gpa);
    defer gpa.free(wf_path);

    // ---- 3. Run planar-execute with the plan_id as trailing arg.
    const res = try runExecuteWithFixture(gpa, &suite, &.{ wf_path, pid });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\neligible test stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    // ---- 4. Assert on stdout output.
    const out = res.stdout;

    // eligible_count: 2 (alpha and beta are disjoint)
    try std.testing.expect(std.mem.indexOf(u8, out, "eligible_count:2") != null);
    // fan_out_available: true (>= 2 eligible)
    try std.testing.expect(std.mem.indexOf(u8, out, "fan_out:true") != null);
    // serialized_count: 2 (both overlap tasks serialized by rule 2)
    try std.testing.expect(std.mem.indexOf(u8, out, "serialized_count:2") != null);

    // alpha and beta are eligible.
    const t_a_str = std.fmt.allocPrint(gpa, "eligible_id:{d}", .{t_a}) catch unreachable;
    defer gpa.free(t_a_str);
    const t_b_str = std.fmt.allocPrint(gpa, "eligible_id:{d}", .{t_b}) catch unreachable;
    defer gpa.free(t_b_str);
    try std.testing.expect(std.mem.indexOf(u8, out, t_a_str) != null);
    try std.testing.expect(std.mem.indexOf(u8, out, t_b_str) != null);

    // Overlap tasks are in serialized.
    const t_o1_str = std.fmt.allocPrint(gpa, "serialized_id:{d}", .{t_o1}) catch unreachable;
    defer gpa.free(t_o1_str);
    const t_o2_str = std.fmt.allocPrint(gpa, "serialized_id:{d}", .{t_o2}) catch unreachable;
    defer gpa.free(t_o2_str);
    try std.testing.expect(std.mem.indexOf(u8, out, t_o1_str) != null);
    try std.testing.expect(std.mem.indexOf(u8, out, t_o2_str) != null);

    // Both overlap tasks are excluded by rule 2.
    // We expect excluded_rule:2 to appear at least twice (once per overlap task).
    const rule2_count = std.mem.count(u8, out, "excluded_rule:2");
    try std.testing.expect(rule2_count >= 2);
}

test "ctx.eligible: fan_out_available false when < 2 eligible tasks (task 3185)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Seed: one eligible task, one serialized (overlapping migration touch).
    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "elig-fanout-false", "--json", "ELIG_FANOUT_FALSE",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_ok = addTask(&suite, arena, pid, "ok-task");
    touchPath(&suite, repo, t_ok, "src/ok.zig");
    const t_mig = addTask(&suite, arena, pid, "migration-task");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.sql");

    const workflow_src =
        \\return {
        \\  meta = { name = "fanout-false", description = "fan_out false test", phases = {} },
        \\  run = function(ctx)
        \\    local plan_id = tonumber(ctx.args[1])
        \\    local r = ctx.eligible(plan_id)
        \\    print("eligible_count:" .. #r.eligible)
        \\    print("fan_out:" .. tostring(r.fan_out_available))
        \\  end
        \\}
    ;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "fanout_false.lua", workflow_src);
    const wf_path = try workflowPath(tmp_abs, "fanout_false.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ wf_path, pid });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nfanout_false stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    const out = res.stdout;
    try std.testing.expect(std.mem.indexOf(u8, out, "eligible_count:1") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "fan_out:false") != null);
}
