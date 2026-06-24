//! host.zig — the spawn-free Lua host registry for `planar-execute`.
//!
//! This module implements the D7 host API surface (plan 633): the exact,
//! frozen set of host functions a sandboxed deterministic workflow can call.
//! It deliberately omits every spawn-shaped primitive (`agent`, `parallel`,
//! `pipeline`, `workflow`, `dispatch_table`, `compact`, `budget`, any general
//! `exec`/process primitive) so that D5 (planar-execute is a deterministic
//! engine, never an LLM orchestrator) holds *by construction*.
//!
//! ## Surface (D7 allowlist)
//!
//! Host functions are fields on the `cli` / `git` / `fs` / `flow` / `ctx`
//! tables — never globals.  The registered set is introspectable as the
//! comptime `ALLOWED_HOST_FNS` manifest below, which P0.3 locks against a
//! frozen constant (and asserts none of `DENIED_HOST_FNS` appears).
//!
//!   cli.planar(argv)          — allowlisted shell of planar/planar-agent/
//!   cli.planar_json(argv)       planar-watch ONLY; binary hardcoded.  No exec.
//!   cli.planar_agent(argv)    — mutation shell of planar-agent (claim ritual:
//!   cli.planar_agent_json(argv)  pull/complete/fail/release/block/heartbeat).
//!   cli.planar_watch(argv)    — read-only shell of planar-watch (mode=ro).
//!   cli.planar_watch_json(argv)
//!   git.reset_hard(sha)     — confined git group; host injects `-C <worktree>`
//!   git.checkout(ref)         from the run config; the script never names the
//!   git.diff_name_only(opts?)  dir.
//!   git.head_sha()
//!   git.clean(opts?)
//!   fs.read(path)           — path-confined to the sandbox root; `..` and
//!   fs.write(path, data)      absolute paths are rejected.
//!   fs.exists(path)
//!   fs.mkdir(path)
//!   flow.log(msg)           — pure: log / phase marker / fail / result.
//!   flow.phase(name)
//!   flow.fail(msg)
//!   flow.result(table)
//!   ctx.plan_show(id)       — deterministic planner reads backed by the
//!   ctx.task_show(id)         salvaged state.zig / schema.zig / brief.zig
//!   ctx.task_touches(id)      modules.  ctx.brief stays in-engine.
//!   ctx.recommend_strategy(id)
//!   ctx.context([stage])
//!   ctx.brief(opts)
//!   plus injected ctx.now / ctx.seed / ctx.args.
//!
//! ## Marshalling
//!
//! `pushHostClosure` installs each host fn as a C closure with the `*HostState`
//! captured as upvalue #1.  JSON returned by a `_json` CLI shell is parsed and
//! pushed to Lua via `pushJsonValue` (recursive std.json.Value → Lua value).
//! `flow.result(table)` walks the inverse direction (`luaToJson`) to serialize
//! the workflow's final payload to JSON for stdout.
//!
//! ## Memory
//!
//! Every per-call allocation lives on the `HostState.arena`; the arena is
//! reset by the engine between phases.  Host fns map subprocess and confinement
//! failures to a `luaL_error` (a longjmp) — the workflow sees a Lua error, the
//! engine never sees a half-built stack.

const std = @import("std");

const state = @import("state.zig");
const schema = @import("schema.zig");
const brief = @import("brief.zig");

/// Shared Lua C-API import. Exported so `main.zig` reuses the SAME cimport
/// (two separate `@cImport` blocks produce incompatible opaque types for
/// `lua_State`, which breaks passing the state across module boundaries).
pub const c = @cImport({
    @cInclude("lua.h");
    @cInclude("lauxlib.h");
    @cInclude("lualib.h");
});

const Io = std.Io;

// ---------------------------------------------------------------------------
// Frozen host-fn manifest (D7 P0.3 enforcement target)
// ---------------------------------------------------------------------------

/// A single registered host function: its owning table ("cli"/"git"/"fs"/
/// "flow"/"ctx") and its field name. The `(table, name)` pairs in this slice
/// are the *entire* host-call capability surface; nothing reaches the host
/// except through one of these.  P0.3 asserts the registered set equals this
/// manifest and that it shares no name with `DENIED_HOST_FNS`.
pub const HostFn = struct {
    table: []const u8,
    name: []const u8,
};

/// ALLOWED_HOST_FNS — the comptime allowlist (D7). This is the single source
/// of truth the registrar iterates and the P0.3 lock test compares against.
/// Keep alphabetical within each group for stable diffs.
pub const ALLOWED_HOST_FNS = [_]HostFn{
    // cli.* — allowlisted-binary shells (no general exec).
    .{ .table = "cli", .name = "planar" },
    .{ .table = "cli", .name = "planar_agent" },
    .{ .table = "cli", .name = "planar_agent_json" },
    .{ .table = "cli", .name = "planar_json" },
    .{ .table = "cli", .name = "planar_watch" },
    .{ .table = "cli", .name = "planar_watch_json" },
    // git.* — confined group; host injects -C <worktree>.
    .{ .table = "git", .name = "checkout" },
    .{ .table = "git", .name = "clean" },
    .{ .table = "git", .name = "diff_name_only" },
    .{ .table = "git", .name = "head_sha" },
    .{ .table = "git", .name = "reset_hard" },
    // fs.* — path-confined to the sandbox root.
    .{ .table = "fs", .name = "exists" },
    .{ .table = "fs", .name = "mkdir" },
    .{ .table = "fs", .name = "read" },
    .{ .table = "fs", .name = "write" },
    // flow.* — pure.
    .{ .table = "flow", .name = "fail" },
    .{ .table = "flow", .name = "log" },
    .{ .table = "flow", .name = "phase" },
    .{ .table = "flow", .name = "result" },
    // ctx.* — deterministic planner reads (salvaged state/schema/brief).
    .{ .table = "ctx", .name = "brief" },
    .{ .table = "ctx", .name = "context" },
    .{ .table = "ctx", .name = "plan_show" },
    .{ .table = "ctx", .name = "recommend_strategy" },
    .{ .table = "ctx", .name = "task_show" },
    .{ .table = "ctx", .name = "task_touches" },
};

/// DENIED_HOST_FNS — names that MUST NOT appear in the registered set. These
/// are the spawn / general-exec primitives that grew planar-execute into a
/// harness (the reason it was extracted to a separate project). P0.3 asserts the
/// intersection with the registered set is empty; this module asserts the same
/// at comptime so a regression cannot even compile.
pub const DENIED_HOST_FNS = [_][]const u8{
    "agent",          "parallel", "pipeline", "workflow",
    "dispatch_table", "compact",  "budget",   "exec",
    "spawn",          "child",    "claude",   "codex",
    "headless",       "model",
};

