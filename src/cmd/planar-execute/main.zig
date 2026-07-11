//! planar-execute — the deterministic, spawn-free Lua workflow engine.
//!
//! Fifth Planar binary (plan 633). An LLM caller invokes `planar-execute` to do
//! a discrete chunk of *deterministic* work — shell allowlisted CLI verbs, run
//! confined git, do path-confined file IO, evaluate control flow — and get a
//! JSON result back.  It is NOT an orchestrator: its Lua host API exposes NO
//! model-spawning function (decision D5).  LLM orchestration is strictly the
//! caller's job; planar-execute is the deterministic substrate it drives.
//!
//! ## Hand-back model (decision D7)
//!
//! Discrete phase entrypoints, one clean process per deterministic segment:
//!
//!   planar-execute run <workflow.lua> --phase <name> [--args <json>]
//!
//! The engine loads the workflow in the sandbox, registers the D7 host surface
//! (`cli` / `git` / `fs` / `flow` / `ctx` — see `host.zig`), calls the named
//! phase function, marshals the workflow's `flow.result(table)` payload to JSON
//! on stdout, and exits.  There is NO coroutine that parks awaiting an external
//! worker — that resume point was where re-entrant spawning regrew when this
//! binary previously lived in-tree.  Arm/rep sequencing lives in the caller's
//! loop: phase A=setup → engine exits → caller does the LLM coder/reviewer step
//! → phase C=measure.
//!
//! ## Spawn-free by construction
//!
//! This file and `host.zig` shell only `planar` / `planar-agent` /
//! `planar-watch` (hardcoded allowlist) and `git` (confined to a host-injected
//! worktree).  There is no general `exec`, no `claude`/`codex`, no
//! `std.process.Child` headless-client path.  The host-fn manifest in
//! `host.zig` is the frozen capability surface P0.3 locks.

const std = @import("std");

const host = @import("host.zig");

// Pull the salvaged spawn-free modules in via `pub const` aliases so their
// declarations (and tests) are reachable from this binary's test compilation.
// Zig lazy-eval skips modules wired only via plain `@import`; the `refAllDecls`
// block below + these aliases ensure the new engine modules' tests actually run
// under `zig build test`.
pub const host_mod = host;
pub const state_mod = @import("state.zig");
pub const schema_mod = @import("schema.zig");
pub const brief_mod = @import("brief.zig");

// Reuse host.zig's cimport so the `lua_State` opaque type is identical across
// the two modules (two separate @cImport blocks produce incompatible types).
const c = host.c;

const Io = std.Io;

/// EngineError — the bounded error set the engine's run path maps everything
/// into. Marshalled to a non-zero exit with a clear message.
const EngineError = error{
    BadUsage,
    InitFailed,
    LoadFailed,
    PhaseMissing,
    PhaseFailed,
};

/// Parsed CLI arguments for `planar-execute run`.
const Args = struct {
    workflow: []const u8,
    phase: []const u8,
    args_json: []const u8 = "",
    worktree: []const u8 = "",
    sandbox_root: []const u8 = "",
};

/// errWrite writes `bytes` to stderr through `io`, swallowing IO errors (a
/// diagnostic that cannot be written is not worth crashing over).
fn errWrite(io: Io, bytes: []const u8) void {
    Io.File.stderr().writeStreamingAll(io, bytes) catch {};
}

/// outWrite writes `bytes` to stdout through `io`.
fn outWrite(io: Io, bytes: []const u8) !void {
    try Io.File.stdout().writeStreamingAll(io, bytes);
}

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const raw_args = try init.minimal.args.toSlice(arena);
    const io = init.io;

    if (raw_args.len < 2) {
        usage(io);
        std.process.exit(2);
    }

    const verb = raw_args[1];
    if (std.mem.eql(u8, verb, "run")) {
        const parsed = parseRunArgs(arena, raw_args[2..]) catch {
            usage(io);
            std.process.exit(2);
        };
        runWorkflow(arena, io, parsed) catch |e| {
            switch (e) {
                EngineError.BadUsage => {
                    usage(io);
                    std.process.exit(2);
                },
                else => std.process.exit(1),
            }
        };
        return;
    }

    if (std.mem.eql(u8, verb, "--help") or std.mem.eql(u8, verb, "-h") or std.mem.eql(u8, verb, "help")) {
        usage(io);
        return;
    }

    errWrite(io, "planar-execute: unknown verb: ");
    errWrite(io, verb);
    errWrite(io, "\n");
    usage(io);
    std.process.exit(2);
}

