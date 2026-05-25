//! Minimal SQLite wrapper.
//!
//! Only enough surface for the migrations runner: open(:memory: or path),
//! exec a SQL script, scalar-int query, close. The rest of the binary
//! grows on top of this as needs appear.
const std = @import("std");
const c = @cImport({
    @cInclude("sqlite3.h");
});

pub const Error = error{
    OpenFailed,
    ExecFailed,
    PrepareFailed,
    StepFailed,
    BindFailed,
};

/// One bound parameter for a prepared statement. `null` binds SQL NULL.
///
/// Text bindings use SQLITE_STATIC (no copy), so the slice must remain
/// valid for the duration of the `execParams` call. That's always true
/// since `execParams` is synchronous: bind → step → finalize all happen
/// before the function returns. Don't store `Param.text` values across
/// `execParams` boundaries.
pub const Param = union(enum) {
    null: void,
    int: i64,
    text: []const u8,
};

/// A prepared statement. Returned by `Db.prepare`; callers bind
/// parameters, step (possibly many times for SELECTs), read columns,
/// and finalize. Never share a Stmt across threads.
pub const Stmt = struct {
    handle: *c.sqlite3_stmt,

    pub const StepResult = enum { row, done };

    pub fn finalize(self: *Stmt) void {
        _ = c.sqlite3_finalize(self.handle);
    }

    /// Bind params positionally (1-indexed in SQL, 0-indexed here).
    /// Text uses SQLITE_STATIC — slices must live until the next
    /// step/finalize.
    pub fn bind(self: *Stmt, params: []const Param) Error!void {
        for (params, 0..) |p, i| {
            const idx: c_int = @intCast(i + 1);
            const rc: c_int = switch (p) {
                .null => c.sqlite3_bind_null(self.handle, idx),
                .int => |v| c.sqlite3_bind_int64(self.handle, idx, v),
                .text => |s| c.sqlite3_bind_text(self.handle, idx, s.ptr, @intCast(s.len), null),
            };
            if (rc != c.SQLITE_OK) return error.BindFailed;
        }
    }

    /// Step once. Returns `.row` if a row is available (read columns),
    /// `.done` if the statement is complete. Any other rc → StepFailed.
    pub fn step(self: *Stmt) Error!StepResult {
        return switch (c.sqlite3_step(self.handle)) {
            c.SQLITE_ROW => .row,
            c.SQLITE_DONE => .done,
            else => error.StepFailed,
        };
    }

    pub fn columnIsNull(self: *Stmt, idx: c_int) bool {
        return c.sqlite3_column_type(self.handle, idx) == c.SQLITE_NULL;
    }

    pub fn columnInt(self: *Stmt, idx: c_int) i64 {
        return c.sqlite3_column_int64(self.handle, idx);
    }

    pub fn columnIntOpt(self: *Stmt, idx: c_int) ?i64 {
        if (self.columnIsNull(idx)) return null;
        return self.columnInt(idx);
    }

    /// Read a REAL (double-precision float) column. FTS5 ranking
    /// functions such as bm25() emit REAL values; use this method to
    /// read them back without silent integer truncation.
    pub fn columnDouble(self: *Stmt, idx: c_int) f64 {
        return c.sqlite3_column_double(self.handle, idx);
    }

    /// Read a TEXT column into an allocator-owned slice. SQLite's
    /// internal buffer goes away after `step` or `finalize`, so we
    /// must copy before either.
    pub fn columnTextAlloc(
        self: *Stmt,
        idx: c_int,
        allocator: std.mem.Allocator,
    ) std.mem.Allocator.Error![]const u8 {
        const ptr = c.sqlite3_column_text(self.handle, idx);
        const len: usize = @intCast(c.sqlite3_column_bytes(self.handle, idx));
        if (ptr == null or len == 0) return try allocator.dupe(u8, "");
        return try allocator.dupe(u8, ptr[0..len]);
    }

    pub fn columnTextOpt(
        self: *Stmt,
        idx: c_int,
        allocator: std.mem.Allocator,
    ) std.mem.Allocator.Error!?[]const u8 {
        if (self.columnIsNull(idx)) return null;
        return try self.columnTextAlloc(idx, allocator);
    }
};

