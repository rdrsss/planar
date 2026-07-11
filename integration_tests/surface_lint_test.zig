//! Black-box coverage for the standalone authored-surface validator.

const std = @import("std");

pub fn main(init: std.process.Init) !void {
    const allocator = init.arena.allocator();
    const args = try init.minimal.args.toSlice(allocator);
    try expect(args.len == 3);
    try testDirtyCorpus(allocator, init.io, args[1], args[2]);
    try testCleanCorpus(allocator, init.io, args[1], args[2]);
    try std.Io.File.stdout().writeStreamingAll(init.io, "All 2 surface-lint black-box tests passed.\n");
}

fn testDirtyCorpus(allocator: std.mem.Allocator, io: std.Io, bin: []const u8, fixtures: []const u8) !void {
    const dirty = try std.fs.path.join(allocator, &.{ fixtures, "dirty" });
    const text = try run(allocator, io, bin, &.{ dirty, "--require-feedback-contract" });
    try expectExit(text.term, 1);
    try expectEqual("", text.stderr);
    const expected_lines = [_][]const u8{
        "agents/drift.md:6: surface-link-missing:",
        "agents/drift.md:7: surface-legacy-reference:",
        "agents/drift.md:8: surface-artifact-set-drift:",
        "agents/drift.md:9: surface-command-drift:",
        "agents/drift.md:10: surface-capability-drift:",
        "skills/src/missing-contract.md:1: surface-contract-missing:",
        "surface-lint: 6 finding(s) across 2 files\n",
    };
    var last: usize = 0;
    for (expected_lines) |needle| {
        const at = std.mem.indexOfPos(u8, text.stdout, last, needle) orelse return error.TestExpectedEqual;
        last = at + needle.len;
    }

    const json_run = try run(allocator, io, bin, &.{ dirty, "--require-feedback-contract", "--json" });
    try expectExit(json_run.term, 1);
    const Envelope = struct {
        version: u8,
        ok: bool,
        files_scanned: usize,
        findings: []const struct { code: []const u8, file: []const u8, line: usize, message: []const u8 },
    };
    const parsed = try std.json.parseFromSlice(Envelope, allocator, json_run.stdout, .{ .ignore_unknown_fields = false });
    try expect(parsed.value.version == 1);
    try expect(!parsed.value.ok);
    try expect(parsed.value.files_scanned == 2);
    try expect(parsed.value.findings.len == 6);
    const codes = [_][]const u8{
        "surface-link-missing",  "surface-legacy-reference", "surface-artifact-set-drift",
        "surface-command-drift", "surface-capability-drift", "surface-contract-missing",
    };
    for (codes, parsed.value.findings) |code, finding| try expectEqual(code, finding.code);
}

fn testCleanCorpus(allocator: std.mem.Allocator, io: std.Io, bin: []const u8, fixtures: []const u8) !void {
    const clean = try std.fs.path.join(allocator, &.{ fixtures, "clean" });
    const result = try run(allocator, io, bin, &.{ clean, "--require-feedback-contract" });
    try expectExit(result.term, 0);
    try expectEqual("surface-lint: clean (1 files)\n", result.stdout);
    try expectEqual("", result.stderr);
}

fn run(allocator: std.mem.Allocator, io: std.Io, bin: []const u8, extra: []const []const u8) !std.process.RunResult {
    var argv: std.ArrayList([]const u8) = .empty;
    try argv.append(allocator, bin);
    try argv.appendSlice(allocator, extra);
    return std.process.run(allocator, io, .{ .argv = argv.items });
}

fn expectExit(term: std.process.Child.Term, code: u8) !void {
    try expect(term == .exited);
    try expect(term.exited == code);
}
fn expect(ok: bool) !void {
    if (!ok) return error.TestExpectedEqual;
}
fn expectEqual(expected: []const u8, actual: []const u8) !void {
    if (!std.mem.eql(u8, expected, actual)) return error.TestExpectedEqual;
}