fn usage(io: Io) void {
    errWrite(io,
        \\planar-execute — deterministic, spawn-free Lua workflow engine.
        \\
        \\Usage:
        \\  planar-execute run <workflow.lua> --phase <name> [--args <json>]
        \\                     [--worktree <dir>] [--sandbox-root <dir>]
        \\
        \\Loads the workflow in the sandbox, registers the deterministic host
        \\surface (cli/git/fs/flow/ctx), calls the named phase, and prints the
        \\workflow's flow.result(table) payload as JSON on stdout.
        \\
    );
}

/// parseRunArgs parses the `run` verb's positional + flags. The first
/// positional is the workflow path; --phase is required.
fn parseRunArgs(arena: std.mem.Allocator, args: []const []const u8) EngineError!Args {
    _ = arena;
    var workflow: ?[]const u8 = null;
    var phase: ?[]const u8 = null;
    var args_json: []const u8 = "";
    var worktree: []const u8 = "";
    var sandbox_root: []const u8 = "";

    var i: usize = 0;
    while (i < args.len) : (i += 1) {
        const a = args[i];
        if (std.mem.eql(u8, a, "--phase")) {
            i += 1;
            if (i >= args.len) return EngineError.BadUsage;
            phase = args[i];
        } else if (std.mem.eql(u8, a, "--args")) {
            i += 1;
            if (i >= args.len) return EngineError.BadUsage;
            args_json = args[i];
        } else if (std.mem.eql(u8, a, "--worktree")) {
            i += 1;
            if (i >= args.len) return EngineError.BadUsage;
            worktree = args[i];
        } else if (std.mem.eql(u8, a, "--sandbox-root")) {
            i += 1;
            if (i >= args.len) return EngineError.BadUsage;
            sandbox_root = args[i];
        } else if (std.mem.startsWith(u8, a, "--")) {
            return EngineError.BadUsage;
        } else if (workflow == null) {
            workflow = a;
        } else {
            return EngineError.BadUsage;
        }
    }

    return Args{
        .workflow = workflow orelse return EngineError.BadUsage,
        .phase = phase orelse return EngineError.BadUsage,
        .args_json = args_json,
        .worktree = worktree,
        .sandbox_root = sandbox_root,
    };
}

