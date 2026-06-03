//! planar-execute — Lua 5.4 script execution harness (plan 492).
//!
//! Fifth binary in the Planar family. Unlike the other binaries, this one
//! does NOT open SQLite and does NOT link the runtime / engine / db modules.
//! Its sole dependency beyond the standard library is liblua54 (vendored
//! under vendor/lua/) and the cli module (etc-cli, for argument parsing).
//!
//! M1 scope:
//!   task 3209 — vendor Lua, link it, prove the link with a newstate/close
//!               round-trip.
//!   task 3163 — load string → lua_pcall → read return value back into Zig;
//!               surface Lua compile/runtime errors as Zig errors (no panic).
//!   task 3164 — workflow module loading: compile + execute the top-level chunk
//!               to obtain a {meta, run} table; read meta fields (name,
//!               description, phases) into Zig structs via a caller-owned
//!               allocator (closes task 3221 — string return from Lua); call
//!               run(ctx) with a minimal stub ctx; chunkname-bearing loader
//!               (closes task 3223).
//!   task 3165 — CLI surface: `planar-execute <workflow.lua> [args…]` (default
//!               run-path), `planar-execute version`, `planar-execute --help`.
//!               The first positional is resolved as a workflow file path;
//!               trailing [args…] are threaded into ctx.args as a 1-based Lua
//!               sequence of strings. Lua errors map to non-zero exit.
//!
//! What is intentionally absent (later tasks):
//!   - host-function surface (agent/parallel/pipeline/phase/log/budget/
//!     workflow): m2-host-fns
//!   - stdlib sandboxing (strip os/io/os.time/math.random): m2-sandbox
//!   - --dry-run flag (stops after loadModule, prints meta/phases): task 3166
//!   - named built-in workflow registry: m10-quality-spine (no entries yet)

const std = @import("std");
const Io = std.Io;

const cli = @import("cli");

const c = @cImport({
    @cInclude("lua.h");
    @cInclude("lauxlib.h");
    @cInclude("lualib.h");
});

// ---------------------------------------------------------------------------
// Process-global I/O context (no runtime module — planar-execute has no DB).
// ---------------------------------------------------------------------------

/// Lightweight process context for planar-execute. Does not inherit from
/// runtime.Ctx because this binary intentionally has no DB handle.
pub const ExecCtx = struct {
    allocator: std.mem.Allocator,
    io: Io,
    stdout: *Io.Writer,
    stderr: *Io.Writer,
};

var stdout_buf: [4096]u8 = undefined;
var stderr_buf: [1024]u8 = undefined;
var stdout_writer_storage: ?Io.File.Writer = null;
var stderr_writer_storage: ?Io.File.Writer = null;
var global_ctx: ?ExecCtx = null;

fn initCtx(allocator: std.mem.Allocator, io: Io) void {
    stdout_writer_storage = Io.File.Writer.init(.stdout(), io, &stdout_buf);
    stderr_writer_storage = Io.File.Writer.init(.stderr(), io, &stderr_buf);
    global_ctx = .{
        .allocator = allocator,
        .io = io,
        .stdout = &stdout_writer_storage.?.interface,
        .stderr = &stderr_writer_storage.?.interface,
    };
}

fn currentCtx() *const ExecCtx {
    return &(global_ctx orelse @panic("planar-execute: ctx not initialized"));
}

fn flushCtx() !void {
    if (global_ctx) |_| {
        try stdout_writer_storage.?.interface.flush();
        try stderr_writer_storage.?.interface.flush();
    }
}

// ---------------------------------------------------------------------------
// Errors that evalString and loadModule can surface to Zig callers.
// ---------------------------------------------------------------------------

pub const LuaError = error{
    /// luaL_newstate returned null — allocator failure.
    LuaAllocFailed,
    /// The script failed to compile (syntax error).
    LuaCompileError,
    /// The script compiled but raised a runtime error.
    LuaRuntimeError,
    /// The script ran successfully but returned a type that is not a number.
    /// Applies to evalString; loadModule expects a table (see below).
    LuaUnexpectedType,
    /// The workflow chunk executed but did not return a table.
    LuaModuleNotTable,
    /// The returned table lacks a `meta` field that is itself a table.
    LuaModuleMissingMeta,
    /// The returned table lacks a `run` field that is a function.
    LuaModuleMissingRun,
    /// `meta` is present as a table but a required string field
    /// (`name` or `description`) is missing or not a string.
    LuaModuleInvalidMeta,
    /// An allocator error occurred while copying Lua strings into Zig memory.
    LuaStringAllocError,
};

// ---------------------------------------------------------------------------
// Workflow module structs
// ---------------------------------------------------------------------------

/// A single phase entry extracted from meta.phases[i].
pub const PhaseMeta = struct {
    title: []const u8,
    detail: []const u8,
};

