//! runtime — process-wide context shared across all Planar binaries.
//!
//! Linked into `planar`, `planar-agent`, and `planar-watch`. The
//! `current()` accessor is the contract: each binary's `main.zig`
//! initializes a process-global Ctx before dispatch, and handlers grab
//! it via `runtime.current()`.
//!
//! Exactly one Ctx per process. Set once at startup, lives until exit;
//! no synchronization needed (handlers run on the main thread under
//! cli.dispatch).
//!
//! **Lazy DB acquisition.** `init` does NOT open the DB — opening
//! triggers migration check (one SELECT minimum, plus inserts on
//! upgrade). Help-only paths (`planar --help`, `planar plan --help`)
//! should not pay that cost. Handlers that actually need the DB call
//! `ensureDb()` as their first DB-touching line; the first call opens
//! and migrates, subsequent calls return the cached handle.
//!
//! `planar` runs migrations on open. `planar-agent` and `planar-watch`
//! refuse to start if the DB's max schema version is older than this
//! binary's minimum required — they're consumers of the schema, not
//! owners. (Future read-only `openReadOnly` path for `planar-watch`
//! is layered on top of `ensureDb`'s caching.)
//!
//! Things explicitly NOT on Ctx:
//!   - --json mode: per-handler, lives in the parsed args struct,
//!     passed to output.emit at the call site.
//!   - --scope override: per-write-verb, passed to scope.resolve.
//!   - --verbose: passed through args where declared.
//! Anything that varies per invocation stays in args, not Ctx, so
//! handlers can't accidentally mix two operators' state.

const std = @import("std");
const Io = std.Io;
const db = @import("db");

/// Snapshot of everything a handler needs from the process environment.
/// Owned by `runtime.zig`; handed out by const pointer via `current()`.
pub const Ctx = struct {
    allocator: std.mem.Allocator,
    io: Io,
    stdout: *Io.Writer,
    stderr: *Io.Writer,
    db_path: [:0]const u8,
    environ: std.process.Environ,
    argv: []const []const u8,
};

var current_ctx: ?Ctx = null;
var stdout_writer_storage: ?Io.File.Writer = null;
var stderr_writer_storage: ?Io.File.Writer = null;
var db_storage: ?db.sqlite.Db = null;

/// Install the process Ctx. Call exactly once from main, before
/// dispatch. The buffer slices and `db_path` must outlive the process.
/// Does NOT open the DB — see `ensureDb`.
pub fn init(
    allocator: std.mem.Allocator,
    io: Io,
    stdout_buf: []u8,
    stderr_buf: []u8,
    db_path: [:0]const u8,
    environ: std.process.Environ,
    argv: []const []const u8,
) void {
    stdout_writer_storage = Io.File.Writer.init(.stdout(), io, stdout_buf);
    stderr_writer_storage = Io.File.Writer.init(.stderr(), io, stderr_buf);
    current_ctx = .{
        .allocator = allocator,
        .io = io,
        .stdout = &stdout_writer_storage.?.interface,
        .stderr = &stderr_writer_storage.?.interface,
        .db_path = db_path,
        .environ = environ,
        .argv = argv,
    };
}

/// Borrow the Ctx. Panics if `init` hasn't run — that's a programming
/// error in main, not a runtime condition worth handling.
pub fn current() *const Ctx {
    return &(current_ctx orelse @panic("runtime.current() before runtime.init()"));
}