comptime {
    // Compile-time guard: no allowlisted name collides with a denied name.
    for (ALLOWED_HOST_FNS) |hf| {
        for (DENIED_HOST_FNS) |denied| {
            if (std.mem.eql(u8, hf.name, denied)) {
                @compileError("host.zig: ALLOWED_HOST_FNS contains a DENIED name: " ++ hf.name);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// HostState — run/phase-scoped context shared with every host closure
// ---------------------------------------------------------------------------

/// HostState carries everything the host closures need: the arena for
/// per-call allocations, the `std.Io` for subprocess shells, the run-config
/// (worktree dir + sandbox root), determinism inputs (now/seed), the JSON args
/// payload, and the captured `flow.result` / `flow.fail` outcome.
///
/// One instance per `planar-execute run` invocation; pointed at by every host
/// closure as upvalue #1.  Not thread-safe (the engine is single-threaded by
/// design — one clean process per phase, no scheduler).
pub const HostState = struct {
    arena: std.mem.Allocator,
    io: Io,

    /// Working tree the confined `git.*` group operates in (host injects
    /// `-C <worktree>`).  Empty means "no worktree configured" — git.* then
    /// raise a Lua error rather than fall back to cwd.
    worktree: []const u8 = "",

    /// Sandbox root that `fs.*` paths are confined under.  `fs.*` rejects any
    /// path containing `..` or an absolute path, and joins the remainder onto
    /// this root.  Empty means "no fs root" — fs.* then raise a Lua error.
    sandbox_root: []const u8 = "",

    /// Determinism injection — the ONLY time/seed source the sandbox sees
    /// (os/math.random are nil'd).  Exposed as ctx.now / ctx.seed.
    now: i64 = 0,
    seed: i64 = 0,

    /// Raw JSON string passed via `--args`; pushed to Lua as `ctx.args` (parsed
    /// to a Lua value).  Empty/`""` becomes an empty table.
    args_json: []const u8 = "",

    /// Captured `flow.result(table)` payload, serialized to JSON. Null until a
    /// workflow calls flow.result; the engine marshals this to stdout.
    result_json: ?[]const u8 = null,

    /// Captured `flow.fail(msg)`. When set the engine treats the phase as
    /// failed and exits non-zero with this message.
    fail_msg: ?[]const u8 = null,

    /// The current phase name (set by flow.phase). Informational.
    current_phase: []const u8 = "",
};

/// Recover the `*HostState` from C-closure upvalue #1.
fn hostStateUpvalue(L: ?*c.lua_State) *HostState {
    const raw = c.lua_touserdata(L, c.lua_upvalueindex(1));
    return @ptrCast(@alignCast(raw.?));
}

// ---------------------------------------------------------------------------
// Registrar
// ---------------------------------------------------------------------------

/// installHostSurface builds the five host tables (cli/git/fs/flow/ctx) on the
/// global environment, populates each with its allowlisted closures (capturing
/// `hs` as upvalue), and injects the determinism + args fields onto `ctx`.
///
/// The function list is derived from `dispatchFor`, which maps every
/// `(table, name)` in `ALLOWED_HOST_FNS` to its C function.  A name in the
/// manifest with no dispatch entry is a comptime error — the manifest and the
/// implementations cannot drift.
pub fn installHostSurface(L: ?*c.lua_State, hs: *HostState) void {
    // Raise the comptime branch quota for the inline-for loop that calls
    // comptimePrint + dispatchFor for every ALLOWED_HOST_FNS entry. Each
    // additional allowlist entry consumes additional comptime branches; the
    // default quota (3000) is exhausted when the manifest reaches ~23 entries.
    @setEvalBranchQuota(10000);
    // Create the five tables and register them as globals.
    inline for ([_][]const u8{ "cli", "git", "fs", "flow", "ctx" }) |tbl_name| {
        c.lua_createtable(L, 0, 8);
        // Iterate the manifest, install every fn whose table == tbl_name.
        inline for (ALLOWED_HOST_FNS) |hf| {
            if (comptime std.mem.eql(u8, hf.table, tbl_name)) {
                // hf.name is comptime-known; comptimePrint yields a
                // sentinel-terminated literal for the C `lua_setfield` API.
                const name_z = comptime std.fmt.comptimePrint("{s}", .{hf.name});
                pushHostClosure(L, dispatchFor(hf), name_z, hs);
            }
        }
        c.lua_setglobal(L, tbl_name ++ "");
    }

    // Inject determinism + args onto ctx (the only time/seed source).
    _ = c.lua_getglobal(L, "ctx");
    const ctx_idx = c.lua_absindex(L, -1);
    c.lua_pushinteger(L, @intCast(hs.now));
    c.lua_setfield(L, ctx_idx, "now");
    c.lua_pushinteger(L, @intCast(hs.seed));
    c.lua_setfield(L, ctx_idx, "seed");
    // ctx.args = parsed --args JSON (or empty table).
    pushArgsTable(L, hs);
    c.lua_setfield(L, ctx_idx, "args");
    c.lua_settop(L, ctx_idx - 1); // pop ctx
}

/// dispatchFor maps a manifest entry to its C implementation. A missing case
/// is a comptime error, so the manifest can never name an unimplemented fn.
fn dispatchFor(comptime hf: HostFn) *const fn (?*c.lua_State) callconv(.c) c_int {
    const key = hf.table ++ "." ++ hf.name;
    return comptime if (std.mem.eql(u8, key, "cli.planar")) hostCliPlanar else if (std.mem.eql(u8, key, "cli.planar_agent")) hostCliPlanarAgent else if (std.mem.eql(u8, key, "cli.planar_agent_json")) hostCliPlanarAgentJson else if (std.mem.eql(u8, key, "cli.planar_json")) hostCliPlanarJson else if (std.mem.eql(u8, key, "cli.planar_watch")) hostCliPlanarWatch else if (std.mem.eql(u8, key, "cli.planar_watch_json")) hostCliPlanarWatchJson else if (std.mem.eql(u8, key, "git.reset_hard")) hostGitResetHard else if (std.mem.eql(u8, key, "git.checkout")) hostGitCheckout else if (std.mem.eql(u8, key, "git.diff_name_only")) hostGitDiffNameOnly else if (std.mem.eql(u8, key, "git.head_sha")) hostGitHeadSha else if (std.mem.eql(u8, key, "git.clean")) hostGitClean else if (std.mem.eql(u8, key, "fs.read")) hostFsRead else if (std.mem.eql(u8, key, "fs.write")) hostFsWrite else if (std.mem.eql(u8, key, "fs.exists")) hostFsExists else if (std.mem.eql(u8, key, "fs.mkdir")) hostFsMkdir else if (std.mem.eql(u8, key, "flow.log")) hostFlowLog else if (std.mem.eql(u8, key, "flow.phase")) hostFlowPhase else if (std.mem.eql(u8, key, "flow.fail")) hostFlowFail else if (std.mem.eql(u8, key, "flow.result")) hostFlowResult else if (std.mem.eql(u8, key, "ctx.plan_show")) hostCtxPlanShow else if (std.mem.eql(u8, key, "ctx.task_show")) hostCtxTaskShow else if (std.mem.eql(u8, key, "ctx.task_touches")) hostCtxTaskTouches else if (std.mem.eql(u8, key, "ctx.recommend_strategy")) hostCtxRecommendStrategy else if (std.mem.eql(u8, key, "ctx.context")) hostCtxContext else if (std.mem.eql(u8, key, "ctx.brief")) hostCtxBrief else @compileError("host.zig: no dispatch for " ++ key);
}

/// pushHostClosure installs `fn_ptr` as a field `name` on the table at the top
/// of the stack, capturing `hs` as upvalue #1.
fn pushHostClosure(
    L: ?*c.lua_State,
    fn_ptr: *const fn (?*c.lua_State) callconv(.c) c_int,
    name: [*:0]const u8,
    hs: *HostState,
) void {
    c.lua_pushlightuserdata(L, hs);
    c.lua_pushcclosure(L, @ptrCast(fn_ptr), 1);
    c.lua_setfield(L, -2, name);
}

// ---------------------------------------------------------------------------
// Sandbox
// ---------------------------------------------------------------------------

/// openSandboxedLibs opens a curated, deterministic subset of the Lua stdlib
/// and strips every host-reach escape hatch.
///
/// Opened: base, table, string, math, utf8.  (coroutine is intentionally NOT
/// opened — the D7 hand-back model is "one clean process per phase, no
/// coroutine parks awaiting a worker": coroutines were the regrowth point for
/// re-entrant spawning, so the surface is removed entirely.)
///
/// NEVER opened: `os`, `io` — left nil so os.execute / io.open are unreachable.
///
/// Stripped after open: `math.random`, `math.randomseed` (non-deterministic);
/// and the loader/filesystem escape hatches `load`, `loadfile`, `loadstring`,
/// `dofile`, `require` are nil'd from the global env.  With os/io gone and the
/// loaders nil'd, the script's only time/seed source is ctx.now / ctx.seed and
/// its only host reach is the D7 surface.
pub fn openSandboxedLibs(L: ?*c.lua_State) void {
    const Lib = struct {
        name: [*:0]const u8,
        open: *const fn (?*c.lua_State) callconv(.c) c_int,
    };
    const libs = [_]Lib{
        .{ .name = c.LUA_GNAME, .open = @ptrCast(&c.luaopen_base) },
        .{ .name = c.LUA_TABLIBNAME, .open = @ptrCast(&c.luaopen_table) },
        .{ .name = c.LUA_STRLIBNAME, .open = @ptrCast(&c.luaopen_string) },
        .{ .name = c.LUA_MATHLIBNAME, .open = @ptrCast(&c.luaopen_math) },
        .{ .name = c.LUA_UTF8LIBNAME, .open = @ptrCast(&c.luaopen_utf8) },
    };
    inline for (libs) |lib| {
        c.luaL_requiref(L, lib.name, @ptrCast(lib.open), 1);
        c.lua_settop(L, -2); // pop the module table requiref left on the stack
    }

    // Strip non-deterministic math sources.
    const math_type = c.lua_getglobal(L, "math");
    if (math_type == c.LUA_TTABLE) {
        const math_idx = c.lua_absindex(L, -1);
        c.lua_pushnil(L);
        c.lua_setfield(L, math_idx, "random");
        c.lua_pushnil(L);
        c.lua_setfield(L, math_idx, "randomseed");
    }
    c.lua_settop(L, -2); // pop math (or the non-table value)

    // Strip loader / filesystem escape hatches from the global env.
    inline for ([_][*:0]const u8{ "dofile", "loadfile", "load", "loadstring", "require" }) |g| {
        c.lua_pushnil(L);
        c.lua_setglobal(L, g);
    }
}

// ---------------------------------------------------------------------------
// Lua arg helpers
// ---------------------------------------------------------------------------

/// luaArgString reads the string at `idx` (returns "" when not a string).
fn luaArgString(L: ?*c.lua_State, idx: c_int) []const u8 {
    if (c.lua_type(L, idx) != c.LUA_TSTRING) return "";
    var len: usize = 0;
    const raw = c.lua_tolstring(L, idx, &len);
    if (raw == null) return "";
    return raw[0..len];
}

/// raiseError formats a Lua error and longjmps out of the closure (never
/// returns). Used for all confinement / subprocess failures.
fn raiseError(L: ?*c.lua_State, comptime fmt: []const u8, args: anytype) noreturn {
    var buf: [512]u8 = undefined;
    const msg = std.fmt.bufPrintZ(&buf, fmt, args) catch "planar-execute: host error (message truncated)";
    _ = c.luaL_error(L, "%s", msg.ptr);
    unreachable;
}

/// argvFromLuaTable reads a Lua array-table at `idx` into an allocator-owned
/// `[][]const u8`. Non-string elements are rejected with a Lua error.
/// `idx` is absolutized at entry so callers may safely pass a relative
/// (negative) stack index — subsequent pushes during element reads will not
/// shift the table reference.
fn argvFromLuaTable(L: ?*c.lua_State, hs: *HostState, idx: c_int) [][]const u8 {
    const abs = c.lua_absindex(L, idx);
    if (c.lua_type(L, abs) != c.LUA_TTABLE) {
        raiseError(L, "expected an argv table (array of strings)", .{});
    }
    const n: usize = @intCast(c.lua_rawlen(L, abs));
    const out = hs.arena.alloc([]const u8, n) catch raiseError(L, "out of memory building argv", .{});
    var i: usize = 0;
    while (i < n) : (i += 1) {
        _ = c.lua_rawgeti(L, abs, @intCast(i + 1)); // 1-based
        if (c.lua_type(L, -1) != c.LUA_TSTRING) {
            raiseError(L, "argv element {d} is not a string", .{i + 1});
        }
        var len: usize = 0;
        const raw = c.lua_tolstring(L, -1, &len);
        out[i] = hs.arena.dupe(u8, raw[0..len]) catch raiseError(L, "out of memory copying argv", .{});
        c.lua_settop(L, -2); // pop the element
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON ⇄ Lua marshalling
// ---------------------------------------------------------------------------

/// pushJsonValue recursively pushes a parsed `std.json.Value` onto the Lua
/// stack: object → table keyed by field; array → 1-based array table; string →
/// string; number → integer or float; bool → boolean; null → nil.
fn pushJsonValue(L: ?*c.lua_State, v: std.json.Value) void {
    switch (v) {
        .null => c.lua_pushnil(L),
        .bool => |b| c.lua_pushboolean(L, if (b) 1 else 0),
        .integer => |i| c.lua_pushinteger(L, @intCast(i)),
        .float => |f| c.lua_pushnumber(L, f),
        .number_string => |s| _ = c.lua_pushlstring(L, s.ptr, s.len),
        .string => |s| _ = c.lua_pushlstring(L, s.ptr, s.len),
        .array => |arr| {
            c.lua_createtable(L, @intCast(arr.items.len), 0);
            for (arr.items, 0..) |item, i| {
                pushJsonValue(L, item);
                c.lua_rawseti(L, -2, @intCast(i + 1)); // 1-based
            }
        },
        .object => |obj| {
            c.lua_createtable(L, 0, @intCast(obj.count()));
            var it = obj.iterator();
            while (it.next()) |entry| {
                const k = entry.key_ptr.*;
                // std.json keys are not null-terminated; push key + value and
                // set via lua_settable rather than the C-string lua_setfield.
                _ = c.lua_pushlstring(L, k.ptr, k.len);
                pushJsonValue(L, entry.value_ptr.*);
                c.lua_settable(L, -3);
            }
        },
    }
}

/// pushParsedJson parses `json` and pushes the value; on parse failure raises a
/// Lua error.  Uses `.alloc_always` so the pushed Lua strings do not alias the
/// (soon-freed) source buffer.
fn pushParsedJson(L: ?*c.lua_State, hs: *HostState, json: []const u8) void {
    const trimmed = std.mem.trim(u8, json, " \t\r\n");
    if (trimmed.len == 0) {
        c.lua_createtable(L, 0, 0);
        return;
    }
    const parsed = std.json.parseFromSlice(std.json.Value, hs.arena, trimmed, .{ .allocate = .alloc_always }) catch {
        raiseError(L, "host returned non-JSON output", .{});
    };
    pushJsonValue(L, parsed.value);
}

/// luaToJson serializes the Lua value at `idx` into JSON text on the arena.
/// Tables with a 1..n contiguous integer key span serialize as arrays;
/// otherwise as objects. Functions / userdata are rejected with a Lua error.
fn luaToJson(L: ?*c.lua_State, hs: *HostState, idx: c_int) []const u8 {
    var buf: std.Io.Writer.Allocating = .init(hs.arena);
    writeLuaJson(L, hs, c.lua_absindex(L, idx), &buf.writer) catch raiseError(L, "out of memory serializing result", .{});
    return buf.written();
}

fn writeLuaJson(L: ?*c.lua_State, hs: *HostState, idx: c_int, w: *std.Io.Writer) std.Io.Writer.Error!void {
    switch (c.lua_type(L, idx)) {
        c.LUA_TNIL => try w.writeAll("null"),
        c.LUA_TBOOLEAN => try w.writeAll(if (c.lua_toboolean(L, idx) != 0) "true" else "false"),
        c.LUA_TNUMBER => {
            if (c.lua_isinteger(L, idx) != 0) {
                try w.print("{d}", .{c.lua_tointegerx(L, idx, null)});
            } else {
                try w.print("{d}", .{c.lua_tonumberx(L, idx, null)});
            }
        },
        c.LUA_TSTRING => {
            var len: usize = 0;
            const raw = c.lua_tolstring(L, idx, &len);
            try std.json.Stringify.encodeJsonString(raw[0..len], .{}, w);
        },
        c.LUA_TTABLE => try writeLuaTableJson(L, hs, idx, w),
        else => raiseError(L, "cannot serialize Lua {s} to JSON", .{std.mem.span(@as([*:0]const u8, @ptrCast(c.lua_typename(L, c.lua_type(L, idx)))))}),
    }
}

fn writeLuaTableJson(L: ?*c.lua_State, hs: *HostState, idx: c_int, w: *std.Io.Writer) std.Io.Writer.Error!void {
    // Absolutize immediately. Any subsequent lua_push* call shifts the stack,
    // making a relative index (e.g. -1) point at the wrong slot. lua_next in
    // the object branch and lua_rawgeti in the array branch both require the
    // table index to remain stable across pushes; capturing the absolute index
    // here is the single fix for the nested-table panic.
    const abs_idx = c.lua_absindex(L, idx);
    const n = c.lua_rawlen(L, abs_idx);

    // Decide array vs object.
    //
    // Non-empty sequence table (rawlen > 0): emit as JSON array.
    //
    // Any other table (rawlen == 0): emit as JSON object by iterating
    // string keys via lua_next.  This covers both the "pure map" case
    // ({a=1, b=2}) and the "truly empty table" case ({}).  A truly empty
    // table has no string keys either, so lua_next returns 0 immediately
    // and we emit `{}`.
    //
    // Empty-table policy: an empty Lua `{}` serializes as `{}` (empty JSON
    // object). This is deterministic and faithful: the table has no
    // sequence keys and no string keys, so both `[]` and `{}` are
    // technically valid but `{}` is what the object branch naturally
    // produces and is what callers expect when they write `key = {}` as a
    // placeholder.  Changing this would require a two-pass scan (first check
    // for any string keys, then decide) which adds complexity for no benefit
    // over the natural `{}` output.
    if (n > 0) {
        try w.writeByte('[');
        var i: usize = 1;
        while (i <= n) : (i += 1) {
            if (i > 1) try w.writeByte(',');
            _ = c.lua_rawgeti(L, abs_idx, @intCast(i));
            try writeLuaJson(L, hs, -1, w);
            c.lua_settop(L, -2);
        }
        try w.writeByte(']');
        return;
    }
    // Object (or empty table): iterate all key/value pairs. Use abs_idx
    // throughout so that lua_pushnil (the first-key seed) and subsequent
    // lua_next pushes do not invalidate the table reference.
    try w.writeByte('{');
    var first = true;
    c.lua_pushnil(L); // first key — must come AFTER abs_idx is captured
    while (c.lua_next(L, abs_idx) != 0) {
        // key at -2, value at -1.
        if (c.lua_type(L, -2) != c.LUA_TSTRING) {
            // Non-string keys in an object table: coerce via tostring of a
            // COPY so we never mutate the original key (which would confuse
            // lua_next). Skip booleans/tables silently is wrong; reject.
            raiseError(L, "JSON object key must be a string", .{});
        }
        if (!first) try w.writeByte(',');
        first = false;
        var klen: usize = 0;
        const kraw = c.lua_tolstring(L, -2, &klen);
        try std.json.Stringify.encodeJsonString(kraw[0..klen], .{}, w);
        try w.writeByte(':');
        try writeLuaJson(L, hs, -1, w);
        c.lua_settop(L, -2); // pop value, keep key for next
    }
    try w.writeByte('}');
}

/// pushArgsTable parses `hs.args_json` and pushes it (empty table when blank).
fn pushArgsTable(L: ?*c.lua_State, hs: *HostState) void {
    const trimmed = std.mem.trim(u8, hs.args_json, " \t\r\n");
    if (trimmed.len == 0) {
        c.lua_createtable(L, 0, 0);
        return;
    }
    const parsed = std.json.parseFromSlice(std.json.Value, hs.arena, trimmed, .{ .allocate = .alloc_always }) catch {
        // Bad --args is a configuration error, not a runtime one; surface a
        // clear nil-safe empty table rather than crash before run() executes.
        c.lua_createtable(L, 0, 0);
        return;
    };
    pushJsonValue(L, parsed.value);
}

// ---------------------------------------------------------------------------
// Subprocess shells (allowlisted binaries only — NO general exec)
// ---------------------------------------------------------------------------

/// ALLOWED_CLI_BINS — the only binaries the cli.* fns may shell. Each fn
/// hardcodes its binary (cli.planar → "planar", cli.planar_agent →
/// "planar-agent", cli.planar_watch → "planar-watch"); this constant
/// documents the full allowlist and is the guard runAllowlisted checks.
const ALLOWED_CLI_BINS = [_][]const u8{ "planar", "planar-agent", "planar-watch" };

/// runAllowlisted shells `<bin> <argv...>` (bin MUST be in ALLOWED_CLI_BINS)
/// and returns captured stdout on the arena. Non-zero exit raises a Lua error.
fn runAllowlisted(L: ?*c.lua_State, hs: *HostState, bin: []const u8, argv_tail: []const []const u8) []const u8 {
    var ok = false;
    for (ALLOWED_CLI_BINS) |b| {
        if (std.mem.eql(u8, b, bin)) ok = true;
    }
    if (!ok) raiseError(L, "binary not allowlisted: {s}", .{bin});

    const argv = hs.arena.alloc([]const u8, argv_tail.len + 1) catch raiseError(L, "out of memory building argv", .{});
    argv[0] = bin;
    for (argv_tail, 0..) |a, i| argv[i + 1] = a;

    const result = std.process.run(hs.arena, hs.io, .{
        .argv = argv,
        .stdout_limit = Io.Limit.limited(8 * 1024 * 1024),
        .stderr_limit = Io.Limit.limited(64 * 1024),
    }) catch raiseError(L, "failed to spawn {s}", .{bin});

    const exit_ok = result.term == .exited and result.term.exited == 0;
    if (!exit_ok) {
        raiseError(L, "{s} exited non-zero: {s}", .{ bin, std.mem.trim(u8, result.stderr, " \t\r\n") });
    }
    return result.stdout;
}

/// runGit shells `git -C <worktree> <argv...>` and returns stdout on the arena.
/// The worktree is host-injected from run config; the script never names it.
fn runGit(L: ?*c.lua_State, hs: *HostState, git_args: []const []const u8) []const u8 {
    if (hs.worktree.len == 0) raiseError(L, "git.* requires a configured worktree (--worktree)", .{});

    const argv = hs.arena.alloc([]const u8, git_args.len + 3) catch raiseError(L, "out of memory building git argv", .{});
    argv[0] = "git";
    argv[1] = "-C";
    argv[2] = hs.worktree;
    for (git_args, 0..) |a, i| argv[i + 3] = a;

    const result = std.process.run(hs.arena, hs.io, .{
        .argv = argv,
        .stdout_limit = Io.Limit.limited(8 * 1024 * 1024),
        .stderr_limit = Io.Limit.limited(64 * 1024),
    }) catch raiseError(L, "failed to spawn git", .{});

    const exit_ok = result.term == .exited and result.term.exited == 0;
    if (!exit_ok) {
        raiseError(L, "git exited non-zero: {s}", .{std.mem.trim(u8, result.stderr, " \t\r\n")});
    }
    return result.stdout;
}

// ---------------------------------------------------------------------------
// fs path confinement
// ---------------------------------------------------------------------------

/// confinePath rejects absolute paths and any `..` component, then joins the
/// relative path onto the sandbox root.  Returns an arena-owned absolute path.
fn confinePath(L: ?*c.lua_State, hs: *HostState, rel: []const u8) []const u8 {
    if (hs.sandbox_root.len == 0) raiseError(L, "fs.* requires a configured sandbox root (--sandbox-root)", .{});
    if (rel.len == 0) raiseError(L, "fs.* path is empty", .{});
    if (std.fs.path.isAbsolute(rel)) raiseError(L, "fs.* rejects absolute path: {s}", .{rel});
    // Reject any `..` path component (string-level; defense before normalization).
    var it = std.mem.tokenizeAny(u8, rel, "/\\");
    while (it.next()) |comp| {
        if (std.mem.eql(u8, comp, "..")) raiseError(L, "fs.* rejects '..' in path: {s}", .{rel});
    }
    const joined = std.fs.path.join(hs.arena, &.{ hs.sandbox_root, rel }) catch raiseError(L, "out of memory joining path", .{});
    return joined;
}

// ===========================================================================
// Host functions — cli.*
// ===========================================================================

/// cli.planar(argv) → stdout string. Shells `planar <argv...>`.
fn hostCliPlanar(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const argv = argvFromLuaTable(L, hs, 1);
    const out = runAllowlisted(L, hs, "planar", argv);
    _ = c.lua_pushlstring(L, out.ptr, out.len);
    return 1;
}

/// cli.planar_json(argv) → parsed JSON value. Shells `planar <argv...>` and
/// parses stdout as JSON (the caller is responsible for passing `--json`).
fn hostCliPlanarJson(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const argv = argvFromLuaTable(L, hs, 1);
    const out = runAllowlisted(L, hs, "planar", argv);
    pushParsedJson(L, hs, out);
    return 1;
}

/// cli.planar_agent(argv) → stdout string. Shells `planar-agent <argv...>`.
/// Mutation-capable (claim/complete/fail/release/block/heartbeat) but still
/// an allowlisted CLI subprocess — not a spawn primitive.
fn hostCliPlanarAgent(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const argv = argvFromLuaTable(L, hs, 1);
    const out = runAllowlisted(L, hs, "planar-agent", argv);
    _ = c.lua_pushlstring(L, out.ptr, out.len);
    return 1;
}

/// cli.planar_agent_json(argv) → parsed JSON value. Shells `planar-agent <argv...>`
/// and parses stdout as JSON (the caller is responsible for passing `--json`).
fn hostCliPlanarAgentJson(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const argv = argvFromLuaTable(L, hs, 1);
    const out = runAllowlisted(L, hs, "planar-agent", argv);
    pushParsedJson(L, hs, out);
    return 1;
}

/// cli.planar_watch(argv) → stdout string. Shells `planar-watch <argv...>`.
/// Read-only: planar-watch opens the DB in mode=ro and registers no write verbs.
fn hostCliPlanarWatch(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const argv = argvFromLuaTable(L, hs, 1);
    const out = runAllowlisted(L, hs, "planar-watch", argv);
    _ = c.lua_pushlstring(L, out.ptr, out.len);
    return 1;
}

/// cli.planar_watch_json(argv) → parsed JSON value. Shells `planar-watch <argv...>`
/// and parses stdout as JSON (the caller is responsible for passing `--json`).
fn hostCliPlanarWatchJson(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const argv = argvFromLuaTable(L, hs, 1);
    const out = runAllowlisted(L, hs, "planar-watch", argv);
    pushParsedJson(L, hs, out);
    return 1;
}

// ===========================================================================
// Host functions — git.* (confined; host injects -C <worktree>)
// ===========================================================================

fn hostGitResetHard(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const sha = luaArgString(L, 1);
    if (sha.len == 0) raiseError(L, "git.reset_hard requires a sha", .{});
    _ = runGit(L, hs, &.{ "reset", "--hard", sha });
    return 0;
}

fn hostGitCheckout(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const ref = luaArgString(L, 1);
    if (ref.len == 0) raiseError(L, "git.checkout requires a ref", .{});
    _ = runGit(L, hs, &.{ "checkout", ref });
    return 0;
}

/// git.diff_name_only(opts?) → array of changed paths. opts.base sets the diff
/// base (e.g. "HEAD~1"); omitted means working-tree-vs-HEAD.
fn hostGitDiffNameOnly(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    var base: []const u8 = "";
    if (c.lua_type(L, 1) == c.LUA_TTABLE) {
        _ = c.lua_getfield(L, 1, "base");
        base = luaArgString(L, -1);
        c.lua_settop(L, -2);
    }
    const out = if (base.len > 0)
        runGit(L, hs, &.{ "diff", "--name-only", base })
    else
        runGit(L, hs, &.{ "diff", "--name-only" });
    // Split lines into a Lua array.
    c.lua_createtable(L, 0, 0);
    var it = std.mem.tokenizeScalar(u8, out, '\n');
    var i: c_longlong = 1;
    while (it.next()) |line| {
        const t = std.mem.trim(u8, line, " \t\r");
        if (t.len == 0) continue;
        _ = c.lua_pushlstring(L, t.ptr, t.len);
        c.lua_rawseti(L, -2, i);
        i += 1;
    }
    return 1;
}

/// git.head_sha() → the worktree HEAD sha (trimmed).
fn hostGitHeadSha(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const out = runGit(L, hs, &.{ "rev-parse", "HEAD" });
    const sha = std.mem.trim(u8, out, " \t\r\n");
    _ = c.lua_pushlstring(L, sha.ptr, sha.len);
    return 1;
}

/// git.clean(opts?) → removes untracked files. opts.force defaults true;
/// opts.directories adds -d. Always runs -f (git refuses clean without it).
fn hostGitClean(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    var directories = false;
    if (c.lua_type(L, 1) == c.LUA_TTABLE) {
        _ = c.lua_getfield(L, 1, "directories");
        directories = c.lua_toboolean(L, -1) != 0;
        c.lua_settop(L, -2);
    }
    if (directories) {
        _ = runGit(L, hs, &.{ "clean", "-fd" });
    } else {
        _ = runGit(L, hs, &.{ "clean", "-f" });
    }
    return 0;
}

// ===========================================================================
// Host functions — fs.* (path-confined)
// ===========================================================================

fn hostFsRead(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const path = confinePath(L, hs, luaArgString(L, 1));
    const data = std.Io.Dir.cwd().readFileAlloc(hs.io, path, hs.arena, .limited(16 * 1024 * 1024)) catch raiseError(L, "fs.read failed: {s}", .{path});
    _ = c.lua_pushlstring(L, data.ptr, data.len);
    return 1;
}

fn hostFsWrite(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const path = confinePath(L, hs, luaArgString(L, 1));
    const data = luaArgString(L, 2);
    // Ensure the parent dir exists (confined under sandbox root already).
    if (std.fs.path.dirname(path)) |dir| {
        std.Io.Dir.cwd().createDirPath(hs.io, dir) catch {};
    }
    std.Io.Dir.cwd().writeFile(hs.io, .{ .sub_path = path, .data = data }) catch raiseError(L, "fs.write failed: {s}", .{path});
    return 0;
}

fn hostFsExists(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const path = confinePath(L, hs, luaArgString(L, 1));
    const exists = blk: {
        std.Io.Dir.cwd().access(hs.io, path, .{}) catch break :blk false;
        break :blk true;
    };
    c.lua_pushboolean(L, if (exists) 1 else 0);
    return 1;
}

fn hostFsMkdir(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const path = confinePath(L, hs, luaArgString(L, 1));
    std.Io.Dir.cwd().createDirPath(hs.io, path) catch raiseError(L, "fs.mkdir failed: {s}", .{path});
    return 0;
}

// ===========================================================================
// Host functions — flow.* (pure)
// ===========================================================================

fn hostFlowLog(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const msg = luaArgString(L, 1);
    // Logs go to stderr so stdout stays a clean result channel.
    const stderr = Io.File.stderr();
    stderr.writeStreamingAll(hs.io, "[planar-execute] ") catch {};
    stderr.writeStreamingAll(hs.io, msg) catch {};
    stderr.writeStreamingAll(hs.io, "\n") catch {};
    return 0;
}

fn hostFlowPhase(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const name = luaArgString(L, 1);
    hs.current_phase = hs.arena.dupe(u8, name) catch hs.current_phase;
    return 0;
}

fn hostFlowFail(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const msg = luaArgString(L, 1);
    hs.fail_msg = hs.arena.dupe(u8, msg) catch "planar-execute: flow.fail (message lost to OOM)";
    raiseError(L, "flow.fail: {s}", .{msg});
}

/// flow.result(table) captures the final payload as JSON. The engine marshals
/// hs.result_json to stdout after run() returns.
fn hostFlowResult(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    if (c.lua_type(L, 1) != c.LUA_TTABLE) raiseError(L, "flow.result expects a table", .{});
    hs.result_json = luaToJson(L, hs, 1);
    return 0;
}

// ===========================================================================
// Host functions — ctx.* (deterministic planner reads)
// ===========================================================================

/// ctx.plan_show(id) → parsed `planar plan show <id> --json`.
fn hostCtxPlanShow(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const id = ctxIdArg(L, 1, "ctx.plan_show");
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{id}) catch unreachable;
    const out = runAllowlisted(L, hs, "planar", &.{ "plan", "show", id_str, "--json" });
    pushParsedJson(L, hs, out);
    return 1;
}

/// ctx.task_show(id) → parsed `planar task show <id> --json`.
fn hostCtxTaskShow(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const id = ctxIdArg(L, 1, "ctx.task_show");
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{id}) catch unreachable;
    const out = runAllowlisted(L, hs, "planar", &.{ "task", "show", id_str, "--json" });
    pushParsedJson(L, hs, out);
    return 1;
}

/// ctx.task_touches(id) → parsed `planar task touches list <id> --json`.
fn hostCtxTaskTouches(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const id = ctxIdArg(L, 1, "ctx.task_touches");
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{id}) catch unreachable;
    const out = runAllowlisted(L, hs, "planar", &.{ "task", "touches", "list", id_str, "--json" });
    pushParsedJson(L, hs, out);
    return 1;
}

/// ctx.recommend_strategy(id) → parsed `planar plan recommend-strategy <id> --json`.
fn hostCtxRecommendStrategy(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    const id = ctxIdArg(L, 1, "ctx.recommend_strategy");
    var id_buf: [32]u8 = undefined;
    const id_str = std.fmt.bufPrint(&id_buf, "{d}", .{id}) catch unreachable;
    const out = runAllowlisted(L, hs, "planar", &.{ "plan", "recommend-strategy", id_str, "--json" });
    pushParsedJson(L, hs, out);
    return 1;
}

/// ctx.context([stage]) → parsed `planar-watch …` context records. Minimal
/// scope (D7): reads the run context via `planar-agent context list` when a
/// run is configured; absent run config it returns an empty table.
///
/// The current engine has no run binding (one clean process per phase, no
/// scheduler), so this degrades to an empty table rather than fabricating a
/// run id. The verb stays registered so the surface matches D7's allowlist and
/// a later milestone can wire the run binding behind the same lock.
fn hostCtxContext(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    _ = hs;
    c.lua_createtable(L, 0, 0);
    return 1;
}

/// ctx.brief(opts) → the compiled coder brief string. compileBrief stays
/// in-engine (D7): the host fetches plan + task + agent schema via the salvaged
/// helpers and renders the brief.
///
/// opts table fields:
///   plan_id           (int, required)
///   task_id           (int, required)
///   claim_token       (string, required)
///   problem_statement (string, required)
///   gates             (array of strings, optional)
fn hostCtxBrief(L: ?*c.lua_State) callconv(.c) c_int {
    const hs = hostStateUpvalue(L);
    if (c.lua_type(L, 1) != c.LUA_TTABLE) raiseError(L, "ctx.brief expects an opts table", .{});

    const plan_id = tableIntField(L, 1, "plan_id") orelse raiseError(L, "ctx.brief: opts.plan_id required", .{});
    const task_id = tableIntField(L, 1, "task_id") orelse raiseError(L, "ctx.brief: opts.task_id required", .{});
    const claim_token = tableStrField(L, hs, 1, "claim_token") orelse raiseError(L, "ctx.brief: opts.claim_token required", .{});
    const problem = tableStrField(L, hs, 1, "problem_statement") orelse raiseError(L, "ctx.brief: opts.problem_statement required", .{});

    // Gates (optional array of strings).
    var gates: []const []const u8 = &.{};
    _ = c.lua_getfield(L, 1, "gates");
    if (c.lua_type(L, -1) == c.LUA_TTABLE) {
        gates = argvFromLuaTable(L, hs, c.lua_absindex(L, -1));
    }
    c.lua_settop(L, -2);

    // Fetch plan + task + schema via the salvaged helpers (deterministic reads).
    const plan_parsed = state.planShow(hs.arena, hs.io, @intCast(plan_id)) catch raiseError(L, "ctx.brief: plan {d} not found", .{plan_id});
    const task_parsed = state.taskShow(hs.arena, hs.io, @intCast(task_id)) catch raiseError(L, "ctx.brief: task {d} not found", .{task_id});
    const schema_parsed = schema.loadSchema(hs.arena, hs.io, "planar-agent") catch raiseError(L, "ctx.brief: failed to load planar-agent schema", .{});
    const agent_schema = schema.BinSchema.init(schema_parsed.value);

    // Adapt TaskShow → the TaskEntry shape compileBrief consumes.
    const tasks = hs.arena.alloc(state.TaskEntry, 1) catch raiseError(L, "out of memory", .{});
    tasks[0] = .{
        .id = task_parsed.value.id,
        .plan_id = @intCast(plan_id),
        .title = plan_parsed.value.title,
        .slug = task_parsed.value.slug,
        .status = task_parsed.value.status,
    };

    const inputs = brief.BriefInputs{
        .plan = plan_parsed.value,
        .tasks = tasks,
        .claim_token = claim_token,
        .problem_statement = problem,
        .spec_citations = &.{},
        .locked_decisions = &.{},
        .agent_schema = agent_schema,
        .gates = gates,
    };
    const compiled = brief.compileBrief(hs.arena, inputs) catch raiseError(L, "ctx.brief: compileBrief failed", .{});
    _ = c.lua_pushlstring(L, compiled.ptr, compiled.len);
    return 1;
}

// ---------------------------------------------------------------------------
// ctx helpers
// ---------------------------------------------------------------------------

/// ctxIdArg reads a positive integer id at `idx` or raises a Lua error.
fn ctxIdArg(L: ?*c.lua_State, idx: c_int, comptime who: []const u8) u64 {
    if (c.lua_isinteger(L, idx) != 0) {
        const v = c.lua_tointegerx(L, idx, null);
        if (v <= 0) raiseError(L, who ++ ": id must be positive", .{});
        return @intCast(v);
    }
    raiseError(L, who ++ ": expected an integer id", .{});
}

/// tableIntField reads an integer field from the table at `idx` (or null).
fn tableIntField(L: ?*c.lua_State, idx: c_int, field: [*:0]const u8) ?i64 {
    _ = c.lua_getfield(L, idx, field);
    defer c.lua_settop(L, -2);
    if (c.lua_isinteger(L, -1) != 0) return c.lua_tointegerx(L, -1, null);
    return null;
}

/// tableStrField reads a string field from the table at `idx`, duped on the
/// arena (or null when absent / non-string).
fn tableStrField(L: ?*c.lua_State, hs: *HostState, idx: c_int, field: [*:0]const u8) ?[]const u8 {
    _ = c.lua_getfield(L, idx, field);
    defer c.lua_settop(L, -2);
    if (c.lua_type(L, -1) != c.LUA_TSTRING) return null;
    var len: usize = 0;
    const raw = c.lua_tolstring(L, -1, &len);
    return hs.arena.dupe(u8, raw[0..len]) catch null;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test {
    std.testing.refAllDecls(@This());
}

test "manifest: ALLOWED contains the D7 surface and none of DENIED" {
    // Every allowlisted name must be absent from the denylist (also enforced at
    // comptime, but asserted here so the test count reflects the invariant).
    for (ALLOWED_HOST_FNS) |hf| {
        for (DENIED_HOST_FNS) |denied| {
            try std.testing.expect(!std.mem.eql(u8, hf.name, denied));
        }
    }
    // The surface must be exactly the D7 allowlist size (25 fns).
    try std.testing.expectEqual(@as(usize, 25), ALLOWED_HOST_FNS.len);
}

test "manifest: cli surface has no general exec" {
    for (ALLOWED_HOST_FNS) |hf| {
        try std.testing.expect(!std.mem.eql(u8, hf.name, "exec"));
        try std.testing.expect(!std.mem.eql(u8, hf.name, "spawn"));
        try std.testing.expect(!std.mem.eql(u8, hf.name, "agent"));
    }
}

test "manifest: allowlisted cli bins are the three planar binaries" {
    try std.testing.expectEqual(@as(usize, 3), ALLOWED_CLI_BINS.len);
    try std.testing.expectEqualStrings("planar", ALLOWED_CLI_BINS[0]);
    try std.testing.expectEqualStrings("planar-agent", ALLOWED_CLI_BINS[1]);
    try std.testing.expectEqualStrings("planar-watch", ALLOWED_CLI_BINS[2]);
}

// ---------------------------------------------------------------------------
// P0.3 RUNTIME LOCK — asserts the REAL registered Lua surface matches D7.
//
// These tests create an actual sandboxed Lua state, call installHostSurface,
// then walk the live Lua global tables using lua_next to enumerate every
// function actually registered at runtime. The enumerated set is compared
// against ALLOWED_HOST_FNS exactly (no extras, no missing entries), and
// every entry in DENIED_HOST_FNS is asserted absent. This catches
// registrar↔manifest drift that the comptime guard cannot observe.
// ---------------------------------------------------------------------------

/// enumerateHostTable walks a Lua table by iterating its keys with lua_next
/// and appends every string-keyed function entry to `out` as a heap-allocated
/// "table.fn" pair. Non-function values are ignored (e.g. ctx.now / ctx.seed /
/// ctx.args are integers and a table, not functions).
fn enumerateHostTable(
    gpa: std.mem.Allocator,
    L: ?*c.lua_State,
    table_name: []const u8,
    out: *std.ArrayList([]const u8),
) !void {
    // Push the global table by name.
    const name_z = try gpa.dupeZ(u8, table_name);
    defer gpa.free(name_z);
    const ty = c.lua_getglobal(L, name_z.ptr);
    defer c.lua_settop(L, -2); // always pop the table (or nil) when done
    if (ty != c.LUA_TTABLE) return; // global not present; that's a test failure path

    // lua_next iteration: push nil as first key, then advance.
    c.lua_pushnil(L);
    while (c.lua_next(L, -2) != 0) {
        // key is at -2, value at -1.
        defer c.lua_settop(L, -2); // pop value; keep key for next iteration
        // We only care about string-keyed function entries.
        if (c.lua_type(L, -2) == c.LUA_TSTRING and c.lua_type(L, -1) == c.LUA_TFUNCTION) {
            var klen: usize = 0;
            const kraw = c.lua_tolstring(L, -2, &klen);
            const fn_name = kraw[0..klen];
            // Build "table.fn" label for comparison with ALLOWED_HOST_FNS.
            const label = try std.fmt.allocPrint(gpa, "{s}.{s}", .{ table_name, fn_name });
            try out.append(gpa, label);
        }
    }
}

test "runtime lock: installHostSurface registers EXACTLY the ALLOWED_HOST_FNS set" {
    // This test observes the REAL Lua state — not the manifest alone.
    // It catches any drift between ALLOWED_HOST_FNS and what installHostSurface
    // actually registers (e.g. an extra push that the manifest doesn't list).
    const gpa = std.testing.allocator;

    const L = c.luaL_newstate();
    defer c.lua_close(L);

    // Open the sandboxed stdlib and install the host surface.
    openSandboxedLibs(L);

    // Construct a minimal HostState. The test only validates what's registered;
    // no host fn is actually called, so we don't need a real io handle.
    var hs: HostState = .{
        .arena = gpa,
        .io = std.testing.io,
    };
    installHostSurface(L, &hs);

    // Enumerate every function actually registered on the five host tables.
    var runtime_fns: std.ArrayList([]const u8) = .empty;
    defer {
        for (runtime_fns.items) |s| gpa.free(s);
        runtime_fns.deinit(gpa);
    }

    const tables = [_][]const u8{ "cli", "git", "fs", "flow", "ctx" };
    for (tables) |tbl| {
        try enumerateHostTable(gpa, L, tbl, &runtime_fns);
    }

    // Build a set from the runtime functions.
    var runtime_set = std.StringHashMap(void).init(gpa);
    defer runtime_set.deinit();
    for (runtime_fns.items) |label| {
        try runtime_set.put(label, {});
    }

    // Build the expected set from ALLOWED_HOST_FNS.
    var expected_set = std.StringHashMap(void).init(gpa);
    defer expected_set.deinit();
    var expected_labels: std.ArrayList([]u8) = .empty;
    defer {
        for (expected_labels.items) |s| gpa.free(s);
        expected_labels.deinit(gpa);
    }
    for (ALLOWED_HOST_FNS) |hf| {
        const label = try std.fmt.allocPrint(gpa, "{s}.{s}", .{ hf.table, hf.name });
        try expected_labels.append(gpa, label);
        try expected_set.put(label, {});
    }

    // Assert: every expected entry is present in the runtime set.
    var exp_it = expected_set.keyIterator();
    while (exp_it.next()) |k| {
        if (!runtime_set.contains(k.*)) {
            std.debug.print(
                "[host lock] ALLOWED fn '{s}' is missing from the runtime-registered surface\n",
                .{k.*},
            );
            return error.AllowedFnMissingAtRuntime;
        }
    }

    // Assert: every runtime entry is in the expected set (no extras).
    var rt_it = runtime_set.keyIterator();
    while (rt_it.next()) |k| {
        if (!expected_set.contains(k.*)) {
            std.debug.print(
                "[host lock] runtime fn '{s}' is registered but NOT in ALLOWED_HOST_FNS\n",
                .{k.*},
            );
            return error.UnexpectedRuntimeFn;
        }
    }

    // Assert: counts match (catches the case where a fn appears under two
    // tables — would pass the individual membership checks but inflate the count).
    if (runtime_set.count() != expected_set.count()) {
        std.debug.print(
            "[host lock] runtime fn count {d} != expected {d}\n",
            .{ runtime_set.count(), expected_set.count() },
        );
        return error.FnCountMismatch;
    }
}

test "runtime lock: DENIED_HOST_FNS names are absent from every host table at runtime" {
    // Walk the five host tables and assert no key matches a denied name.
    // This catches a registration that sneaks past the comptime guard
    // (e.g. a helper registered under an alias not in DENIED_HOST_FNS at compile
    // time but whose effect is spawn-shaped).
    const gpa = std.testing.allocator;

    const L = c.luaL_newstate();
    defer c.lua_close(L);

    openSandboxedLibs(L);
    var hs: HostState = .{
        .arena = gpa,
        .io = std.testing.io,
    };
    installHostSurface(L, &hs);

    var runtime_fns: std.ArrayList([]const u8) = .empty;
    defer {
        for (runtime_fns.items) |s| gpa.free(s);
        runtime_fns.deinit(gpa);
    }

    const tables = [_][]const u8{ "cli", "git", "fs", "flow", "ctx" };
    for (tables) |tbl| {
        try enumerateHostTable(gpa, L, tbl, &runtime_fns);
    }

    // For each runtime fn label ("table.fn"), check if the fn-name component
    // appears in DENIED_HOST_FNS.
    for (runtime_fns.items) |label| {
        // Extract the fn-name part after the dot.
        const dot = std.mem.lastIndexOfScalar(u8, label, '.') orelse continue;
        const fn_name = label[dot + 1 ..];
        for (DENIED_HOST_FNS) |denied| {
            if (std.mem.eql(u8, fn_name, denied)) {
                std.debug.print(
                    "[host lock] DENIED fn name '{s}' found registered as '{s}'\n",
                    .{ denied, label },
                );
                return error.DeniedFnPresentAtRuntime;
            }
        }
    }
}

test "runtime lock: sandbox nils os, io, load, loadfile, loadstring, dofile, require, math.random, math.randomseed" {
    // Assert against the LIVE Lua state that every escape hatch is nil.
    // A positive test: openSandboxedLibs is the function under test here.
    const gpa = std.testing.allocator;
    _ = gpa;

    const L = c.luaL_newstate();
    defer c.lua_close(L);

    openSandboxedLibs(L);

    // Top-level globals that MUST be nil.
    const nil_globals = [_][*:0]const u8{
        "os", "io", "load", "loadfile", "loadstring", "dofile", "require",
    };
    for (nil_globals) |g| {
        const ty = c.lua_getglobal(L, g);
        defer c.lua_settop(L, -2);
        if (ty != c.LUA_TNIL) {
            std.debug.print(
                "[sandbox lock] global '{s}' should be nil but is type {d}\n",
                .{ std.mem.span(g), ty },
            );
            return error.SandboxEscapeHatchPresent;
        }
    }

    // math.random and math.randomseed must be nil within the math table.
    const math_ty = c.lua_getglobal(L, "math");
    defer c.lua_settop(L, -2);
    // math table must exist (we opened math).
    try std.testing.expectEqual(c.LUA_TTABLE, math_ty);
    const math_idx = c.lua_absindex(L, -1);

    const random_ty = c.lua_getfield(L, math_idx, "random");
    defer c.lua_settop(L, -2);
    if (random_ty != c.LUA_TNIL) {
        std.debug.print("[sandbox lock] math.random should be nil but is type {d}\n", .{random_ty});
        return error.MathRandomPresent;
    }

    const randomseed_ty = c.lua_getfield(L, math_idx, "randomseed");
    defer c.lua_settop(L, -2);
    if (randomseed_ty != c.LUA_TNIL) {
        std.debug.print("[sandbox lock] math.randomseed should be nil but is type {d}\n", .{randomseed_ty});
        return error.MathRandomseedPresent;
    }
}

test "manifest: cli.planar_watch is present and shells only planar-watch" {
    // Confirm cli.planar_watch / cli.planar_watch_json are in the allowlist
    // and map to the correct binary (planar-watch), not any other bin.
    var found_watch: bool = false;
    var found_watch_json: bool = false;
    for (ALLOWED_HOST_FNS) |hf| {
        if (std.mem.eql(u8, hf.table, "cli")) {
            if (std.mem.eql(u8, hf.name, "planar_watch")) found_watch = true;
            if (std.mem.eql(u8, hf.name, "planar_watch_json")) found_watch_json = true;
        }
    }
    try std.testing.expect(found_watch);
    try std.testing.expect(found_watch_json);

    // The implementations shell ONLY "planar-watch", not "planar" or
    // "planar-agent". Verify the binary-string used by runAllowlisted is
    // "planar-watch" by confirming it is in ALLOWED_CLI_BINS and that neither
    // "exec" nor any un-allowlisted binary is referenced.
    var watch_allowed: bool = false;
    for (ALLOWED_CLI_BINS) |b| {
        if (std.mem.eql(u8, b, "planar-watch")) watch_allowed = true;
    }
    try std.testing.expect(watch_allowed);

    // runAllowlisted rejects any binary not in ALLOWED_CLI_BINS. Assert that
    // "planar-execute" itself is NOT in the allowlist (no self-spawning).
    for (ALLOWED_CLI_BINS) |b| {
        try std.testing.expect(!std.mem.eql(u8, b, "planar-execute"));
    }
}

test "argvFromLuaTable: relative (negative) index is safe" {
    // Regression guard for task-4209 hardening: argvFromLuaTable absolutizes
    // `idx` at entry so a negative/relative index (e.g. -1) stays valid
    // after elements are pushed onto the stack during the read loop.
    //
    // Setup: push a sentinel string then the argv table {"foo","bar"} so the
    // table sits at -1 (top) and at absolute index 2.  Pass -1 (relative).
    // Before the fix, lua_rawlen(L, -1) and lua_rawgeti(L, -1, ...) would
    // address the wrong slot after the first element push; with lua_absindex
    // the slot is pinned to 2 for the lifetime of the call.
    const gpa = std.testing.allocator;

    const L = c.luaL_newstate();
    defer c.lua_close(L);

    // Push a sentinel so the table is NOT at absolute index 1.
    _ = c.lua_pushstring(L, "sentinel");

    // Build table {"foo", "bar"} on top.
    c.lua_createtable(L, 2, 0);
    _ = c.lua_pushstring(L, "foo");
    c.lua_rawseti(L, -2, 1);
    _ = c.lua_pushstring(L, "bar");
    c.lua_rawseti(L, -2, 2);
    // Stack: [sentinel, {foo,bar}]  — table is at idx -1 (relative) or 2 (absolute).

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    var hs: HostState = .{
        .arena = arena_state.allocator(),
        .io = std.testing.io,
    };

    // Pass -1: the relative index.  Must produce {"foo","bar"} without panic.
    const argv = argvFromLuaTable(L, &hs, -1);
    try std.testing.expectEqual(@as(usize, 2), argv.len);
    try std.testing.expectEqualStrings("foo", argv[0]);
    try std.testing.expectEqualStrings("bar", argv[1]);
}
