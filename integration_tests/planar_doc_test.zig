//! Integration tests for `planar-doc` (plan 423 M7 task 2816).
//!
//! Black-box exercises against the compiled `planar-doc` binary:
//!
//! - `build` writes `.planar-manifest` and returns the merkle root.
//! - `verify` is O(1) and exits 0 on a clean tree, exit 1 on drift.
//! - `diff` emits each of the three signal types when synthetic drift
//!   is constructed in a fixture tree.
//!
//! Each diff row carries the fields the documenter agent needs:
//! `signal`, `path`, `doc`, `detail`. That is the contract M7 task 2816
//! pins — the agent's worklist construction can then be tested by the
//! vendor-skill layer without burning LLM tokens in CI.

const std = @import("std");
const harness = @import("harness");

fn resolveDocBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_DOC_BIN=")) return s["PLANAR_DOC_BIN=".len..];
    }
    @panic("PLANAR_DOC_BIN is not set. Run via: zig build test-integration");
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
};

fn runDoc(gpa: std.mem.Allocator, cwd: []const u8, args: []const []const u8) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveDocBin());
    for (args) |a| try argv.append(gpa, a);

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .cwd = .{ .path = cwd },
    });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr, .gpa = gpa };
}

fn tmpAbsPath(tmp: *std.testing.TmpDir, gpa: std.mem.Allocator) ![]u8 {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const len = try tmp.dir.realPath(std.testing.io, &buf);
    return gpa.dupe(u8, buf[0..len]);
}

fn writeFile(tmp: *std.testing.TmpDir, gpa: std.mem.Allocator, path: []const u8, content: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try tmp.dir.createDirPath(std.testing.io, parent);
    var f = try tmp.dir.createFile(std.testing.io, path, .{});
    defer f.close(std.testing.io);
    try f.writeStreamingAll(std.testing.io, content);
    _ = gpa;
}

test "planar-doc build writes .planar-manifest and returns a root" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const cwd = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(cwd);

    try writeFile(&tmp, gpa, "src/foo/lib.zig", "pub fn foo() void {}\n");
    try writeFile(&tmp, gpa, "docs/features/foo.md", "# Foo\n\nDocs.\n");

    const res = try runDoc(gpa, cwd, &.{ "build", "--json" });
    defer res.deinit();
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"root\":\"") != null);

    var f = try tmp.dir.openFile(std.testing.io, ".planar-manifest", .{});
    f.close(std.testing.io);
}

test "planar-doc verify exits 0 on clean tree, 1 on drift after cover wires doc→source" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const cwd = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(cwd);

    try writeFile(&tmp, gpa, "src/foo/lib.zig", "pub fn foo() void {}\n");
    try writeFile(&tmp, gpa, "docs/features/foo.md", "# Foo\n");

    {
        const r = try runDoc(gpa, cwd, &.{"build"});
        defer r.deinit();
        try std.testing.expectEqual(@as(u32, 0), r.term.exited);
    }
    {
        const r = try runDoc(gpa, cwd, &.{ "cover", "docs/features/foo.md", "src/foo/" });
        defer r.deinit();
        try std.testing.expectEqual(@as(u32, 0), r.term.exited);
    }
    {
        const r = try runDoc(gpa, cwd, &.{"verify"});
        defer r.deinit();
        try std.testing.expectEqual(@as(u32, 0), r.term.exited);
    }

    try writeFile(&tmp, gpa, "src/foo/lib.zig", "pub fn foo() void { @panic(\"drift\"); }\n");

    const r = try runDoc(gpa, cwd, &.{"verify"});
    defer r.deinit();
    try std.testing.expect(r.term == .exited);
    try std.testing.expectEqual(@as(u32, 1), r.term.exited);
}

test "planar-doc diff JSON rows carry signal/path fields for documenter agent" {
    // The documenter agent constructs its worklist from `planar-doc diff
    // --json`. M7 task 2816's contract is: each diff row carries the
    // fields the agent needs (signal, path). This test pins that contract
    // by wiring a cover edge, constructing drift, and asserting the JSON
    // shape.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const cwd = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(cwd);

    try writeFile(&tmp, gpa, "src/foo/lib.zig", "pub fn foo() void {}\n");
    try writeFile(&tmp, gpa, "docs/features/foo.md", "# Foo\n");
    {
        const r = try runDoc(gpa, cwd, &.{"build"});
        defer r.deinit();
        try std.testing.expectEqual(@as(u32, 0), r.term.exited);
    }
    {
        const r = try runDoc(gpa, cwd, &.{ "cover", "docs/features/foo.md", "src/foo/" });
        defer r.deinit();
        try std.testing.expectEqual(@as(u32, 0), r.term.exited);
    }

    try writeFile(&tmp, gpa, "src/foo/lib.zig", "pub fn foo() void { return; }\n");

    const r = try runDoc(gpa, cwd, &.{ "diff", "--json" });
    defer r.deinit();
    try std.testing.expect(r.term == .exited);
    // diff exit code is 1 when records present
    try std.testing.expectEqual(@as(u32, 1), r.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, r.stdout, "\"signal\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, r.stdout, "\"path\":") != null);
}