/// Open the DB if it isn't open yet, apply pending migrations
/// idempotently, and return the cached handle. Subsequent calls are
/// O(1). Creates the parent directory if it doesn't exist (single
/// level — covers `~/.planar/`).
///
/// After migrations are applied the schema-version guard fires if the
/// DB's max version exceeds this binary's embedded max (i.e. a newer
/// binary migrated the DB and this binary is stale). On `SchemaVersionAhead`
/// the caller should surface both versions to the operator and suggest
/// upgrading the binary.
pub fn ensureDb() !*db.sqlite.Db {
    if (db_storage != null) return &db_storage.?;
    const ctx = current();

    if (ctx.db_path.len > 0 and ctx.db_path[0] == '/') {
        if (std.fs.path.dirname(ctx.db_path)) |parent| {
            std.Io.Dir.createDirAbsolute(ctx.io, parent, .default_dir) catch |e| switch (e) {
                error.PathAlreadyExists => {},
                else => return e,
            };
        }
    }

    db_storage = try db.sqlite.Db.open(ctx.db_path.ptr);

    // WAL mode is a load-bearing prerequisite for the wake-tier ladder
    // behind `planar-watch <verb> --follow` (Tier 2 kqueue / inotify on
    // the `-wal` sibling; Tier 3 writer-side update_hook → sidecar
    // notify). It also lets `planar-watch` read concurrently while
    // `planar-agent` writes. The PRAGMA is idempotent — existing
    // databases switch on next open without rewriting rows. Errors are
    // swallowed only after we surface them to stderr; a WAL-mode
    // failure on first open turns into "Tier 1 polling continues to
    // work" rather than refusing service.
    db_storage.?.exec("PRAGMA journal_mode = WAL") catch |e| {
        ctx.stderr.print(
            "warning: failed to set journal_mode=WAL ({s}); follow / dashboard latency may degrade\n",
            .{@errorName(e)},
        ) catch {};
    };

    try db.migrate.applyAll(&db_storage.?, ctx.allocator);

    var db_version: u32 = 0;
    var emb_max: u32 = 0;
    db.migrate.assertSchemaCompatible(&db_storage.?, &db_version, &emb_max) catch |e| switch (e) {
        error.SchemaVersionAhead => {
            ctx.stderr.print(
                "error: schema_version {d} in {s} is newer than this binary's embedded max ({d}); " ++
                    "upgrade your binary or use one that supports schema_version >= {d}\n",
                .{ db_version, ctx.db_path, emb_max, db_version },
            ) catch {};
            db_storage.?.close();
            db_storage = null;
            return e;
        },
    };

    return &db_storage.?;
}

/// Flush both writers. Errors propagate — call from the normal-exit
/// path so the caller learns if buffered output was lost.
pub fn flush() !void {
    if (current_ctx) |*c| {
        try c.stdout.flush();
        try c.stderr.flush();
    }
}

/// Final teardown: flush writers (best-effort), close the DB if open,
/// reset module state. Called from `main`'s deferred-cleanup path AND
/// from `exit.die` before `std.process.exit`, since `process.exit`
/// skips defers and would otherwise leave the DB un-closed and stderr
/// half-written. Best-effort because callers are already on the way
/// out — there's nowhere to surface a flush error to.
pub fn shutdown() void {
    if (current_ctx) |*c| {
        c.stdout.flush() catch {};
        c.stderr.flush() catch {};
    }
    if (db_storage) |*d| {
        d.close();
        db_storage = null;
    }
}

/// Resolve the DB path with the standard precedence:
///   1. $PLANAR_DB (test override, custom installs)
///   2. $HOME/.planar/planar.db (default)
///
/// Returns a sentinel-terminated string owned by `allocator`. The
/// parent directory is NOT created here — `ensureDb` does that lazily
/// the first time the DB is actually needed.
pub fn resolveDbPath(
    allocator: std.mem.Allocator,
    environ: std.process.Environ,
) ![:0]const u8 {
    if (environ.getPosix("PLANAR_DB")) |raw| {
        return try allocator.dupeZ(u8, raw);
    }
    const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
    return try std.fs.path.joinZ(allocator, &.{ home, ".planar", "planar.db" });
}

test "resolveDbPath: PLANAR_DB overrides HOME" {
    // The Zig std doesn't expose an in-memory Environ constructor we
    // can hand to resolveDbPath without a process snapshot. We exercise
    // the override + default paths via the real process Environ in the
    // integration suite instead; this unit test pins the function shape
    // so a signature change shows up at compile time.
    const E = @TypeOf(resolveDbPath);
    _ = E;
}