pub const Db = struct {
    handle: *c.sqlite3,

    pub fn openMemory() Error!Db {
        return open(":memory:");
    }

    pub fn open(path: [*:0]const u8) Error!Db {
        var h: ?*c.sqlite3 = null;
        const rc = c.sqlite3_open(path, &h);
        if (rc != c.SQLITE_OK or h == null) {
            if (h) |hh| _ = c.sqlite3_close(hh);
            return error.OpenFailed;
        }
        return .{ .handle = h.? };
    }

    pub fn close(self: *Db) void {
        _ = c.sqlite3_close(self.handle);
    }

    /// Execute one or more SQL statements separated by `;`. SQLite parses
    /// the full script; we do not split.
    pub fn exec(self: *Db, sql: [:0]const u8) Error!void {
        var errmsg: [*c]u8 = null;
        const rc = c.sqlite3_exec(self.handle, sql.ptr, null, null, &errmsg);
        if (rc != c.SQLITE_OK) {
            if (errmsg != null) {
                std.log.err("sqlite exec failed (rc={d}): {s}", .{ rc, errmsg });
                c.sqlite3_free(errmsg);
            }
            return error.ExecFailed;
        }
    }

    /// Execute a SQL script that is not guaranteed null-terminated by
    /// duplicating it into a sentinel-terminated buffer first.
    pub fn execSlice(self: *Db, alloc: std.mem.Allocator, sql: []const u8) !void {
        const z = try alloc.dupeZ(u8, sql);
        defer alloc.free(z);
        try self.exec(z);
    }

    /// Run a single SELECT that returns one int64 column in one row.
    pub fn intQuery(self: *Db, sql: [:0]const u8) Error!i64 {
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(self.handle, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) {
            return error.PrepareFailed;
        }
        defer _ = c.sqlite3_finalize(stmt);
        if (c.sqlite3_step(stmt) != c.SQLITE_ROW) return error.StepFailed;
        return c.sqlite3_column_int64(stmt, 0);
    }

    /// Prepare a statement for repeated bind/step calls. Caller must
    /// `defer stmt.finalize()`.
    pub fn prepare(self: *Db, sql: [:0]const u8) Error!Stmt {
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(self.handle, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) {
            return error.PrepareFailed;
        }
        return .{ .handle = stmt.? };
    }

    /// Last SQLite errno on this connection. Useful right after a
    /// failed exec/step to distinguish constraint violations
    /// (`SQLITE_CONSTRAINT_*`) from other failure modes.
    pub fn errCode(self: *Db) c_int {
        return c.sqlite3_extended_errcode(self.handle);
    }

    /// True when the last failure was a uniqueness-style constraint
    /// violation — UNIQUE index OR composite PRIMARY KEY collision.
    /// Both fire on "you tried to insert a row whose key already
    /// exists," which is what callers actually want to detect.
    pub fn lastWasUniqueViolation(self: *Db) bool {
        const code = self.errCode();
        return code == c.SQLITE_CONSTRAINT_UNIQUE or
            code == c.SQLITE_CONSTRAINT_PRIMARYKEY;
    }

    /// Prepare `sql`, bind `params` positionally (1-indexed in SQL,
    /// 0-indexed in the slice), step once, return the last insert
    /// rowid. Use for parameterized INSERT/UPDATE/DELETE where you
    /// don't need to read columns back.
    ///
    /// Text params are bound with `SQLITE_TRANSIENT` so SQLite copies
    /// them — the caller's buffers don't have to outlive this call.
    pub fn execParams(self: *Db, sql: [:0]const u8, params: []const Param) Error!i64 {
        var stmt: ?*c.sqlite3_stmt = null;
        if (c.sqlite3_prepare_v2(self.handle, sql.ptr, -1, &stmt, null) != c.SQLITE_OK) {
            return error.PrepareFailed;
        }
        defer _ = c.sqlite3_finalize(stmt);

        for (params, 0..) |p, i| {
            const idx: c_int = @intCast(i + 1); // SQLite binds are 1-indexed.
            const rc: c_int = switch (p) {
                .null => c.sqlite3_bind_null(stmt, idx),
                .int => |v| c.sqlite3_bind_int64(stmt, idx, v),
                // SQLITE_STATIC: SQLite won't copy. Safe because the
                // slice lives until execParams returns, after step+finalize.
                .text => |s| c.sqlite3_bind_text(stmt, idx, s.ptr, @intCast(s.len), null),
            };
            if (rc != c.SQLITE_OK) return error.BindFailed;
        }

        const step_rc = c.sqlite3_step(stmt);
        if (step_rc != c.SQLITE_DONE and step_rc != c.SQLITE_ROW) {
            return error.StepFailed;
        }
        return c.sqlite3_last_insert_rowid(self.handle);
    }

    /// Start a named SQLite savepoint on this connection.
    pub fn savepoint(self: *Db, alloc: std.mem.Allocator, name: []const u8) !void {
        const sql = try std.fmt.allocPrint(alloc, "savepoint {s}", .{name});
        defer alloc.free(sql);
        try self.execSlice(alloc, sql);
    }

    /// Roll back to a named savepoint on this connection.
    pub fn rollbackToSavepoint(self: *Db, alloc: std.mem.Allocator, name: []const u8) !void {
        const sql = try std.fmt.allocPrint(alloc, "rollback to savepoint {s}", .{name});
        defer alloc.free(sql);
        try self.execSlice(alloc, sql);
    }

    /// Release a named savepoint on this connection.
    pub fn releaseSavepoint(self: *Db, alloc: std.mem.Allocator, name: []const u8) !void {
        const sql = try std.fmt.allocPrint(alloc, "release savepoint {s}", .{name});
        defer alloc.free(sql);
        try self.execSlice(alloc, sql);
    }
};

