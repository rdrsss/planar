//! Integration tests for `planar-execute` (plan 492 M1 task 3165).
//!
//! Black-box exercises against the compiled `planar-execute` binary:
//!
//! - `planar-execute <workflow.lua>` exits 0 for a well-formed workflow
//!   whose run() does pure Lua (no host fns — those are M2).
//! - Trailing [args…] are threaded into ctx.args[1], ctx.args[2], ...
//!   as a 1-based Lua sequence; the workflow can assert on them.
//! - `planar-execute version` prints a version line and exits 0.
//! - `planar-execute /no/such/file.lua` exits non-zero with a message on stderr.
//! - A workflow whose run() calls error() exits non-zero with the message.
//! - A syntactically invalid workflow exits non-zero (compile error path).
//! - A workflow that returns a non-table exits with code 2 (invalid module shape).

const std = @import("std");

fn resolveExecuteBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_EXECUTE_BIN=")) return s["PLANAR_EXECUTE_BIN=".len..];
    }
    @panic("PLANAR_EXECUTE_BIN is not set. Run via: zig build test-integration");
}

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

fn runExecute(gpa: std.mem.Allocator, args: []const []const u8) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
    });
    return .{
        .term = result.term,
        .stdout = result.stdout,
        .stderr = result.stderr,
        .gpa = gpa,
    };
}

fn tmpAbsPath(tmp: *std.testing.TmpDir, gpa: std.mem.Allocator) ![]u8 {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const len = try tmp.dir.realPath(std.testing.io, &buf);
    return gpa.dupe(u8, buf[0..len]);
}

fn writeWorkflow(tmp: *std.testing.TmpDir, name: []const u8, content: []const u8) !void {
    var f = try tmp.dir.createFile(std.testing.io, name, .{});
    defer f.close(std.testing.io);
    try f.writeStreamingAll(std.testing.io, content);
}

fn workflowPath(tmp_abs: []const u8, name: []const u8, gpa: std.mem.Allocator) ![]u8 {
    return std.fs.path.join(gpa, &.{ tmp_abs, name });
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "planar-execute version prints a version line and exits 0" {
    const gpa = std.testing.allocator;

    const res = try runExecute(gpa, &.{"version"});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // Output must contain "planar-execute" and a Lua version marker.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "planar-execute") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "lua") != null or
        std.mem.indexOf(u8, res.stdout, "Lua") != null);
}

test "planar-execute: trivial workflow exits 0" {
    // A well-formed workflow whose run() does only pure Lua (no host fns)
    // must succeed. This is the M1 end-to-end smoke test.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return {
        \\  meta = { name = "trivial", description = "M1 smoke", phases = {} },
        \\  run = function(ctx)
        \\    -- pure Lua, no host fns; must succeed.
        \\    local x = 1 + 1
        \\    _ = x
        \\  end,
        \\}
    ;
    try writeWorkflow(&tmp, "trivial.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "trivial.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute: trailing args reach ctx.args as 1-based sequence" {
    // Verify the CLI arg-threading contract: [args…] after the workflow path
    // are threaded into run(ctx) as ctx.args[1], ctx.args[2], ...
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return {
        \\  meta = { name = "args-check", description = "threading", phases = {} },
        \\  run = function(ctx)
        \\    assert(ctx.args[1] == "alpha", "expected ctx.args[1]='alpha'")
        \\    assert(ctx.args[2] == "beta",  "expected ctx.args[2]='beta'")
        \\    assert(ctx.args[3] == nil,     "expected ctx.args[3]=nil")
        \\  end,
        \\}
    ;
    try writeWorkflow(&tmp, "args_check.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "args_check.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ wf_path, "alpha", "beta" });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute: missing file exits non-zero with message" {
    // A file path that does not exist must produce a non-zero exit and a
    // human-readable message on stderr.
    const gpa = std.testing.allocator;

    const res = try runExecute(gpa, &.{"/no/such/planar-execute-test-file.lua"});
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    // stderr must mention the file path or a meaningful error keyword.
    try std.testing.expect(
        std.mem.indexOf(u8, res.stderr, "cannot read") != null or
            std.mem.indexOf(u8, res.stderr, "FileNotFound") != null or
            std.mem.indexOf(u8, res.stderr, "no such") != null,
    );
}

test "planar-execute: runtime error in run() exits non-zero with message" {
    // A workflow whose run() calls error() must exit non-zero and put
    // the Lua error message on stderr.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return {
        \\  meta = { name = "boom", description = "errors", phases = {} },
        \\  run = function(ctx)
        \\    error("intentional test error")
        \\  end,
        \\}
    ;
    try writeWorkflow(&tmp, "boom.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "boom.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "intentional test error") != null);
}

test "planar-execute: compile error exits 3 with message" {
    // A syntactically invalid workflow must exit with code 3 (compile error).
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "bad_syntax.lua", "this is not valid lua @@@@\n");
    const wf_path = try workflowPath(tmp_abs, "bad_syntax.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 3), res.exitCode());
    try std.testing.expect(res.stderr.len > 0);
}

test "planar-execute: non-table return exits 2 (invalid module shape)" {
    // A workflow that returns a number instead of a table must exit with
    // code 2 (invalid module structure).
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "not_table.lua", "return 42\n");
    const wf_path = try workflowPath(tmp_abs, "not_table.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 2), res.exitCode());
}

test "planar-execute: help flag exits 0" {
    // `planar-execute --help` must exit 0 and produce help text.
    const gpa = std.testing.allocator;

    const res = try runExecute(gpa, &.{"--help"});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(
        std.mem.indexOf(u8, res.stdout, "planar-execute") != null,
    );
}