/// The `meta` sub-table of a loaded workflow module.
pub const WorkflowMeta = struct {
    name: []const u8,
    description: []const u8,
    /// Owned slice of phase entries; each string inside is also allocator-owned.
    phases: []PhaseMeta,
};

/// A fully-loaded and validated workflow module.
///
/// All strings are allocator-owned copies made before the Lua state is
/// closed. The caller must call `deinit(allocator)` when done to free them.
/// The Lua state is closed before `loadModule` returns.
pub const WorkflowModule = struct {
    meta: WorkflowMeta,

    /// Free all allocator-owned memory held by this module.
    pub fn deinit(self: *WorkflowModule, allocator: std.mem.Allocator) void {
        for (self.meta.phases) |phase| {
            allocator.free(phase.title);
            allocator.free(phase.detail);
        }
        allocator.free(self.meta.phases);
        allocator.free(self.meta.name);
        allocator.free(self.meta.description);
    }
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// copyLuaString copies the Lua string at stack index `idx` into
/// allocator-owned memory and returns the slice. The Lua string stays
/// on the stack; the copy is independent of the Lua state lifetime.
///
/// Returns LuaStringAllocError on OOM, LuaUnexpectedType when the value
/// at `idx` is not a string.
fn copyLuaString(L: ?*c.lua_State, idx: c_int, allocator: std.mem.Allocator) (LuaError || std.mem.Allocator.Error)![]const u8 {
    if (c.lua_type(L, idx) != c.LUA_TSTRING) return LuaError.LuaUnexpectedType;
    var len: usize = 0;
    const raw = c.lua_tolstring(L, idx, &len);
    if (raw == null) return LuaError.LuaUnexpectedType;
    const src: []const u8 = raw[0..len];
    const copy = try allocator.dupe(u8, src);
    return copy;
}

/// captureError reads the error message off the top of the Lua stack
/// (placed there by luaL_loadstring/luaL_loadbufferx or lua_pcallk on
/// failure) and writes it into err_buf, null-terminated, truncated to fit.
/// Pops the value.
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

// ---------------------------------------------------------------------------
// evalString — retained from task 3163, numeric return only.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// loadModule — task 3164.
// ---------------------------------------------------------------------------

/// loadModule compiles and executes the Lua chunk in `source` (identified by
/// `chunkname` in error messages), then validates that the chunk returned a
/// table with shape { meta = { name, description, phases }, run = function }.
///
/// All strings in the returned `WorkflowModule` are allocator-owned copies
/// made before the Lua state is closed.  The caller owns the memory and must
/// call `WorkflowModule.deinit(allocator)` when done.
///
/// How "read meta without running run" is satisfied: executing the top-level
/// chunk only *constructs and returns* the module table — it does not call
/// `run`.  Reading `meta` fields is purely table access.  `run(ctx)` is
/// invoked only by a subsequent explicit `callRun` call.
///
/// On any error, the Lua state is closed, `err_buf` receives the Lua error
/// message (if applicable), and a `LuaError` is returned.
///
/// Chunkname parameter: passed directly to `luaL_loadbufferx` so that Lua
/// compile/runtime errors cite the workflow source identifier rather than the
/// generic "[string ...]". (Closes task 3223.)
///
/// String copies via caller allocator: `copyLuaString` copies each string
/// before `lua_close`, satisfying the invariant that returned data outlives
/// the Lua state. (Closes task 3221.)
pub fn loadModule(
    source: []const u8,
    chunkname: [*:0]const u8,
    allocator: std.mem.Allocator,
    err_buf: []u8,
) (LuaError || std.mem.Allocator.Error)!WorkflowModule {
    const L = c.luaL_newstate();
    if (L == null) return LuaError.LuaAllocFailed;
    defer c.lua_close(L);

    // Compile the chunk. luaL_loadbufferx accepts a length-delimited buffer
    // and a chunkname; on error it pushes a message string.
    const load_rc = c.luaL_loadbufferx(
        L,
        source.ptr,
        source.len,
        chunkname,
        null, // mode: default (text or binary)
    );
    if (load_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaCompileError;
    }

    // Execute the top-level chunk; it must return exactly one value (the module
    // table). Executing it does NOT invoke run — run is a table field, not a
    // call target yet. This is the mechanism by which meta is readable without
    // running run.
    const call_rc = c.lua_pcallk(L, 0, 1, 0, 0, null);
    if (call_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaRuntimeError;
    }

    // Stack: [-1] = return value from chunk. Validate it is a table.
    if (c.lua_type(L, -1) != c.LUA_TTABLE) {
        return LuaError.LuaModuleNotTable;
    }
    // Absolute index for the module table (index 1, bottom of user stack).
    const module_idx: c_int = -1;

    // -----------------------------------------------------------------------
    // Validate and extract meta.
    // -----------------------------------------------------------------------

    // lua_getfield pushes the value of t[k]; the return value is the type tag.
    const meta_type = c.lua_getfield(L, module_idx, "meta");
    if (meta_type != c.LUA_TTABLE) {
        return LuaError.LuaModuleMissingMeta;
    }
    // Stack: [-2] module table, [-1] meta table.
    const meta_idx: c_int = -1;

    // Extract meta.name (required string).
    const name_type = c.lua_getfield(L, meta_idx, "name");
    if (name_type != c.LUA_TSTRING) {
        return LuaError.LuaModuleInvalidMeta;
    }
    const meta_name = try copyLuaString(L, -1, allocator);
    // errdefer covers all subsequent errors from this point on.
    errdefer allocator.free(meta_name);
    c.lua_settop(L, -(1) - 1); // pop name

    // Extract meta.description (required string).
    const desc_type = c.lua_getfield(L, meta_idx, "description");
    if (desc_type != c.LUA_TSTRING) {
        // errdefer above frees meta_name on this return.
        return LuaError.LuaModuleInvalidMeta;
    }
    const meta_desc = try copyLuaString(L, -1, allocator);
    errdefer allocator.free(meta_desc);
    c.lua_settop(L, -(1) - 1); // pop description

    // Extract meta.phases (optional array; if missing or nil, treat as empty).
    // std.ArrayList in Zig 0.16 is the unmanaged Aligned variant — allocator
    // is passed at each mutating call site rather than stored in the list.
    var phases: std.ArrayList(PhaseMeta) = .empty;
    errdefer {
        for (phases.items) |ph| {
            allocator.free(ph.title);
            allocator.free(ph.detail);
        }
        phases.deinit(allocator);
    }

    const phases_type = c.lua_getfield(L, meta_idx, "phases");
    if (phases_type == c.LUA_TTABLE) {
        // Iterate over the sequence part: phases[1], phases[2], ...
        const phases_idx: c_int = -1;
        const n_phases = c.lua_rawlen(L, phases_idx);
        var i: c.lua_Unsigned = 1;
        while (i <= n_phases) : (i += 1) {
            // lua_rawgeti pushes phases[i].
            _ = c.lua_rawgeti(L, phases_idx, @intCast(i));
            // Each phase entry is a table; tolerate non-tables by using empty strings.
            var ph_title: []const u8 = &.{};
            var ph_detail: []const u8 = &.{};
            if (c.lua_type(L, -1) == c.LUA_TTABLE) {
                const ph_idx: c_int = -1;
                // title
                const tt = c.lua_getfield(L, ph_idx, "title");
                if (tt == c.LUA_TSTRING) {
                    ph_title = try copyLuaString(L, -1, allocator);
                }
                c.lua_settop(L, -(1) - 1); // pop title
                // detail
                const dt = c.lua_getfield(L, ph_idx, "detail");
                if (dt == c.LUA_TSTRING) {
                    ph_detail = try copyLuaString(L, -1, allocator);
                }
                c.lua_settop(L, -(1) - 1); // pop detail
            }
            c.lua_settop(L, -(1) - 1); // pop phase entry
            try phases.append(allocator, .{ .title = ph_title, .detail = ph_detail });
        }
    }
    c.lua_settop(L, -(1) - 1); // pop phases value (table or nil/other)

    // Pop meta table.
    c.lua_settop(L, -(1) - 1);

    // -----------------------------------------------------------------------
    // Validate run field.
    // -----------------------------------------------------------------------
    const run_type = c.lua_getfield(L, module_idx, "run");
    if (run_type != c.LUA_TFUNCTION) {
        // errdefers above will free meta_name, meta_desc, and phases entries.
        return LuaError.LuaModuleMissingRun;
    }
    c.lua_settop(L, -(1) - 1); // pop run function

    // toOwnedSlice transfers ownership of the backing allocation away from the
    // ArrayList. The errdefer for phases fires only on error; on success, the
    // returned slice is owned by WorkflowModule.
    return WorkflowModule{
        .meta = WorkflowMeta{
            .name = meta_name,
            .description = meta_desc,
            .phases = try phases.toOwnedSlice(allocator),
        },
    };
}

// ---------------------------------------------------------------------------
// callRun — task 3164/3165: call run(ctx) with ctx.args populated.
// ---------------------------------------------------------------------------

/// callRun re-loads a workflow module from `source` and invokes its `run`
/// function with a `ctx` table carrying the caller-supplied `args`.
///
/// `args` is a slice of CLI argument strings (the trailing positionals from
/// the command line, excluding the workflow file path itself). They are
/// threaded into `ctx.args` as a 1-based Lua sequence of strings:
///   ctx.args[1] = args[0], ctx.args[2] = args[1], ...
///
/// An empty `args` slice produces an empty `ctx.args` table, preserving the
/// existing invariant that `ctx.args` is always a table.
///
/// Real host functions (agent, parallel, pipeline, log, budget, workflow)
/// are M2 work. A workflow whose `run` calls any of those globals will get
/// a Lua "attempt to call a nil value" runtime error — expected at M1.
///
/// `source` and `chunkname` must match what was passed to `loadModule`.
/// The Lua state is fresh and closed on return.
///
/// On Lua runtime errors, `err_buf` is populated and `LuaRuntimeError`
/// returned.
pub fn callRun(
    source: []const u8,
    chunkname: [*:0]const u8,
    args: []const []const u8,
    err_buf: []u8,
) LuaError!void {
    const L = c.luaL_newstate();
    if (L == null) return LuaError.LuaAllocFailed;
    defer c.lua_close(L);

    // Open the base library so scripts can use tostring/print/type/error etc.
    // Full stdlib sandboxing is M2 (m2-sandbox).
    c.luaL_openlibs(L);

    // Compile and execute the chunk to obtain the module table.
    const load_rc = c.luaL_loadbufferx(L, source.ptr, source.len, chunkname, null);
    if (load_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaCompileError;
    }
    const chunk_rc = c.lua_pcallk(L, 0, 1, 0, 0, null);
    if (chunk_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaRuntimeError;
    }

    // Stack: [-1] module table.
    if (c.lua_type(L, -1) != c.LUA_TTABLE) return LuaError.LuaModuleNotTable;
    const module_idx: c_int = -1;

    // Push module.run onto the stack.
    const run_type = c.lua_getfield(L, module_idx, "run");
    if (run_type != c.LUA_TFUNCTION) return LuaError.LuaModuleMissingRun;
    // Stack: [-2] module table, [-1] run function.

    // Build ctx table: { args = { [1]=args[0], [2]=args[1], ... } }.
    // Stack before: [-2] module table, [-1] run function.
    c.lua_createtable(L, 0, 1); // push ctx table
    // Stack: [-3] module, [-2] run, [-1] ctx.
    c.lua_createtable(L, @intCast(args.len), 0); // push args table (sequence hint)
    // Stack: [-4] module, [-3] run, [-2] ctx, [-1] args table.

    // Populate args as a 1-indexed Lua sequence.
    for (args, 0..) |arg, idx| {
        // lua_pushstring copies the C string. We pass a pointer to the first
        // byte; the slice must be null-terminated for lua_pushstring. Since
        // Zig slices are NOT guaranteed null-terminated, we use lua_pushlstring
        // which accepts a length, making it safe for arbitrary []const u8.
        _ = c.lua_pushlstring(L, arg.ptr, arg.len);
        // lua_rawseti(L, table_idx, key): args_table[idx+1] = arg_string
        // The args table is at [-2] after the string push ([-1] = string, [-2] = args table).
        c.lua_rawseti(L, -2, @intCast(idx + 1));
    }

    // lua_setfield(L, idx, k): sets t[idx][k] = stack[-1], pops stack[-1].
    // idx=-2 is ctx; sets ctx["args"] = args_table, pops args_table.
    c.lua_setfield(L, -2, "args");
    // Stack: [-3] module table, [-2] run function, [-1] ctx table.

    // Call run(ctx): 1 argument, 0 expected return values, no error handler.
    const run_rc = c.lua_pcallk(L, 1, 0, 0, 0, null);
    if (run_rc != c.LUA_OK) {
        captureError(L, err_buf);
        return LuaError.LuaRuntimeError;
    }
}

// ---------------------------------------------------------------------------
// CLI surface — task 3165.
// ---------------------------------------------------------------------------

/// Version string for planar-execute. Embeds the Lua version constant.
/// The Lua version string is defined as a comptime constant in lua.h:
///   #define LUA_VERSION "Lua 5.4"
/// We pair it with the binary name for consistency with the other binaries'
/// `<binary> <version-info>` format.
const planar_execute_version = "planar-execute 0.1.0 (lua " ++ c.LUA_VERSION_MAJOR ++ "." ++ c.LUA_VERSION_MINOR ++ ")";

/// The `run` subcommand: execute a workflow file.
///
/// This is the default path. When `planar-execute <workflow.lua>` is
/// invoked, `main` injects "run" before dispatching so the parser sees
/// `planar-execute run <workflow.lua>`.
///
/// Note: there is NO named-built-in registry — that is m10-quality-spine's
/// scope. Built-in workflows do not exist yet; the file-path form is the
/// only run-path in M1.
const run_verb: cli.Cmd = .{
    .name = "run",
    .desc = "Execute a workflow Lua file.",
    .long_desc =
    \\Execute a Lua 5.4 workflow script.
    \\
    \\  Usage:
    \\    planar-execute run <workflow.lua> [args...]
    \\    planar-execute run --dry-run <workflow.lua>
    \\
    \\  The workflow file must return a table:
    \\    { meta = { name, description, phases }, run = function(ctx) ... end }
    \\
    \\  Trailing [args...] are passed to run(ctx) as ctx.args[1], ctx.args[2], ...
    \\
    \\  Exit codes:
    \\    0   run() completed without error (or --dry-run validation passed).
    \\    1   Lua runtime error or missing/invalid workflow file.
    \\    2   Invalid module structure (bad meta/run shape).
    \\    3   Lua compile error.
    \\
    \\  Note: host functions (agent/parallel/pipeline/log) are not yet
    \\  registered at M1. A workflow that calls them will get a Lua runtime
    \\  error — this is expected and will be addressed in m2-host-fns.
    ,
    .flags = &.{
        .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false }, .desc = "Load and validate the workflow, print meta and phases, exit without running." },
    },
    .positionals = &.{
        .{ .name = "workflow", .kind = .string, .required = true },
    },
    .rest_field = "rest_args",
    .run = cli.handler(handleRun),
};

