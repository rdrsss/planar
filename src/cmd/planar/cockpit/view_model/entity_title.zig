//! Shared entity-title lookup for view-model domains that render links.

const std = @import("std");
const db = @import("db");

pub fn resolve(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: []const u8,
    id: i64,
) ?[]const u8 {
    const sql: [:0]const u8 = if (std.mem.eql(u8, kind, "artifact"))
        "select title from artifacts where id = ?"
    else if (std.mem.eql(u8, kind, "plan"))
        "select title from plans where id = ?"
    else if (std.mem.eql(u8, kind, "task"))
        "select title from tasks where id = ?"
    else if (std.mem.eql(u8, kind, "decision"))
        "select title from decisions where id = ?"
    else if (std.mem.eql(u8, kind, "question"))
        "select title from questions where id = ?"
    else if (std.mem.eql(u8, kind, "test_scenario"))
        "select title from test_scenarios where id = ?"
    else
        return null;

    var stmt = d.prepare(sql) catch return null;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return null;
    return switch (stmt.step() catch return null) {
        .done => null,
        .row => stmt.columnTextAlloc(0, allocator) catch null,
    };
}
