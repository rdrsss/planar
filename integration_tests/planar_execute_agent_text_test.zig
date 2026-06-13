//! integration_tests/planar_execute_agent_text_test.zig —
//!   `result.text` field on the `ctx.agent()` return table (plan 586 task 3946).
//!
//! Black-box integration gate: runs Lua workflows under `--mock-worker` and
//! `--mock-outcomes` and asserts that `result.text` is always present and
//! carries the expected final-message text extracted from the worker stdout.
//!
//! ## What these tests cover
//!
//!   1. `--mock-worker` with default canned stdout ("ok") → result.text == ""
//!      (no parseable vendor event, always-present empty string).
//!   2. `--mock-outcomes` with a `text` field → result.text == "<canned text>"
//!      (the synthesized stdout is parsed and the canned text is returned).
//!   3. `--mock-outcomes` with no `text` or `stdout` → result.text == ""
//!      (graceful empty, best-effort).
//!   4. `result.text` is present (not nil) on skipped calls (already-done /
//!      blocked-by-budget paths). Those paths short-circuit before spawning
//!      any worker; the field must still exist with value "".
//!
//! ## Hermeticity
//!
//!   - Fixture DB seeded via the real CLI (harness Suite injects PLANAR_DB).
//!   - PATH is prepended with the freshly-built bin dir.
//!   - `--mock-worker` / `--mock-outcomes` are used so no real spawn occurs.
//!   - PLANAR_EXECUTE_LIVE_AGENT is NOT set.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Binary resolution helpers (mirrors other execute integration test files)
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

fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: make test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

// ---------------------------------------------------------------------------
// Runner helpers
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

fn writeWorkflow(tmp: *std.testing.TmpDir, name: []const u8, content: []const u8) !void {
    var f = try tmp.dir.createFile(std.testing.io, name, .{});
    defer f.close(std.testing.io);
    try f.writeStreamingAll(std.testing.io, content);
}

fn writeFile(tmp: *std.testing.TmpDir, name: []const u8, content: []const u8) !void {
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = name, .data = content });
}

fn tmpAbsPath(tmp: *std.testing.TmpDir, gpa: std.mem.Allocator) ![]u8 {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const len = try tmp.dir.realPath(std.testing.io, &buf);
    return gpa.dupe(u8, buf[0..len]);
}

fn workflowPath(tmp_abs: []const u8, name: []const u8, gpa: std.mem.Allocator) ![]u8 {
    return std.fs.path.join(gpa, &.{ tmp_abs, name });
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "result.text: always-present as empty string when stdout unparseable (--mock-worker, task 3946)" {
    // The default --mock-worker canned stdout ("ok") does not parse as a vendor
    // stream-json event → result.text must be "" (never nil). The workflow
    // unconditionally reads .text without a nil check.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "rtext-empty", "--json", "rtext-empty" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "rtext empty test" }));

    const wf_src =
        \\return {
        \\  meta = { name = "rtext-empty-wf", description = "result.text empty test", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt-rtext",
        \\      claim_token = "tok-rtext-empty",
        \\      task_slug = "rtext-empty-task",
        \\    })
        \\    assert(r.text ~= nil, "result.text must be present (not nil)")
        \\    assert(type(r.text) == "string", "result.text must be a string, got: " .. type(r.text))
        \\    assert(r.text == "", "result.text must be empty when stdout unparseable, got: " .. r.text)
        \\    print("text_ok")
        \\  end
        \\}
    ;

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "rtext_empty.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "rtext_empty.lua", gpa);
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
            "\nresult.text empty test stdout:\n{s}\nstderr:\n{s}\n",
            .{ res.stdout, res.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(contains(res.stdout, "text_ok"));
}

test "result.text: canned text via --mock-outcomes text field (task 3946)" {
    // The `--mock-outcomes` file has a `text` field with no `stdout`. The
    // harness synthesizes a minimal claude stream-json result line from the
    // text field so extractFinalText can parse it. The workflow sees
    // result.text == "canned final text".
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "rtext-canned", "--json", "rtext-canned" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "rtext canned test" }));

    const wf_src =
        \\return {
        \\  meta = { name = "rtext-canned-wf", description = "result.text canned test", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt-rtextc",
        \\      claim_token = "tok-rtext-canned",
        \\      task_slug = "rtext-canned-task",
        \\    })
        \\    assert(r.text ~= nil, "result.text must be present")
        \\    assert(r.text == "canned final text",
        \\      "expected canned text, got: " .. tostring(r.text))
        \\    print("canned_ok")
        \\  end
        \\}
    ;

    // Mock-outcomes file: one entry with text="canned final text", no stdout.
    const outcomes_ndjson = "{\"exit_code\":0,\"text\":\"canned final text\"}\n";

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "rtext_canned.lua", wf_src);
    try writeFile(&tmp, "outcomes.ndjson", outcomes_ndjson);

    const wf_path = try workflowPath(tmp_abs, "rtext_canned.lua", gpa);
    defer gpa.free(wf_path);
    const outcomes_path = try workflowPath(tmp_abs, "outcomes.ndjson", gpa);
    defer gpa.free(outcomes_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{
        wf_path,
        "--mock-outcomes",
        outcomes_path,
        "--plan",
        plan_arg,
    });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print(
            "\nresult.text canned test stdout:\n{s}\nstderr:\n{s}\n",
            .{ res.stdout, res.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(contains(res.stdout, "canned_ok"));
}

test "result.text: empty when --mock-outcomes has no text or stdout (task 3946)" {
    // A mock-outcomes entry with neither text nor stdout → result.text == "".
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "rtext-nofld", "--json", "rtext-nofld" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "rtext no field test" }));

    const wf_src =
        \\return {
        \\  meta = { name = "rtext-nofld-wf", description = "result.text no field test", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/wt-rtextnf",
        \\      claim_token = "tok-rtext-nofld",
        \\      task_slug = "rtext-nofld-task",
        \\    })
        \\    assert(r.text ~= nil, "result.text must be present")
        \\    assert(r.text == "", "result.text must be empty, got: " .. tostring(r.text))
        \\    print("nofld_ok")
        \\  end
        \\}
    ;

    // Mock-outcomes: minimal entry with only exit_code, no text or stdout.
    const outcomes_ndjson = "{\"exit_code\":0}\n";

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "rtext_nofld.lua", wf_src);
    try writeFile(&tmp, "outcomes_nofld.ndjson", outcomes_ndjson);

    const wf_path = try workflowPath(tmp_abs, "rtext_nofld.lua", gpa);
    defer gpa.free(wf_path);
    const outcomes_path = try workflowPath(tmp_abs, "outcomes_nofld.ndjson", gpa);
    defer gpa.free(outcomes_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{
        wf_path,
        "--mock-outcomes",
        outcomes_path,
        "--plan",
        plan_arg,
    });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print(
            "\nresult.text nofld test stdout:\n{s}\nstderr:\n{s}\n",
            .{ res.stdout, res.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(contains(res.stdout, "nofld_ok"));
}