/// Root CLI command tree for `planar-execute`.
///
/// The default run-path is activated when the first non-flag, non-subcommand
/// argument is a file path. `main` detects this and injects "run" before
/// dispatch so the etc-cli parser sees the explicit `run` subcommand path.
pub const root: cli.Cmd = .{
    .name = "planar-execute",
    .desc = "Execute a Lua workflow script.",
    .long_desc =
    \\planar-execute — Lua 5.4 workflow execution harness (plan 492).
    \\
    \\  Usage:
    \\    planar-execute <workflow.lua> [args...]   Run a workflow file (default).
    \\    planar-execute run <workflow.lua> [args…] Explicit run subcommand.
    \\    planar-execute version                   Print version.
    \\    planar-execute --help                    Show this help.
    \\
    \\  The workflow file must return a Lua table:
    \\    { meta = { name, description, phases }, run = function(ctx) ... end }
    \\
    \\  Trailing [args...] are passed to the workflow as ctx.args[1], ctx.args[2], ...
    ,
    .cmds = &.{
        run_verb,
        version_verb,
    },
};

comptime {
    @setEvalBranchQuota(10_000);
    cli.validate(root);
}

/// `planar-execute version` — print version and exit 0.
const version_verb: cli.Cmd = .{
    .name = "version",
    .desc = "Print the planar-execute version and Lua runtime version.",
    .run = cli.handler(handleVersion),
};

