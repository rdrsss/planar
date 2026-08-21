//! Verbs that need no database must not touch one (planar task 5663).
//!
//! `--help`, `schema`, and `version` produce pure output. They must neither
//! create a database nor migrate an existing one.
//!
//! This is a regression test for a real incident, twice over. CLI telemetry
//! ran on every invocation and acquired its handle through the MIGRATING open,
//! so merely enumerating verbs with `--help` — which `scripts/coverage-check.sh`
//! does once per verb — silently advanced the operator's live database to a
//! dev build's schema. Every other installed binary then failed with
//! SchemaVersionAhead until someone rolled the migration back by hand. Because
//! the telemetry path swallows its errors, the damage was invisible: help text
//! printed normally while the migration landed.

const std = @import("std");
const harness = @import("harness");

fn envBin(name: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const value: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, value, name) and value.len > name.len and value[name.len] == '=') {
            return value[name.len + 1 ..];
        }
    }
    return null;
}

const bin_vars = [_][]const u8{
    "PLANAR_BIN",
    "PLANAR_AGENT_BIN",
    "PLANAR_WATCH_BIN",
    "PLANAR_EXECUTE_BIN",
};

/// Run `bin args...` with PLANAR_DB pointed at `db_path`.
fn runWithDb(
    gpa: std.mem.Allocator,
    bin: []const u8,
    args: []const []const u8,
    db_path: []const u8,
) void {
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(gpa);
    argv.append(gpa, bin) catch @panic("OOM");
    for (args) |a| argv.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const block: std.process.Environ.PosixBlock = .{ .slice = @ptrCast(raw[0..count :null]) };
    var env = (std.process.Environ{ .block = block }).createMap(gpa) catch @panic("OOM");
    defer env.deinit();
    env.put("PLANAR_DB", db_path) catch @panic("OOM");

    const res = std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .environ_map = &env,
    }) catch @panic("spawn failed");
    gpa.free(res.stdout);
    gpa.free(res.stderr);
}

test "no-database verbs never create a database" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.tmpAbsPath();
    const db_path = try std.fs.path.join(gpa, &.{ root, "must-not-appear.db" });
    defer gpa.free(db_path);

    for (bin_vars) |var_name| {
        const bin = envBin(var_name) orelse continue;
        for ([_][]const []const u8{
            &.{"--help"},
            &.{"version"},
        }) |args| {
            std.Io.Dir.cwd().deleteFile(std.testing.io, db_path) catch {};
            runWithDb(gpa, bin, args, db_path);

            // Creating the database is itself a side effect: a bare `--help`
            // on a fresh machine must not leave one behind.
            const created = blk: {
                std.Io.Dir.cwd().access(std.testing.io, db_path, .{}) catch break :blk false;
                break :blk true;
            };
            if (created) {
                std.debug.print("{s} {s} created a database\n", .{ bin, args[0] });
                return error.NoDatabaseVerbCreatedDatabase;
            }
        }
    }
}

test "no-database verbs never migrate an existing database" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.tmpAbsPath();
    const db_path = try std.fs.path.join(gpa, &.{ root, "empty.db" });
    defer gpa.free(db_path);

    for (bin_vars) |var_name| {
        const bin = envBin(var_name) orelse continue;
        for ([_][]const []const u8{
            &.{"--help"},
            &.{"version"},
        }) |args| {
            // An EMPTY file stands in for "a database this binary is ahead
            // of", which makes the assertion independent of whatever schema
            // version happens to be current.
            std.Io.Dir.cwd().deleteFile(std.testing.io, db_path) catch {};
            try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = db_path, .data = "" });

            runWithDb(gpa, bin, args, db_path);

            // Assert on SCHEMA, not on bytes. Merely opening a SQLite database
            // writes — it initializes a page and sets WAL mode per connection —
            // so "the file is byte-identical" is not an invariant any binary
            // can hold. What must never happen is the schema being created or
            // advanced, and SQLite stores table names as plain text in the
            // file, so the presence of `schema_migrations` is a direct read of
            // "did something migrate this".
            const body = try std.Io.Dir.cwd().readFileAlloc(
                std.testing.io,
                db_path,
                gpa,
                .limited(8 * 1024 * 1024),
            );
            defer gpa.free(body);

            if (std.mem.indexOf(u8, body, "schema_migrations") != null) {
                std.debug.print(
                    "{s} {s} migrated a database it should not have touched\n",
                    .{ bin, args[0] },
                );
                return error.NoDatabaseVerbMigratedDatabase;
            }
        }
    }
}