/// runWorkflow is the engine core: open a sandboxed Lua state, register the D7
/// host surface, load + execute the workflow chunk (which defines its phase
/// functions as globals), invoke the named phase, then marshal the captured
/// `flow.result` payload to stdout.
fn runWorkflow(arena: std.mem.Allocator, io: Io, args: Args) !void {
    // Read the workflow file (NOT confined — the workflow path is operator
    // input, not script-controlled; fs.* confinement applies to in-script IO).
    const source = std.Io.Dir.cwd().readFileAllocOptions(
        io,
        args.workflow,
        arena,
        .limited(8 * 1024 * 1024),
        .of(u8),
        0,
    ) catch {
        errWrite(io, "planar-execute: cannot read workflow: ");
        errWrite(io, args.workflow);
        errWrite(io, "\n");
        return EngineError.LoadFailed;
    };

    const L = c.luaL_newstate() orelse return EngineError.LoadFailed;
    defer c.lua_close(L);

    // Curated, deterministic stdlib; nil os/io/loaders/math.random.
    host.openSandboxedLibs(L);

    // Determinism inputs: ctx.now / ctx.seed are the only time/seed source the
    // sandbox sees (os/math.random are nil'd). Both default to 0 so a run is
    // reproducible by default; the caller injects concrete values via --args
    // when a workflow needs a real clock/seed. Keeping the wall clock OUT of
    // the engine is the deterministic-by-default posture D5/D7 want.
    const bin_dir = std.process.executableDirPathAlloc(io, arena) catch return EngineError.InitFailed;
    var hs = host.HostState{
        .arena = arena,
        .io = io,
        .bin_dir = bin_dir,
        .worktree = args.worktree,
        .sandbox_root = args.sandbox_root,
        .now = 0,
        .seed = 0,
        .args_json = args.args_json,
    };

    host.installHostSurface(L, &hs);

    // Load the workflow chunk (defines phase functions as globals).
    if (c.luaL_loadbufferx(L, source.ptr, source.len, "@workflow", null) != 0) {
        reportLuaError(L, io, "load");
        return EngineError.LoadFailed;
    }
    // Execute the top-level chunk so phase functions get defined.
    if (c.lua_pcallk(L, 0, 0, 0, 0, null) != 0) {
        reportLuaError(L, io, "init");
        return EngineError.LoadFailed;
    }

    // Resolve the named phase function as a global.
    const phase_z = std.fmt.allocPrintSentinel(arena, "{s}", .{args.phase}, 0) catch return EngineError.PhaseMissing;
    const ptype = c.lua_getglobal(L, phase_z.ptr);
    if (ptype != c.LUA_TFUNCTION) {
        errWrite(io, "planar-execute: phase function not found: ");
        errWrite(io, args.phase);
        errWrite(io, "\n");
        return EngineError.PhaseMissing;
    }

    // Call the phase with no args (the script reaches ctx/cli/git/fs/flow as
    // globals). The phase signals completion via flow.result / flow.fail.
    if (c.lua_pcallk(L, 0, 0, 0, 0, null) != 0) {
        // flow.fail raises a Lua error too; prefer the captured fail message.
        if (hs.fail_msg) |fm| {
            errWrite(io, "planar-execute: phase failed: ");
            errWrite(io, fm);
            errWrite(io, "\n");
        } else {
            reportLuaError(L, io, "phase");
        }
        return EngineError.PhaseFailed;
    }

    // Marshal the result payload to stdout (clean JSON channel).
    if (hs.result_json) |rj| {
        try outWrite(io, rj);
        try outWrite(io, "\n");
    } else {
        // No explicit result — emit an empty object so callers always get JSON.
        try outWrite(io, "{}\n");
    }
}

/// reportLuaError reads the error value off the top of the Lua stack and prints
/// it to stderr with a stage prefix. Pops the error value.
fn reportLuaError(L: ?*c.lua_State, io: Io, stage: []const u8) void {
    var len: usize = 0;
    const raw = c.luaL_tolstring(L, -1, &len);
    const msg: []const u8 = if (raw != null) raw[0..len] else "(unknown error)";
    errWrite(io, "planar-execute: ");
    errWrite(io, stage);
    errWrite(io, " error: ");
    errWrite(io, msg);
    errWrite(io, "\n");
    c.lua_settop(L, -3); // pop coerced string + original error
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test {
    // Pull every declaration (and the aliased engine modules' tests) into this
    // binary's test compilation unit so they actually run under `zig build test`.
    std.testing.refAllDecls(@This());
}

test "parseRunArgs: workflow + phase + args" {
    const a = try parseRunArgs(std.testing.allocator, &.{ "wf.lua", "--phase", "setup", "--args", "{\"x\":1}" });
    try std.testing.expectEqualStrings("wf.lua", a.workflow);
    try std.testing.expectEqualStrings("setup", a.phase);
    try std.testing.expectEqualStrings("{\"x\":1}", a.args_json);
}

test "parseRunArgs: missing phase is BadUsage" {
    try std.testing.expectError(EngineError.BadUsage, parseRunArgs(std.testing.allocator, &.{"wf.lua"}));
}

test "parseRunArgs: missing workflow is BadUsage" {
    try std.testing.expectError(EngineError.BadUsage, parseRunArgs(std.testing.allocator, &.{ "--phase", "setup" }));
}

test "parseRunArgs: worktree + sandbox-root flags" {
    const a = try parseRunArgs(std.testing.allocator, &.{ "wf.lua", "--phase", "p", "--worktree", "/tmp/wt", "--sandbox-root", "/tmp/sb" });
    try std.testing.expectEqualStrings("/tmp/wt", a.worktree);
    try std.testing.expectEqualStrings("/tmp/sb", a.sandbox_root);
}