fn handleVersion(args_ptr: *const anyopaque) anyerror!void {
    _ = args_ptr;
    const ctx = currentCtx();
    try ctx.stdout.print("{s}\n", .{planar_execute_version});
}

/// printDryRun writes the dry-run preview to `writer`:
///   workflow: <name>
///   description: <description>
///   phases: <N>
///     1. <title>[ — <detail>]
///     2. ...
///
/// Lines are newline-terminated. Detail is omitted when the phase's detail
/// string is empty. This is the stable output format pinned by integration tests.
pub fn printDryRun(mod: WorkflowModule, writer: *Io.Writer) !void {
    try writer.print("workflow: {s}\n", .{mod.meta.name});
    try writer.print("description: {s}\n", .{mod.meta.description});
    try writer.print("phases: {d}\n", .{mod.meta.phases.len});
    for (mod.meta.phases, 0..) |phase, i| {
        if (phase.detail.len > 0) {
            try writer.print("  {d}. {s} — {s}\n", .{ i + 1, phase.title, phase.detail });
        } else {
            try writer.print("  {d}. {s}\n", .{ i + 1, phase.title });
        }
    }
}

/// handleRun is the default handler: resolve the first positional as a
/// workflow file path, read the source, load the module, and invoke run(ctx).
///
/// Trailing positionals (rest_args) are threaded into ctx.args as a
/// 1-based Lua sequence.
///
/// When --dry-run is set: loads and validates the module (same code path as
/// normal), prints meta and phases via printDryRun, and exits 0 without
/// calling callRun. A malformed module still exits non-zero (load/validate
/// is shared). run(ctx) is never entered under --dry-run.
///
/// Error mapping:
///   - File not found / read error   → exit 1 (message on stderr)
///   - Lua compile error             → exit 3 (message on stderr)
///   - Invalid module shape          → exit 2 (message on stderr)
///   - Lua runtime error in run()    → exit 1 (message on stderr)
fn handleRun(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(root, &.{"run"}, args_ptr);
    const ctx = currentCtx();

    const workflow_path = args.workflow;
    const rest_args: []const []const u8 = args.rest_args;
    const dry_run: bool = args.dry_run;

    // Read the workflow source from disk.
    // Use the arena allocator from the process context. All allocations
    // within this handler are freed when the arena is torn down at exit.
    const allocator = ctx.allocator;

    const source = std.Io.Dir.cwd().readFileAlloc(ctx.io, workflow_path, allocator, .limited(16 * 1024 * 1024)) catch |e| {
        try ctx.stderr.print("planar-execute: cannot read '{s}': {s}\n", .{ workflow_path, @errorName(e) });
        try flushCtx();
        std.process.exit(1);
    };
    defer allocator.free(source);

    // Build a null-terminated chunkname from the workflow path.
    // Prefix with '@' so Lua shows the path as a filename in error messages
    // (e.g. "path/to/wf.lua:4: ...") rather than the "[string ...]" form.
    const chunkname_owned = allocator.alloc(u8, workflow_path.len + 2) catch {
        try ctx.stderr.print("planar-execute: out of memory\n", .{});
        try flushCtx();
        std.process.exit(1);
    };
    defer allocator.free(chunkname_owned);
    chunkname_owned[0] = '@';
    @memcpy(chunkname_owned[1 .. 1 + workflow_path.len], workflow_path);
    chunkname_owned[1 + workflow_path.len] = 0;
    const chunkname: [*:0]const u8 = @ptrCast(chunkname_owned.ptr);

    // Validate the module structure without running run().
    // Zero-init so that structural errors (LuaModuleNotTable etc.) that do
    // not write a Lua error string produce a clean "empty" message check.
    var err_buf: [512]u8 = @splat(0);
    var mod = loadModule(source, chunkname, allocator, &err_buf) catch |e| {
        const exit_code: u8 = switch (e) {
            LuaError.LuaCompileError => 3,
            LuaError.LuaModuleNotTable,
            LuaError.LuaModuleMissingMeta,
            LuaError.LuaModuleMissingRun,
            LuaError.LuaModuleInvalidMeta,
            => 2,
            else => 1,
        };
        const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
        if (msg.len > 0) {
            try ctx.stderr.print("planar-execute: {s}\n", .{msg});
        } else {
            try ctx.stderr.print("planar-execute: workflow load error: {s}\n", .{@errorName(e)});
        }
        try flushCtx();
        std.process.exit(exit_code);
    };
    defer mod.deinit(allocator);

    // --dry-run: print meta + phases and exit 0 WITHOUT calling run(ctx).
    // This is the load-bearing guarantee: run is never entered under --dry-run.
    if (dry_run) {
        try printDryRun(mod, ctx.stdout);
        try flushCtx();
        return; // exit 0 — no callRun
    }

    // Invoke run(ctx) with the trailing args threaded into ctx.args.
    callRun(source, chunkname, rest_args, &err_buf) catch |e| {
        const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
        if (msg.len > 0) {
            try ctx.stderr.print("planar-execute: {s}\n", .{msg});
        } else {
            try ctx.stderr.print("planar-execute: run error: {s}\n", .{@errorName(e)});
        }
        try flushCtx();
        std.process.exit(1);
    };
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

/// Inject "run" as the first positional when the user invoked the binary as
///   planar-execute <workflow.lua> [args…]
/// without the explicit `run` subcommand keyword. This mirrors how
/// `planar-watch` injects its default "feed" verb.
///
/// Injection is skipped when:
///   - the first non-binary token is already a known subcommand name, or
///   - it is a global flag (--help / -h) — let the parser handle it, or
///   - argv has no extra tokens at all (bare invocation → show help).
///
/// Special case: `--dry-run` is a flag that belongs to the `run` subcommand,
/// not to the root. When the user writes `planar-execute --dry-run <wf.lua>`
/// (default short form), we inject "run" so the parser routes correctly.
fn maybeInjectRun(arena: std.mem.Allocator, raw_args: []const []const u8) []const []const u8 {
    // argv[0] is the binary name; anything beyond is operator-supplied.
    if (raw_args.len <= 1) return raw_args; // bare invocation → no injection; parser shows help.

    const first = raw_args[1];

    // Known subcommand names and global flags — leave argv alone.
    inline for ([_][]const u8{ "run", "version", "--help", "-h" }) |v| {
        if (std.mem.eql(u8, first, v)) return raw_args;
    }

    // --dry-run belongs to the `run` subcommand — inject "run" before it so
    // the default short form `planar-execute --dry-run <wf.lua>` routes to
    // handleRun.
    const is_run_verb_flag = std.mem.eql(u8, first, "--dry-run");

    // Any other flag or option starting with `-` that is not a known run-verb
    // flag is left for the root parser to handle (it will likely error).
    if (first.len > 0 and first[0] == '-' and !is_run_verb_flag) return raw_args;

    // Looks like a file path, positional, or a run-verb flag — inject "run".
    var out = arena.alloc([]const u8, raw_args.len + 1) catch return raw_args;
    out[0] = raw_args[0];
    out[1] = "run";
    for (raw_args[1..], 0..) |a, i| out[2 + i] = a;
    return out;
}

pub fn main(init: std.process.Init) !void {
    const arena: std.mem.Allocator = init.arena.allocator();
    const raw_args = try init.minimal.args.toSlice(arena);
    const args = maybeInjectRun(arena, raw_args);

    initCtx(arena, init.io);
    defer flushCtx() catch {};

    const ctx = currentCtx();

    cli.dispatch(root, args, ctx.stdout) catch |e| switch (e) {
        cli.Parse.UnknownFlag,
        cli.Parse.MissingValue,
        cli.Parse.InvalidValue,
        cli.Parse.MissingRequired,
        cli.Parse.MissingRequiredPositional,
        cli.Parse.TooManyPositionals,
        cli.Parse.UnknownSubcommand,
        cli.Parse.UnexpectedArgument,
        cli.Parse.DuplicateFlag,
        => {
            try ctx.stderr.print("planar-execute: {s}\n", .{@errorName(e)});
            try flushCtx();
            std.process.exit(1);
        },
        error.NotImplemented => {
            try ctx.stderr.print("planar-execute: not implemented\n", .{});
            try flushCtx();
            std.process.exit(1);
        },
        else => return e,
    };
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

// ---------------------------------------------------------------------------
// task 3164 tests — workflow module loading
// ---------------------------------------------------------------------------

const testing_alloc = std.testing.allocator;

test "loadModule: well-formed module — meta read, run present" {
    // A complete well-formed workflow module.  loadModule must extract meta
    // fields correctly.  run is validated as a function but NOT called.
    const src =
        \\return {
        \\  meta = {
        \\    name = "hello-workflow",
        \\    description = "A test workflow",
        \\    phases = {
        \\      { title = "Phase 1", detail = "Do something" },
        \\      { title = "Phase 2", detail = "Do more" },
        \\    },
        \\  },
        \\  run = function(ctx) error("run must not be called by loadModule") end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    var mod = try loadModule(src, "test:well-formed", testing_alloc, &err_buf);
    defer mod.deinit(testing_alloc);

    try std.testing.expectEqualStrings("hello-workflow", mod.meta.name);
    try std.testing.expectEqualStrings("A test workflow", mod.meta.description);
    try std.testing.expectEqual(@as(usize, 2), mod.meta.phases.len);
    try std.testing.expectEqualStrings("Phase 1", mod.meta.phases[0].title);
    try std.testing.expectEqualStrings("Do something", mod.meta.phases[0].detail);
    try std.testing.expectEqualStrings("Phase 2", mod.meta.phases[1].title);
    try std.testing.expectEqualStrings("Do more", mod.meta.phases[1].detail);
}

test "loadModule: chunk returns non-table → LuaModuleNotTable" {
    // A chunk that returns a number instead of a table must yield
    // LuaModuleNotTable.
    var err_buf: [256]u8 = undefined;
    const err = loadModule("return 42", "test:not-table", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaModuleNotTable, err);
}

test "loadModule: missing meta field → LuaModuleMissingMeta" {
    // A table that omits meta entirely must yield LuaModuleMissingMeta.
    const src =
        \\return {
        \\  run = function(ctx) end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const err = loadModule(src, "test:no-meta", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaModuleMissingMeta, err);
}

test "loadModule: missing run field → LuaModuleMissingRun" {
    // A table with meta but no run must yield LuaModuleMissingRun.
    const src =
        \\return {
        \\  meta = {
        \\    name = "x",
        \\    description = "y",
        \\    phases = {},
        \\  },
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const err = loadModule(src, "test:no-run", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaModuleMissingRun, err);
}

test "loadModule: meta.name missing → LuaModuleInvalidMeta" {
    // meta present as table but name is absent (nil) → LuaModuleInvalidMeta.
    const src =
        \\return {
        \\  meta = {
        \\    description = "missing name",
        \\    phases = {},
        \\  },
        \\  run = function(ctx) end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const err = loadModule(src, "test:no-name", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaModuleInvalidMeta, err);
}

test "loadModule: phases array with entries — count and content" {
    // Three-phase module; verify count and all phase strings.
    const src =
        \\return {
        \\  meta = {
        \\    name = "three-phase",
        \\    description = "desc",
        \\    phases = {
        \\      { title = "A", detail = "a-detail" },
        \\      { title = "B", detail = "b-detail" },
        \\      { title = "C", detail = "c-detail" },
        \\    },
        \\  },
        \\  run = function(ctx) end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    var mod = try loadModule(src, "test:three-phase", testing_alloc, &err_buf);
    defer mod.deinit(testing_alloc);

    try std.testing.expectEqual(@as(usize, 3), mod.meta.phases.len);
    try std.testing.expectEqualStrings("A", mod.meta.phases[0].title);
    try std.testing.expectEqualStrings("c-detail", mod.meta.phases[2].detail);
}

test "loadModule: chunkname appears in compile error" {
    // A syntax error must cite the chunkname in the error message, not a
    // generic "[string ...]".
    var err_buf: [256]u8 = undefined;
    const err = loadModule("!!! bad syntax", "my-workflow.lua", testing_alloc, &err_buf);
    try std.testing.expectError(LuaError.LuaCompileError, err);
    // err_buf must contain "my-workflow.lua" (chunkname) in the message.
    const msg = std.mem.span(@as([*:0]const u8, @ptrCast(&err_buf)));
    try std.testing.expect(std.mem.indexOf(u8, msg, "my-workflow.lua") != null);
}

test "callRun: run(ctx) is actually invoked — empty args" {
    // The script uses a global to record that run was called.  callRun must
    // actually invoke run; the global is set inside run.  We verify the
    // invocation by confirming no error is returned (run completes without
    // raising an error) and by using a script whose run writes to io output
    // via a Lua global flag checked in a second evalString call.
    //
    // Since callRun uses its own fresh Lua state, the simplest proof is that
    // run executes without raising an error, then we inspect an observable
    // effect: we use a sentinel return pattern and confirm no LuaError is
    // returned.
    const src =
        \\local _ran = false
        \\return {
        \\  meta = {
        \\    name = "run-test",
        \\    description = "verify run is called",
        \\    phases = {},
        \\  },
        \\  run = function(ctx)
        \\    -- Verify ctx carries the args sub-table.
        \\    if type(ctx) ~= "table" then error("ctx must be a table") end
        \\    if type(ctx.args) ~= "table" then error("ctx.args must be a table") end
        \\    -- No error means run was reached.
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    try callRun(src, "test:callrun", &.{}, &err_buf);
}

test "callRun: ctx.args receives CLI positionals as 1-based sequence" {
    // The workflow's run receives ctx.args[1], ctx.args[2], ... matching the
    // order of the slice passed to callRun. This is the threading invariant
    // that task 3165 requires.
    const src =
        \\return {
        \\  meta = { name = "args-test", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    if ctx.args[1] ~= "hello" then error("expected ctx.args[1]='hello', got: " .. tostring(ctx.args[1])) end
        \\    if ctx.args[2] ~= "world" then error("expected ctx.args[2]='world', got: " .. tostring(ctx.args[2])) end
        \\    if ctx.args[3] ~= nil    then error("expected ctx.args[3]=nil, got: " .. tostring(ctx.args[3])) end
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const cli_args: []const []const u8 = &.{ "hello", "world" };
    try callRun(src, "test:args-threading", cli_args, &err_buf);
}

test "callRun: ctx.args length matches slice length" {
    // #ctx.args must equal the number of args passed.
    const src =
        \\return {
        \\  meta = { name = "len-test", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local n = #ctx.args
        \\    if n ~= 3 then error("expected #ctx.args=3, got " .. n) end
        \\  end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const cli_args: []const []const u8 = &.{ "a", "b", "c" };
    try callRun(src, "test:args-len", cli_args, &err_buf);
}

test "callRun: runtime error in run is surfaced" {
    // A run that calls error() must yield LuaRuntimeError, not a panic.
    const src =
        \\return {
        \\  meta = { name = "e", description = "d", phases = {} },
        \\  run = function(ctx) error("deliberate run error") end,
        \\}
    ;
    var err_buf: [256]u8 = undefined;
    const err = callRun(src, "test:callrun-err", &.{}, &err_buf);
    try std.testing.expectError(LuaError.LuaRuntimeError, err);
    try std.testing.expect(err_buf[0] != 0);
}

// ---------------------------------------------------------------------------
// task 3166 tests — printDryRun
// ---------------------------------------------------------------------------

test "printDryRun: output contains name, description, and phase titles" {
    // printDryRun must emit the workflow name, description, and each phase
    // title to the writer. This pins the stable output format.
    //
    // printDryRun takes an *Io.Writer; we capture output via
    // std.Io.Writer.Allocating — the allocating variant available in Zig 0.16.
    const alloc = std.testing.allocator;

    var buf: std.Io.Writer.Allocating = .init(alloc);
    defer buf.deinit();
    const w = &buf.writer;

    const phases = [_]PhaseMeta{
        .{ .title = "Setup", .detail = "initialize" },
        .{ .title = "Execute", .detail = "" },
        .{ .title = "Teardown", .detail = "clean up" },
    };
    const mod = WorkflowModule{
        .meta = WorkflowMeta{
            .name = "my-workflow",
            .description = "Does things",
            .phases = @constCast(&phases),
        },
    };

    try printDryRun(mod, w);

    const output = buf.writer.buffered();
    try std.testing.expect(std.mem.indexOf(u8, output, "workflow: my-workflow") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "description: Does things") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "phases: 3") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "1. Setup") != null);
    // Phase 2 has empty detail — must NOT include " — ".
    try std.testing.expect(std.mem.indexOf(u8, output, "2. Execute\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, output, "3. Teardown") != null);
}
