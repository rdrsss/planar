//! planar-execute — Lua 5.4 script execution harness (plan 492).
//!
//! Fifth binary in the Planar family. Unlike the other binaries, this one
//! does NOT open SQLite and does NOT link the runtime / engine / db modules.
//! Its sole dependency in M1 is liblua54 (vendored under vendor/lua/).
//!
//! M1 scope:
//!   task 3209 — vendor Lua, link it, prove the link with a newstate/close
//!               round-trip.
//!   task 3163 — load string → lua_pcall → read return value back into Zig;
//!               surface Lua compile/runtime errors as Zig errors (no panic).
//!
//! What is intentionally absent (later tasks):
//!   - host-function surface (agent/parallel/pipeline/phase/log/budget/
//!     workflow): task 3164 m2-host-fns
//!   - stdlib sandboxing (strip os/io/os.time/math.random): task m2-sandbox
//!   - workflow module loading ({meta, run}): task 3164
//!   - CLI flag parsing / version / --dry-run: tasks 3165/3166

const std = @import("std");
const Io = std.Io;

const c = @cImport({
    @cInclude("lua.h");
    @cInclude("lauxlib.h");
    @cInclude("lualib.h");
});

/// Errors that evalString can surface to Zig callers.
pub const LuaError = error{
    /// luaL_newstate returned null — allocator failure.
    LuaAllocFailed,
    /// The script failed to compile (syntax error).
    LuaCompileError,
    /// The script compiled but raised a runtime error.
    LuaRuntimeError,
    /// The script ran successfully but returned a type that is not a number.
    /// M1 scope: evalString decodes only numeric return values.
    LuaUnexpectedType,
};

/// evalString compiles and executes the given Lua source string inside a
/// fresh, isolated lua_State.  The script MUST return exactly one numeric
/// value via `return <expr>`.  On success the number is returned as f64
/// and the state is closed.
///
/// On any error the Lua error message is written into `err_buf`
/// (null-terminated, truncated to fit), the state is closed, and a LuaError
/// is returned — no panic, no silent swallow.
///
/// Sandboxing note: no stdlib libraries are opened.  A pure-computation
/// script (arithmetic) needs no libraries.  M2 m2-sandbox will register the
/// permitted trimmed subset.
///
/// The Lua C macro `lua_pcall` and `lua_tonumber` expand to inline wrappers
/// that pass C `NULL` for optional pointer parameters.  Zig's cImport maps
/// `NULL` to `?*anyopaque`, which does not unify with the typed pointer
/// parameters (`lua_KFunction`, `[*c]c_int`).  We call the underlying
/// `lua_pcallk` and `lua_tonumberx` directly with typed `null` instead.
///
/// Thread safety: each call creates its own lua_State — safe to call from
/// concurrent Zig threads as long as they do not share the state.
pub fn evalString(source: [*:0]const u8, err_buf: []u8) LuaError!f64 {
    const L = c.luaL_newstate();
    if (L == null) return LuaError.LuaAllocFailed;
    defer c.lua_close(L);

    // luaL_loadstring compiles the source into a Lua function and pushes it.
    // On failure it pushes an error message string instead.
    const load_rc = c.luaL_loadstring(L, source);
    if (load_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaCompileError;
    }

    // lua_pcallk: call the compiled chunk with no arguments, one return value,
    // no error-handler function.  ctx=0, k=null (no continuation).
    const call_rc = c.lua_pcallk(L, 0, 1, 0, 0, null);
    if (call_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaRuntimeError;
    }

    // Read the numeric return value off the top of the stack (-1).
    // lua_tonumberx: pass null for the isnum out-param; check the type tag
    // manually to avoid a second layer of option indirection.
    if (c.lua_type(L, -1) != c.LUA_TNUMBER) {
        return LuaError.LuaUnexpectedType;
    }
    const n = c.lua_tonumberx(L, -1, null);
    return @floatCast(n);
}

