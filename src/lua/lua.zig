//! Lua 5.5 C-API bindings stub.
//!
//! This module is the linkage anchor for the vendored Lua 5.5 static
//! library (`vendor/lua/`). It does not expose a high-level Zig API —
//! that is the concern of the planar-execute engine (P0.2+). Its sole
//! purpose in P0.1 is to:
//!   1. Pull `lua_mod` (the static lib) into the `zig build test` graph
//!      so the linker actually exercises the Lua object files.
//!   2. Provide a canary unit test that creates a Lua state and executes
//!      a trivial chunk, proving end-to-end linkage without requiring a
//!      system Lua installation.
//!
//! Implementation note: Lua 5.5 defines both `lua_pcall` and `luaL_dostring`
//! as macros that chain through `lua_pcallk(..., 0, NULL)`. Zig's cimport is
//! strict about the NULL-to-function-pointer cast those macros produce, so we
//! call `lua_pcallk` directly with an explicit null `lua_KFunction`. The
//! functional outcome is identical to `luaL_dostring`.
//!
//! Consumers that need raw C-API access should `@cImport` the Lua headers
//! directly via their own module (or extend this one once a wrapper is needed).

const std = @import("std");
const c = @cImport({
    @cInclude("lua.h");
    @cInclude("lauxlib.h");
    @cInclude("lualib.h");
});

/// LUA_OK is 0 in Lua 5.5. Defined here so callers do not have to reach
/// into the C namespace for a named constant.
pub const lua_ok: c_int = 0;

/// LUA_MULTRET constant (let the callee push as many return values as it wants).
pub const lua_multret: c_int = c.LUA_MULTRET;

/// doString loads and executes `chunk` in the given Lua state.
/// Returns LUA_OK (0) on success, a Lua error code otherwise.
/// Equivalent to `luaL_dostring` without relying on the macro chain that
/// passes NULL as a lua_KFunction, which Zig's cimport rejects.
pub fn doString(L: *c.lua_State, chunk: [*:0]const u8) c_int {
    const load_rc = c.luaL_loadstring(L, chunk);
    if (load_rc != lua_ok) return load_rc;
    // Call with no args, receive all returns, no error handler, no continuation.
    return c.lua_pcallk(L, 0, lua_multret, 0, 0, null);
}

test "lua smoke: state create, dostring, close" {
    // Open a new Lua state. luaL_newstate() uses the standard C allocator.
    const L = c.luaL_newstate();
    try std.testing.expect(L != null);
    defer c.lua_close(L);

    // Open the standard libraries (math, string, etc.) so assert resolves.
    c.luaL_openlibs(L);

    // Execute a trivial chunk that asserts 1+1 == 2.
    // A non-zero return means the chunk failed; the test asserts LUA_OK.
    const chunk = "local x = 1 + 1; assert(x == 2)";
    const rc = doString(L.?, chunk);
    try std.testing.expectEqual(lua_ok, rc);
}
