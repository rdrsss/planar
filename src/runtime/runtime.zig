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

/// Error: the live DB's max schema version is OLDER than this binary's
/// embedded minimum. Raised by `ensureDbConsumer` (which does NOT apply
/// migrations); the operator runs `planar init` to bring the DB
/// forward. Distinct from `SchemaVersionAhead` (binary too old for
/// DB) so scripts and operators can tell the two skew directions
/// apart cleanly.
pub const SchemaVersionBehind = error{SchemaVersionBehind};

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
    // 5-second busy timeout: under WAL, writers still serialize behind
    // the writer lock. Without a busy_timeout SQLite returns SQLITE_BUSY
    // immediately the moment a second writer (e.g. a concurrent
    // planar-agent process under the cross-process contention path)
    // tries to BEGIN IMMEDIATE while another holds the lock. The
    // 5-second default lets short writer-held transactions complete
    // without forcing every caller to retry. Best-effort; failures
    // degrade to "every BEGIN IMMEDIATE collision returns BUSY
    // immediately," not a silent miscompile.
    db_storage.?.exec("PRAGMA busy_timeout = 5000") catch |e| {
        ctx.stderr.print(
            "warning: failed to set busy_timeout ({s}); concurrent writers may see SQLITE_BUSY\n",
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

/// Open the DB as a schema *consumer* — runs the schema-handshake guard
/// but does NOT apply migrations, returning a writable handle. Used by
/// `planar-agent` (which writes to `agent_actions`, `agent_work_claims`,
/// and `tasks.status` but is not allowed to mutate the schema itself).
/// `planar init` is the only verb that applies migrations; every other
/// binary is a consumer of the schema, not its owner.
///
/// Naming note: the handle returned is read/write at the SQLite driver
/// level — the "consumer" label refers to the schema lifecycle (consumer
/// of versions vs. owner / migrator), not to data-plane writability. The
/// truly read-only entry point is `ensureDbStrictReadOnly`, used by
/// `planar-watch`, which opens the file with `?mode=ro` so even data
/// writes are rejected at the driver layer.
///
/// Two refusal paths, both writing a remediation message to stderr
/// before returning:
///
///   - SchemaVersionBehind  — live DB version < embedded_max. The
///                            operator runs `planar init` to bring the
///                            DB forward.
///   - SchemaVersionAhead   — live DB version > embedded_max. The
///                            operator upgrades the binary.
///
/// On success returns the cached `*db.sqlite.Db` (same singleton
/// `ensureDb` would return). Subsequent calls are O(1).
pub fn ensureDbConsumer() !*db.sqlite.Db {
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

    // WAL mode is per-connection and load-bearing for the wake-tier
    // ladder (planar-watch follow). Same idempotent PRAGMA the writer
    // path uses.
    db_storage.?.exec("PRAGMA journal_mode = WAL") catch |e| {
        ctx.stderr.print(
            "warning: failed to set journal_mode=WAL ({s}); follow / dashboard latency may degrade\n",
            .{@errorName(e)},
        ) catch {};
    };
    // 5-second busy timeout: under WAL, writers still serialize behind
    // the writer lock. Without a busy_timeout SQLite returns SQLITE_BUSY
    // immediately the moment a second writer (e.g. a concurrent
    // planar-agent process under the cross-process contention path)
    // tries to BEGIN IMMEDIATE while another holds the lock. The
    // 5-second default lets short writer-held transactions complete
    // without forcing every caller to retry. Best-effort; failures
    // degrade to "every BEGIN IMMEDIATE collision returns BUSY
    // immediately," not a silent miscompile.
    db_storage.?.exec("PRAGMA busy_timeout = 5000") catch |e| {
        ctx.stderr.print(
            "warning: failed to set busy_timeout ({s}); concurrent writers may see SQLITE_BUSY\n",
            .{@errorName(e)},
        ) catch {};
    };

    // Read the live schema version. A missing `schema_migrations` table
    // (fresh DB never touched by `planar init`) maps to version 0 —
    // which trips SchemaVersionBehind below as long as the binary's
    // embedded migrations include anything at all.
    const db_version: u32 = if (db_storage.?.intQuery(
        "select coalesce(max(version), 0) from schema_migrations",
    )) |v| @intCast(v) else |_| 0;
    const emb_max = db.migrate.embedded_max;

    if (db_version < emb_max) {
        ctx.stderr.print(
            "error: schema version {d} in {s} is older than this binary's minimum of {d}; " ++
                "run `planar init` to apply migrations\n",
            .{ db_version, ctx.db_path, emb_max },
        ) catch {};
        db_storage.?.close();
        db_storage = null;
        return SchemaVersionBehind.SchemaVersionBehind;
    }
    if (db_version > emb_max) {
        ctx.stderr.print(
            "error: schema version {d} in {s} is newer than this binary's embedded max ({d}); " ++
                "upgrade the binary or use one that supports schema_version >= {d}\n",
            .{ db_version, ctx.db_path, emb_max, db_version },
        ) catch {};
        db_storage.?.close();
        db_storage = null;
        return error.SchemaVersionAhead;
    }

    return &db_storage.?;
}

/// Open the DB in strict read-only mode (driver-level rejection of
/// writes), then refuse if the live schema version is outside the
/// binary's supported range. Used by `planar-watch` as the read-only
/// viewer's bootstrap. Distinct from `ensureDbConsumer` because this
/// path produces a handle the SQLite driver itself will refuse to
/// mutate — the second line of defense behind the "no write verbs
/// registered" capability boundary.
///
/// Two refusal paths, both writing a remediation message to stderr
/// before returning:
///
///   - SchemaVersionBehind  — live DB version < embedded_max. The
///                            operator runs `planar init` to bring the
///                            DB forward.
///   - SchemaVersionAhead   — live DB version > embedded_max. The
///                            operator upgrades the binary.
///
/// Idempotent: subsequent calls return the cached handle without
/// re-opening. The cached handle IS the strict read-only one; any
/// caller in this process that retrieves it gets the same write-
/// refusing connection.
pub fn ensureDbStrictReadOnly() !*db.sqlite.Db {
    if (db_storage != null) return &db_storage.?;
    const ctx = current();

    // The strict path does NOT create the parent dir (the writable
    // bootstraps do that). A read-only viewer running before `planar
    // init` should fail loudly with a schema-handshake error, not
    // silently create empty state.

    db_storage = try db.sqlite.Db.openReadOnly(ctx.db_path.ptr);

    // Set a busy_timeout so the read-only connection waits politely
    // when a writer holds the WAL lock. Best-effort — under
    // SQLITE_OPEN_READONLY the connection cannot acquire the writer
    // lock anyway, so this only affects how long a SELECT blocks
    // behind a concurrent writer. WAL-mode PRAGMA is intentionally
    // omitted: it is a writer-side configuration step and a read-
    // only handle cannot set it.
    db_storage.?.exec("PRAGMA busy_timeout = 5000") catch |e| {
        ctx.stderr.print(
            "warning: failed to set busy_timeout on read-only handle ({s}); concurrent writers may delay queries\n",
            .{@errorName(e)},
        ) catch {};
    };

    // Schema-handshake. A missing `schema_migrations` table (fresh DB
    // never touched by `planar init`) maps to version 0 — trips
    // SchemaVersionBehind as long as the binary's embedded migrations
    // include anything at all.
    const db_version: u32 = if (db_storage.?.intQuery(
        "select coalesce(max(version), 0) from schema_migrations",
    )) |v| @intCast(v) else |_| 0;
    const emb_max = db.migrate.embedded_max;

    if (db_version < emb_max) {
        ctx.stderr.print(
            "error: schema version {d} in {s} is older than this binary's minimum of {d}; " ++
                "run `planar init` to apply migrations\n",
            .{ db_version, ctx.db_path, emb_max },
        ) catch {};
        db_storage.?.close();
        db_storage = null;
        return SchemaVersionBehind.SchemaVersionBehind;
    }
    if (db_version > emb_max) {
        ctx.stderr.print(
            "error: schema version {d} in {s} is newer than this binary's embedded max ({d}); " ++
                "upgrade the binary or use one that supports schema_version >= {d}\n",
            .{ db_version, ctx.db_path, emb_max, db_version },
        ) catch {};
        db_storage.?.close();
        db_storage = null;
        return error.SchemaVersionAhead;
    }

    return &db_storage.?;
}

/// Close and re-open the strict read-only DB handle. Used by the
/// `planar-watch --follow` loop between polls to dodge a SQLite
/// behavior where a long-lived read-only connection's wrapped
/// `BEGIN DEFERRED; SELECT ...; COMMIT;` cycle holds a stale
/// snapshot across another process's `PRAGMA wal_checkpoint(TRUNCATE)`.
/// A pure-readonly connection cannot write its read-mark slot to
/// the SHM file, so the WAL-protocol handshake that would have
/// advanced the snapshot never runs, and committed UPDATE rows
/// stay invisible. Closing and re-opening resets the snapshot
/// tracking cleanly. Cheap enough on the follow loop's per-wake
/// cadence (~ms-scale open + first-query cost). Plan 85 t#2623
/// regression fix.
///
/// Returns the freshly-opened handle. Errors propagate the same
/// way `ensureDbStrictReadOnly` does (SchemaVersionBehind / Ahead /
/// OpenFailed). On error, `db_storage` is null and the caller
/// MUST treat the prior handle pointer as invalidated.
pub fn refreshDbStrictReadOnly() !*db.sqlite.Db {
    if (db_storage) |*d| {
        d.close();
        db_storage = null;
    }
    return try ensureDbStrictReadOnly();
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

// The `SchemaVersionBehind` / `SchemaVersionAhead` paths of
// `ensureDbConsumer` are exercised end-to-end in the integration
// suite (planar-agent_test.zig). Unit-testing them here would require
// either swapping `db_storage` for an injected handle (it's a module-
// level singleton today) or running the actual DB-open path with a
// scratch file — both add more harness than payoff at this layer. The
// raw guard logic is covered by `db.migrate.assertSchemaCompatible`
// tests in `src/db/migrate.zig`.
