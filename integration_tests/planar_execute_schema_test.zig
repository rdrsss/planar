//! integration_tests/planar_execute_schema_test.zig
//!
//! Pins the `planar-execute schema` verb contract (plan 503 task 3513).
//!
//! `planar-execute schema` must:
//!   - exit 0
//!   - emit valid JSON parseable by the cli-usage linter
//!   - contain the expected top-level shape (schemaVersion, layout, root, commands)
//!   - root must be "planar-execute"
//!   - the commands array must contain all four verbs: run, version, doctor, schema
//!   - the run verb's flags must include the key flags:
//!       --plan, --dry-run, --mock-worker, --mock-outcomes, --bypass-reviewer-guard
//!
//! The verb is read-only introspection: no DB, no claim, no mutation.
//! Run via: make test-integration

const std = @import("std");

fn resolveExecuteBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_EXECUTE_BIN=")) return s["PLANAR_EXECUTE_BIN=".len..];
    }
    @panic("PLANAR_EXECUTE_BIN is not set. Run via: make test-integration");
}

/// The top-level shape `planar-execute schema` emits. Mirrors the shape the
/// other four binaries emit (same linter, same JSON dialect).
const Catalog = struct {
    schemaVersion: i64,
    layout: []const u8,
    root: []const u8,
    commands: []struct {
        name: []const u8,
        command: []const u8,
        flags: []struct {
            long: []const u8,
        } = &.{},
    },
};

fn runExecute(gpa: std.mem.Allocator, args: []const []const u8) !struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
} {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);
    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
    });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr };
}

test "planar-execute schema: exits 0 and emits valid JSON with correct root" {
    const gpa = std.testing.allocator;
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const res = try runExecute(gpa, &.{"schema"});
    defer {
        gpa.free(res.stdout);
        gpa.free(res.stderr);
    }

    // Must exit 0.
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // Must emit parseable JSON with the expected top-level shape.
    const parsed = try std.json.parseFromSlice(Catalog, arena, res.stdout, .{
        .ignore_unknown_fields = true,
    });
    const cat = parsed.value;

    try std.testing.expect(cat.schemaVersion >= 1);
    try std.testing.expectEqualStrings("flat", cat.layout);
    try std.testing.expectEqualStrings("planar-execute", cat.root);
    try std.testing.expect(cat.commands.len > 0);
}

test "planar-execute schema: commands array contains run, version, doctor, schema" {
    const gpa = std.testing.allocator;
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const res = try runExecute(gpa, &.{"schema"});
    defer {
        gpa.free(res.stdout);
        gpa.free(res.stderr);
    }

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    const parsed = try std.json.parseFromSlice(Catalog, arena, res.stdout, .{
        .ignore_unknown_fields = true,
    });
    const cat = parsed.value;

    // Collect verb names for presence checks.
    var saw_run = false;
    var saw_version = false;
    var saw_doctor = false;
    var saw_schema = false;
    for (cat.commands) |cmd| {
        if (std.mem.eql(u8, cmd.name, "run")) saw_run = true;
        if (std.mem.eql(u8, cmd.name, "version")) saw_version = true;
        if (std.mem.eql(u8, cmd.name, "doctor")) saw_doctor = true;
        if (std.mem.eql(u8, cmd.name, "schema")) saw_schema = true;
    }
    try std.testing.expect(saw_run);
    try std.testing.expect(saw_version);
    try std.testing.expect(saw_doctor);
    try std.testing.expect(saw_schema);
}

test "planar-execute schema: run verb carries --plan, --dry-run, --mock-worker, --mock-outcomes, --bypass-reviewer-guard" {
    const gpa = std.testing.allocator;
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const res = try runExecute(gpa, &.{"schema"});
    defer {
        gpa.free(res.stdout);
        gpa.free(res.stderr);
    }

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    const parsed = try std.json.parseFromSlice(Catalog, arena, res.stdout, .{
        .ignore_unknown_fields = true,
    });
    const cat = parsed.value;

    // Find the "run" command entry by its full command path "planar-execute run".
    var run_cmd: ?@TypeOf(cat.commands[0]) = null;
    for (cat.commands) |cmd| {
        if (std.mem.eql(u8, cmd.command, "planar-execute run")) {
            run_cmd = cmd;
            break;
        }
    }
    const rc = run_cmd orelse {
        std.debug.print("no 'planar-execute run' command in schema output\n", .{});
        return error.TestUnexpectedResult;
    };

    // The run verb must carry the five key flags.
    var saw_plan = false;
    var saw_dry_run = false;
    var saw_mock_worker = false;
    var saw_mock_outcomes = false;
    var saw_bypass = false;
    for (rc.flags) |f| {
        if (std.mem.eql(u8, f.long, "--plan")) saw_plan = true;
        if (std.mem.eql(u8, f.long, "--dry-run")) saw_dry_run = true;
        if (std.mem.eql(u8, f.long, "--mock-worker")) saw_mock_worker = true;
        if (std.mem.eql(u8, f.long, "--mock-outcomes")) saw_mock_outcomes = true;
        if (std.mem.eql(u8, f.long, "--bypass-reviewer-guard")) saw_bypass = true;
    }
    try std.testing.expect(saw_plan);
    try std.testing.expect(saw_dry_run);
    try std.testing.expect(saw_mock_worker);
    try std.testing.expect(saw_mock_outcomes);
    try std.testing.expect(saw_bypass);
}