test "open in-memory db and round-trip an int" {
    var db = try Db.openMemory();
    defer db.close();
    try db.exec("create table t (n integer); insert into t values (42);");
    try std.testing.expectEqual(@as(i64, 42), try db.intQuery("select n from t"));
}

test "execParams round-trips bind types" {
    var db = try Db.openMemory();
    defer db.close();
    try db.exec("create table t (a integer, b text, c text);");
    const rowid = try db.execParams(
        "insert into t (a, b, c) values (?, ?, ?)",
        &.{ .{ .int = 7 }, .{ .text = "hello" }, .{ .null = {} } },
    );
    try std.testing.expect(rowid > 0);
    try std.testing.expectEqual(@as(i64, 7), try db.intQuery("select a from t"));
    try std.testing.expectEqual(@as(i64, 1), try db.intQuery("select count(*) from t where b = 'hello'"));
    try std.testing.expectEqual(@as(i64, 1), try db.intQuery("select count(*) from t where c is null"));
}

test "savepoint helpers roll back partial writes" {
    const a = std.testing.allocator;
    var db = try Db.openMemory();
    defer db.close();
    try db.exec("create table t (v integer);");
    try db.exec("insert into t (v) values (1);");

    try db.savepoint(a, "sp1");
    try db.exec("insert into t (v) values (2);");
    try db.rollbackToSavepoint(a, "sp1");
    try db.releaseSavepoint(a, "sp1");

    try std.testing.expectEqual(@as(i64, 1), try db.intQuery("select count(*) from t"));
    try std.testing.expectEqual(@as(i64, 1), try db.intQuery("select v from t limit 1"));
}
