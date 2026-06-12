//! planar-watch — human-facing read-only viewer for agent activity.
//!
//! Third binary in the three-binary architecture (plan 85). Opens the
//! SQLite database via `runtime.ensureDbStrictReadOnly`, which uses
//! `sqlite3_open_v2(..., SQLITE_OPEN_READONLY, ...)` so the SQLite
//! driver itself refuses every write SQL string. That is the SECOND
//! line of defense behind the capability boundary; the FIRST is that
//! the command tree below registers exactly six read verbs (feed, ps,
//! claims, actions, plans, log) plus the conventional `version` and
//! `completion` helpers. There is no write verb anywhere in this
//! binary.
//!
//! Three-binary architecture:
//!   - `planar`         : operator surface; runs migrations; writes
//!                        planning + tasks.status.
//!   - `planar-agent`   : agent-callable coordination; writes
//!                        agent_* + tasks.status only as part of
//!                        atomic coordinated operations.
//!   - `planar-watch`   : THIS binary; read-only viewer.
//!
//! Verb set (plan 585 addition): `run` group (list / show) provides
//! read-only observability for `workflow_runs` + `context_records`.
//!
//! Schema-version handshake: `planar-watch` is a CONSUMER of the
//! schema, not its owner. Startup queries
//! `schema_migrations.max(version)` and refuses with exit 7 when the
//! live DB is older than the binary's embedded minimum. The
//! remediation pointer ("run `planar init`") lives in
//! `runtime.ensureDbStrictReadOnly`.
//!
//! Default verb: when invoked with no positional verb at all (e.g.
//! `planar-watch`), main rewrites argv to `planar-watch feed`. The
//! activity feed is the cockpit's primary view; users typing the
//! binary name with no args get it for free. `--help` is preserved.

const std = @import("std");
const Io = std.Io;

const cli = @import("cli");
const db = @import("db");
const runtime = @import("runtime");

const cmd_tree = @import("handlers/cmd.zig");
const exit = @import("exit.zig");

/// Root command tree for `planar-watch`. Public so handler files can
/// derive their typed Args via `cli.castArgs(main.root, &.{…}, ptr)`.
pub const root: cli.Cmd = .{
    .name = "planar-watch",
    .desc = "Read-only viewer for live agent activity (feed / ps / claims / actions / plans / log / tree / run).",
    .long_desc = "planar-watch is the human-facing live cockpit for agent activity.\n\n" ++
        "  The default invocation with no args is the activity feed.\n" ++
        "  Subcommands narrow the view; `--follow` turns each one into a\n" ++
        "  streaming view that emits new rows as the underlying tables\n" ++
        "  change. The binary opens the database in strict read-only mode\n" ++
        "  (SQLITE_OPEN_READONLY) — every write SQL string is rejected by\n" ++
        "  the SQLite driver itself, the second line of defense behind\n" ++
        "  this binary's `no write verbs registered` capability boundary.\n\n" ++
        "  `tree` renders the orchestrator → sub-agent forest by walking\n" ++
        "  agent_actions.parent_action_id chains.",
    .cmds = cmd_tree.verbs,
};

comptime {
    @setEvalBranchQuota(50_000);
    cli.validate(root);
}

// Writer buffers live at module scope so runtime.Ctx pointers remain
// valid for the lifetime of the process. Sized identically to the
// other binaries.
var stdout_buffer: [4096]u8 = undefined;
var stderr_buffer: [1024]u8 = undefined;

pub fn main(init: std.process.Init) !void {
    const arena: std.mem.Allocator = init.arena.allocator();
    const raw_args = try init.minimal.args.toSlice(arena);

    // Default-verb rewrite: `planar-watch` with no extra args (or only
    // pure --flag forms with no verb) becomes `planar-watch feed`. We
    // do a minimal scan to detect whether the user supplied a verb at
    // all. Anything that looks like a known verb name or `--help` /
    // `-h` short-circuits the rewrite so existing behavior stays.
    const args = maybeInjectDefaultVerb(arena, raw_args);

    const db_path = try runtime.resolveDbPath(arena, init.minimal.environ);
    runtime.init(arena, init.io, &stdout_buffer, &stderr_buffer, db_path, init.minimal.environ, args);
    defer runtime.shutdown();

    // The handshake is performed lazily by each handler that touches
    // the DB (they call `runtime.ensureDbStrictReadOnly`). That keeps
    // `planar-watch --help` and `planar-watch version` working
    // without a DB present — consistent ergonomics with `planar` and
    // `planar-agent`.

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
        error.SchemaVersionBehind, error.SchemaVersionAhead => |se| {
            // The handshake helper already printed the remediation
            // message to stderr; propagate the exit code.
            exit.die(runtime.current(), se, "{s}", .{@errorName(se)});
        },
        else => return e,
    };

    try runtime.flush();

    // Keep db import live so module test discovery picks it up.
    _ = db;
}

/// Rewrite argv to inject `feed` as the default verb when none was
/// supplied. The check is conservative: if the first non-binary
/// token starts with `-` (i.e. a flag) AND is not `--help` / `-h`,
/// we treat the command as verb-less and prepend `feed`. If the
/// first token is one of our known verbs we leave argv alone.
fn maybeInjectDefaultVerb(
    arena: std.mem.Allocator,
    raw_args: []const []const u8,
) []const []const u8 {
    // argv[0] is the binary name; anything beyond is operator-supplied.
    if (raw_args.len <= 1) {
        // Just the binary name → default to `feed`.
        var out = arena.alloc([]const u8, 2) catch return raw_args;
        out[0] = raw_args[0];
        out[1] = "feed";
        return out;
    }
    const first = raw_args[1];

    // Known verb names — leave argv alone.
    inline for ([_][]const u8{
        "feed",       "ps",    "claims",
        "actions",    "plans", "log",
        "tree",       "run",   "version",
        "completion",
    }) |v| {
        if (std.mem.eql(u8, first, v)) return raw_args;
    }

    // Help requests stay as-is (the help flow handles --help / -h on
    // the root command).
    if (std.mem.eql(u8, first, "--help") or std.mem.eql(u8, first, "-h")) return raw_args;

    // First non-binary token is a flag (e.g. `planar-watch --json`)
    // OR an unknown verb. The latter we leave for cli.dispatch to
    // report; the former we treat as default-feed.
    if (first.len > 0 and first[0] == '-') {
        var out = arena.alloc([]const u8, raw_args.len + 1) catch return raw_args;
        out[0] = raw_args[0];
        out[1] = "feed";
        for (raw_args[1..], 0..) |a, i| out[2 + i] = a;
        return out;
    }

    // Anything else — let dispatch handle it (probably an unknown
    // subcommand error).
    return raw_args;
}
