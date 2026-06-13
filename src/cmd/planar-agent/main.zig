//! planar-agent — agent-callable coordination binary.
//!
//! Owns every write to `agent_work_claims` and every agent-coordination
//! write to `agent_actions`. Operator recovery verbs (`reconcile`,
//! `abort`) live here too because both are agent_* table writers; the
//! capability boundary tracks tables, not audience. (One narrow
//! exception: the `planar` operator binary also appends a best-effort
//! entity-create provenance row to `agent_actions` when a planning
//! entity is created under an active claim — plan 467 D2/D3.)
//!
//! Four-binary architecture:
//!   - `planar`         : operator surface; runs migrations; writes
//!                        planning + tasks.status.
//!   - `planar-agent`   : THIS binary; writes agent_* + tasks.status
//!                        only as part of atomic coordinated operations.
//!   - `planar-watch`   : read-only viewer; opens DB with `?mode=ro`.
//!   - `planar-doc`     : doc-system manifest tool; no SQLite driver.
//!
//! Schema-version handshake: planar-agent is a CONSUMER of the schema,
//! not its owner. Startup queries `schema_migrations.max(version)` and
//! refuses with exit 7 when the live DB is older than the binary's
//! embedded minimum. The remediation pointer ("run `planar init`")
//! lives in `runtime.ensureDbConsumer`.
//!
//! M2 ships the full 13-verb agent surface: 6 atomic ops (pull / peek /
//! complete / fail / release / block) + 2 claim primitives (claim /
//! heartbeat) + 2 nested action verbs (action start / action end) +
//! ingest (skeleton; full adapter routing in M4) + 2 operator-recovery
//! verbs (reconcile / abort). See handlers/cmd.zig for the registry.

const std = @import("std");
const Io = std.Io;

const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const cmd_tree = @import("handlers/cmd.zig");
const exit = @import("exit.zig");

/// Root command tree for `planar-agent`. Public so handler files can
/// derive their typed Args via `cli.castArgs(main.root, &.{…}, ptr)`.
pub const root: cli.Cmd = .{
    .name = "planar-agent",
    .desc = "Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).",
    .cmds = cmd_tree.verbs,
};

comptime {
    @setEvalBranchQuota(50_000);
    cli.validate(root);
}

// Writer buffers live at module scope so runtime.Ctx pointers remain
// valid for the lifetime of the process. Sized identically to `planar`
// — the agent surface has no special output volume.
var stdout_buffer: [4096]u8 = undefined;
var stderr_buffer: [1024]u8 = undefined;

pub fn main(init: std.process.Init) !void {
    const arena: std.mem.Allocator = init.arena.allocator();
    const args = try init.minimal.args.toSlice(arena);

    const db_path = try runtime.resolveDbPath(arena, init.minimal.environ);
    runtime.init(arena, init.io, &stdout_buffer, &stderr_buffer, db_path, init.minimal.environ, args);
    defer runtime.shutdown();

    // Schema-version handshake. We perform it lazily AFTER help / version
    // verbs so a plain `planar-agent --help` doesn't require a DB at all
    // (matches the `planar` binary's lazy-DB ergonomics).
    //
    // The handshake guard is wired through `runtime.ensureDbConsumer`
    // which opens the DB WITHOUT applying migrations and refuses with
    // `SchemaVersionBehind` when the live DB is older than the binary's
    // embedded minimum. Operator surfaces in M2 call ensureDbConsumer
    // as their first DB-touching line; the version verb does not.

    cli.dispatch(root, args, runtime.current().stdout) catch |e| switch (e) {
        cli.Parse.UnknownFlag,
        cli.Parse.MissingValue,
        cli.Parse.InvalidValue,
        cli.Parse.MissingRequired,
        cli.Parse.MissingRequiredPositional,
        cli.Parse.TooManyPositionals,
        cli.Parse.UnknownSubcommand,
        cli.Parse.UnexpectedArgument,
        cli.Parse.DuplicateFlag,
        => exit.die(runtime.current(), e, "{s}", .{@errorName(e)}),
        error.NotImplemented => exit.die(runtime.current(), e, "not implemented yet", .{}),
        // SchemaVersionBehind / SchemaVersionAhead bubble up through
        // handlers that called ensureDb; map both to exit 7.
        error.SchemaVersionBehind, error.SchemaVersionAhead => |se| {
            // The handshake helper already printed the remediation
            // message to stderr; just propagate the exit code.
            exit.die(runtime.current(), se, "{s}", .{@errorName(se)});
        },
        else => return e,
    };

    try runtime.flush();

    // Keep db import live so module test discovery picks it up when we
    // start linking the engine + handlers in M2.
    _ = db;
}

// =========================================================================
// Test-aggregation block — forces Zig to analyse every handler module so
// colocated `test {}` blocks are discovered by `zig build test`.
//
// Without this, Zig's lazy-eval silently skips modules that are only
// reachable via `pub const @import` chains (the same failure mode fixed
// for planar-execute by task 3238). refAllDecls on @This() reaches the
// handlers via the `cmd_tree` import above, which imports every verb file.
//
// Note: Zig 0.16 ships refAllDecls (non-recursive); refAllDeclsRecursive
// is not available. refAllDecls on @This() is sufficient here because the
// handler files are direct pub-const exports of cmd_tree.
// =========================================================================

test {
    std.testing.refAllDecls(@This());
}
