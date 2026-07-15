//! Black-box coverage for the standalone authored-surface validator.

const std = @import("std");

pub fn main(init: std.process.Init) !void {
    const allocator = init.arena.allocator();
    const args = try init.minimal.args.toSlice(allocator);
    try expect(args.len == 7);
    try testDirtyCorpus(allocator, init.io, args[1], args[2]);
    try testCleanCorpus(allocator, init.io, args[1], args[2]);
    try testLiveSchemaInventory(allocator, init.io, args[1], args[3..7]);
    try std.Io.File.stdout().writeStreamingAll(init.io, "All 3 surface-lint black-box tests passed.\n");
}

fn testLiveSchemaInventory(allocator: std.mem.Allocator, io: std.Io, surface_bin: []const u8, schema_bins: []const []const u8) !void {
    var random_bytes: [8]u8 = undefined;
    io.random(&random_bytes);
    const suffix = std.fmt.bytesToHex(random_bytes, .lower);
    const tmp_root = try std.fmt.allocPrint(allocator, "{s}/planar-surface-lint-{s}", .{ tmpBase(), suffix });
    try std.Io.Dir.cwd().createDirPath(io, tmp_root);
    defer std.Io.Dir.cwd().deleteTree(io, tmp_root) catch {};

    const inherited_db = try std.fs.path.join(allocator, &.{ tmp_root, "inherited.db" });
    const isolated_db = try std.fs.path.join(allocator, &.{ tmp_root, "schema.db" });
    var env_map = try currentEnviron().createMap(allocator);
    defer env_map.deinit();

    const inventory_run = try run(allocator, io, surface_bin, &.{"--command-inventory-json"});
    try expectExit(inventory_run.term, 0);
    try expectEqual("", inventory_run.stderr);
    const Inventory = struct {
        version: u8,
        commands: []const struct { command: []const u8, access: []const u8 },
    };
    const inventory = try std.json.parseFromSlice(Inventory, allocator, inventory_run.stdout, .{ .ignore_unknown_fields = false });
    try expect(inventory.value.version == 1);

    var classified: std.StringHashMapUnmanaged(void) = .empty;
    for (inventory.value.commands) |entry| {
        try expect(std.mem.eql(u8, entry.access, "read") or std.mem.eql(u8, entry.access, "mutate"));
        const result = try classified.getOrPut(allocator, entry.command);
        try expect(!result.found_existing);
    }

    const Command = struct { command: []const u8, subcommands: []const []const u8 = &.{} };
    const Schema = struct { commands: []const Command };
    var live: std.StringHashMapUnmanaged(void) = .empty;
    for (schema_bins) |bin| {
        // Start each invocation with a sentinel inherited DB, then override it
        // in the schema runner. If that isolation is ever removed, the fresh
        // binary will create/migrate inherited.db and this test fails.
        try env_map.put("PLANAR_DB", inherited_db);
        const schema_run = try runWithDb(allocator, io, bin, &.{"schema"}, &env_map, isolated_db);
        try expectExit(schema_run.term, 0);
        try expectEqual("", schema_run.stderr);
        const schema = try std.json.parseFromSlice(Schema, allocator, schema_run.stdout, .{ .ignore_unknown_fields = true });
        for (schema.value.commands) |command| {
            if (command.command.len == 0 or command.subcommands.len != 0) continue;
            const result = try live.getOrPut(allocator, command.command);
            try expect(!result.found_existing);
        }
    }

    try expect(classified.count() == live.count());
    var classified_it = classified.keyIterator();
    while (classified_it.next()) |command| try expect(live.contains(command.*));
    var live_it = live.keyIterator();
    while (live_it.next()) |command| try expect(classified.contains(command.*));

    std.Io.Dir.cwd().access(io, inherited_db, .{}) catch |err| switch (err) {
        error.FileNotFound => return,
        else => return err,
    };
    return error.TestExpectedEqual;
}

fn testDirtyCorpus(allocator: std.mem.Allocator, io: std.Io, bin: []const u8, fixtures: []const u8) !void {
    const dirty = try std.fs.path.join(allocator, &.{ fixtures, "dirty" });
    const text = try run(allocator, io, bin, &.{dirty});
    try expectExit(text.term, 1);
    try expectEqual("", text.stderr);
    const expected_lines = [_][]const u8{
        "agents/drift.md:6: surface-link-missing:",
        "agents/drift.md:7: surface-legacy-reference:",
        "agents/drift.md:8: surface-legacy-reference:",
        "agents/drift.md:9: surface-artifact-set-drift:",
        "agents/drift.md:10: surface-command-drift:",
        "agents/drift.md:11: surface-capability-drift:",
        "skills/src/missing-contract.md:1: surface-contract-missing:",
        "surface-lint: 7 finding(s) across 2 files\n",
    };
    var last: usize = 0;
    for (expected_lines) |needle| {
        const at = std.mem.indexOfPos(u8, text.stdout, last, needle) orelse return error.TestExpectedEqual;
        last = at + needle.len;
    }

    const json_run = try run(allocator, io, bin, &.{ dirty, "--json" });
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
    try expect(parsed.value.findings.len == 7);
    const codes = [_][]const u8{
        "surface-link-missing",  "surface-legacy-reference", "surface-legacy-reference", "surface-artifact-set-drift",
        "surface-command-drift", "surface-capability-drift", "surface-contract-missing",
    };
    for (codes, parsed.value.findings) |code, finding| try expectEqual(code, finding.code);
}

fn testCleanCorpus(allocator: std.mem.Allocator, io: std.Io, bin: []const u8, fixtures: []const u8) !void {
    const clean = try std.fs.path.join(allocator, &.{ fixtures, "clean" });
    const result = try run(allocator, io, bin, &.{clean});
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

fn runWithDb(
    allocator: std.mem.Allocator,
    io: std.Io,
    bin: []const u8,
    extra: []const []const u8,
    env_map: *std.process.Environ.Map,
    db_path: []const u8,
) !std.process.RunResult {
    var argv: std.ArrayList([]const u8) = .empty;
    try argv.append(allocator, bin);
    try argv.appendSlice(allocator, extra);
    try env_map.put("PLANAR_DB", db_path);
    return std.process.run(allocator, io, .{ .argv = argv.items, .environ_map = env_map });
}

fn currentEnviron() std.process.Environ {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    return .{ .block = .{ .slice = env_slice } };
}

fn tmpBase() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const value: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, value, "TMPDIR=") and value.len > "TMPDIR=".len) {
            const tmp = value["TMPDIR=".len..];
            return if (tmp[tmp.len - 1] == '/') tmp[0 .. tmp.len - 1] else tmp;
        }
    }
    return "/tmp";
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