/// captureError reads the error message off the top of the Lua stack
/// (placed there by luaL_loadstring or lua_pcallk on failure) and writes it
/// into err_buf, null-terminated, truncated to fit.  Pops the value.
///
/// lua_tostring is a C macro wrapping lua_tolstring with NULL — same
/// ?*anyopaque mismatch.  Call lua_tolstring directly with null.
fn captureError(L: ?*c.lua_State, err_buf: []u8) void {
    if (err_buf.len == 0) {
        c.lua_settop(L, -(1) - 1); // lua_pop(L, 1)
        return;
    }
    // lua_tolstring returns a [*c]const u8 — a C-style null-terminated string.
    // Cast to [*:0] for std.mem.span.
    const raw = c.lua_tolstring(L, -1, null);
    if (raw == null) {
        err_buf[0] = 0;
    } else {
        const sentinel: [*:0]const u8 = @ptrCast(raw);
        const src = std.mem.span(sentinel);
        const copy_len = @min(src.len, err_buf.len - 1);
        @memcpy(err_buf[0..copy_len], src[0..copy_len]);
        err_buf[copy_len] = 0;
    }
    c.lua_settop(L, -(1) - 1); // lua_pop(L, 1)
}

var stdout_buf: [1024]u8 = undefined;

pub fn main(init: std.process.Init) !void {
    var stdout_writer = Io.File.Writer.init(.stdout(), init.io, &stdout_buf);
    const out = &stdout_writer.interface;

    try out.print("planar-execute {s}\n", .{c.LUA_VERSION});

    // Prove the link works: create and close a Lua state.
    const L = c.luaL_newstate();
    if (L == null) {
        std.debug.print("planar-execute: luaL_newstate returned null\n", .{});
        std.process.exit(1);
    }
    c.lua_close(L);
    try out.print("lua state ok\n", .{});

    // Exercise the load→execute→read-return-value path.
    var err_buf: [256]u8 = undefined;
    const result = evalString("return 6 * 7", &err_buf) catch |e| {
        try out.print("evalString error: {s} — {s}\n", .{ @errorName(e), err_buf });
        try out.flush();
        std.process.exit(1);
    };
    try out.print("eval result: {d}\n", .{result});

    try out.flush();
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

test "lua state round-trip" {
    // Create a Lua state, assert it is non-null, then close it.
    // M1 acceptance signal: the static library links and the Lua allocator
    // is functional.
    const L = c.luaL_newstate();
    try std.testing.expect(L != null);
    c.lua_close(L);
}

test "evalString: arithmetic expression returns correct number" {
    // A script that computes 6 * 7 must return 42.0.
    var err_buf: [256]u8 = undefined;
    const result = try evalString("return 6 * 7", &err_buf);
    try std.testing.expectEqual(@as(f64, 42.0), result);
}

test "evalString: compile error surfaces as LuaCompileError" {
    // A syntactically invalid script must yield LuaCompileError, not a panic.
    var err_buf: [256]u8 = undefined;
    const err = evalString("this is not valid lua @@@@", &err_buf);
    try std.testing.expectError(LuaError.LuaCompileError, err);
    // err_buf must contain a non-empty error message from the Lua parser.
    try std.testing.expect(err_buf[0] != 0);
}

test "evalString: runtime error surfaces as LuaRuntimeError" {
    // A script that calls error() at runtime must yield LuaRuntimeError.
    var err_buf: [256]u8 = undefined;
    const err = evalString("error('boom')", &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    try std.testing.expect(err_buf[0] != 0);
}

test "evalString: multi-step computation" {
    // Statements before `return` must execute and accumulate correctly.
    var err_buf: [256]u8 = undefined;
    const result = try evalString(
        \\local x = 10
        \\local y = x * x + 2 * x + 1
        \\return y
    , &err_buf);
    try std.testing.expectEqual(@as(f64, 121.0), result);
}
