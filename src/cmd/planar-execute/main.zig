//! planar-execute — Lua 5.4 script execution harness (plan 492).
//!
//! Fifth binary in the Planar family. Unlike the other binaries, this one
//! does NOT open SQLite and does NOT link the runtime / engine / db modules.
//! Its sole dependency in M1 is liblua54 (vendored under vendor/lua/).
//!
//! M1 scope (task 3209): vendor Lua, link it, prove the link works via a
//! newstate/close round-trip. Actual script execution, module loading, and
//! CLI flag parsing are implemented in tasks 3163–3166.

const std = @import("std");
const Io = std.Io;

const c = @cImport({
    @cInclude("lua.h");
    @cInclude("lauxlib.h");
    @cInclude("lualib.h");
});

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
    try out.flush();
}

test "lua state round-trip" {
    // Create a Lua state, assert it is non-null, then close it.
    // This is the M1 acceptance signal: the static library links and
    // the Lua allocator is functional.
    const L = c.luaL_newstate();
    try std.testing.expect(L != null);
    c.lua_close(L);
}
