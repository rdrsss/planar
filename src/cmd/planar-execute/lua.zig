//! lua.zig — the single `@cImport` of the vendored Lua 5.5 C API for the
//! planar-execute binary.
//!
//! Both `main.zig` (the host-function surface + the run/load paths) and
//! `scheduler.zig` (the M5 coroutine event loop, task 3182) need the same Lua C
//! types — most importantly `lua_State`, so a `*lua_State` produced by
//! `lua_newthread` in one module is the same type the other module's
//! continuation operates on. A `@cImport` produces a fresh type set per
//! invocation, so importing the header twice would make `main`'s `lua_State`
//! distinct from `scheduler`'s. Funneling the import through this one module
//! gives every consumer the SAME type set.
//!
//! Consumers do `const lua = @import("lua.zig"); const c = lua.c;` and use
//! `c.lua_*` exactly as before.

pub const c = @cImport({
    @cInclude("lua.h");
    @cInclude("lauxlib.h");
    @cInclude("lualib.h");
});
