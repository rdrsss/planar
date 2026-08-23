//! cmd/planar/cli_log — opt-in CLI invocation capture hook.
//!
//! Records one `cli_invocations` row per top-level `planar` invocation
//! when `[introspection].cli_log` is true in config.toml. The hook runs
//! on the exit path (called from `exit.die` and from `main` after the
//! verb returns), wraps all errors, and is completely fail-open: any
//! failure to record leaves the command's stdout, stderr, and exit code
//! byte-identical to the logging-off run.
//!
//! Privacy invariant (load-bearing):
//!   `parseArgs` serializes flag NAMES and positional ARITY only.
//!   Argument and flag VALUES are NEVER written to `cli_invocations`.
//!
//! Retention pruning piggybacks on the capture write: SQLite date
//! arithmetic decides whether anything has expired; if so, expired rows
//! are deleted before the INSERT. No last-prune bookkeeping is stored
//! anywhere.

const std = @import("std");
const runtime = @import("runtime");
const engine = @import("engine");
const db_mod = @import("db");
const config_path_mod = @import("handlers/config/path.zig");
const exit_mod = @import("exit.zig");

// =========================================================================
// Module-scoped start timestamp
// =========================================================================

/// Wall-clock (best-effort) nanosecond timestamp captured at process start.
/// Set once by `setStartNs` (called from main.zig immediately after
/// runtime.init). Both the success path (main.zig) and the death path
/// (exit.die) read this to compute duration_ms. Zero means "not yet set
/// or capture failed"; recordInner treats 0 as "omit duration".
var start_ns_global: i128 = 0;

/// Record the process start timestamp. Call exactly once, as early as
/// possible in main (after runtime.init so that stderr is available, but
/// before any verb work). Thread-safe in practice: Planar is
/// single-threaded, and this write happens before any concurrent code.
pub fn setStartNs(ns: i128) void {
    start_ns_global = ns;
}

/// Return the stored start timestamp (for use in exit.die).
pub fn startNs() i128 {
    return start_ns_global;
}

// =========================================================================
// Public types
// =========================================================================

/// The error category enum mirrors the CHECK constraint in the migration.
pub const ErrorCategory = enum {
    usage,
    scope,
    not_found,
    conflict,
    validation,
    io,
    db,
    internal,

    pub fn asStr(self: ErrorCategory) []const u8 {
        return switch (self) {
            .usage => "usage",
            .scope => "scope",
            .not_found => "not_found",
            .conflict => "conflict",
            .validation => "validation",
            .io => "io",
            .db => "db",
            .internal => "internal",
        };
    }
};

/// Map a domain error to the error category written to cli_invocations.
/// Mirrors the exit-code mapping in exit.zig: the same classification
/// logic, expressed as an error category string.
pub fn categoryFor(err: anyerror) ?ErrorCategory {
    return switch (err) {
        error.InvalidEntityRef, error.InvalidInput => .usage,
        error.ScopeMismatch => .scope,
        // NotFound maps to exit 1 (generic) per parity triage §F-exit-code-not-found;
        // classified separately in the category enum.
        error.NotFound => .not_found,
        error.Conflict => .conflict,
        error.SlugConflict, error.AlreadyExists => .conflict,
        error.SchemaVersionAhead => .internal,
        error.NotImplemented => .internal,
        else => blk: {
            // Fall back to exit-code bucket for unmapped errors.
            const code = exit_mod.codeFor(err);
            break :blk switch (code) {
                0 => null,
                2 => .usage,
                3 => .conflict,
                5 => .scope,
                6 => .conflict,
                7 => .internal,
                64 => .internal,
                else => .internal,
            };
        },
    };
}

// =========================================================================
// Args-shape normalization
// =========================================================================

/// The privacy-safe summary of one invocation's arguments.
/// Caller owns both string slices (free via `allocator`).
pub const ParsedArgs = struct {
    /// Subcommand chain before the first flag or positional, e.g. "task add".
    verb_path: []const u8,
    /// Flag names + positional arity, e.g. "<pos:1> --plan --json".
    /// Values are NEVER present here.
    args_shape: []const u8,
};

/// Maximum number of subcommand tokens collected into verb_path.
/// Planar's CLI has at most two levels of subcommands (e.g. "task add",
/// "workbench push"). Tokens beyond this limit are positional arguments
/// even if they look like bare words.
const max_verb_depth: usize = 2;

/// Build the privacy-safe ParsedArgs from the argv slice *after* the
/// binary name (i.e. ctx.argv[1..]).
///
/// Rules (load-bearing privacy invariant):
///   - The first up to `max_verb_depth` non-flag tokens are collected
///     into verb_path (subcommand chain).
///   - Flag tokens (leading `--` or short `-X`) are collected by NAME
///     only; the following non-flag value token is consumed and DISCARDED.
///   - Bare non-flag tokens beyond `max_verb_depth`, or any token after
///     the first flag, increment positional_count; values are DISCARDED.
///   - A bare `--` separator ends everything; all following tokens are
///     positionals (values discarded).
pub fn parseArgs(allocator: std.mem.Allocator, argv: []const []const u8) std.mem.Allocator.Error!ParsedArgs {
    var verb_parts: std.ArrayList([]const u8) = .empty;
    defer verb_parts.deinit(allocator);

    var shape_parts: std.ArrayList([]const u8) = .empty;
    defer shape_parts.deinit(allocator);

    var positional_count: usize = 0;
    // Once we see any flag or the -- separator, verb collection stops.
    var past_verbs = false;

    var i: usize = 0;
    while (i < argv.len) : (i += 1) {
        const tok = argv[i];

        if (std.mem.eql(u8, tok, "--")) {
            // Everything after -- is positionals; values discarded.
            past_verbs = true;
            i += 1;
            while (i < argv.len) : (i += 1) {
                positional_count += 1;
            }
            break;
        }

        const is_long_flag = std.mem.startsWith(u8, tok, "--");
        const is_short_flag = tok.len >= 2 and tok[0] == '-' and tok[1] != '-';

        if (is_long_flag or is_short_flag) {
            past_verbs = true;
            // Record the flag name only.
            // For the --flag=value inline form (single token), strip
            // everything from '=' onward so only the flag name is
            // recorded.  Value-free invariant: the value is discarded.
            if (is_long_flag) {
                const eq_pos = std.mem.indexOfScalar(u8, tok, '=');
                if (eq_pos) |pos| {
                    // Inline value: append only the flag-name portion.
                    try shape_parts.append(allocator, tok[0..pos]);
                    // The value is embedded; no following token to consume.
                    continue;
                }
            }
            // For short flags, canonicalize to the leading `-X` only.
            // A short-flag attached value takes two forms:
            //   -pVALUE  (tok.len > 2, tok[1] != '-', value starts at tok[2])
            //   -p=VALUE (tok.len > 2, tok[2] == '=', value starts at tok[3])
            // In both cases we record only tok[0..2] (the `-X` pair) and
            // discard the rest. No planar flag currently defines .short, but
            // this closure is structural — not conditional on active usage.
            if (is_short_flag and tok.len > 2) {
                try shape_parts.append(allocator, tok[0..2]);
                // Value is embedded in the token; no following token consumed.
                continue;
            }
            try shape_parts.append(allocator, tok);
            // Consume the next token as the flag value if it doesn't look
            // like a flag itself.  Value-free invariant: the value is
            // discarded.
            if (i + 1 < argv.len) {
                const next = argv[i + 1];
                const next_is_flag = std.mem.startsWith(u8, next, "-");
                if (!next_is_flag) {
                    i += 1; // consume and discard the value
                }
            }
        } else if (!past_verbs and verb_parts.items.len < max_verb_depth) {
            // Subcommand token: within the verb depth limit and before any flag.
            try verb_parts.append(allocator, tok);
        } else {
            // Positional argument (beyond max_verb_depth, or after the first flag).
            past_verbs = true;
            positional_count += 1;
        }
    }

    // Build verb_path: "task add"
    const verb_path = try std.mem.join(allocator, " ", verb_parts.items);
    errdefer allocator.free(verb_path);

    // Build args_shape: "<pos:N> --flag1 --flag2"
    var shape_buf: std.ArrayList(u8) = .empty;
    errdefer shape_buf.deinit(allocator);

    if (positional_count > 0) {
        const pos_str = try std.fmt.allocPrint(allocator, "<pos:{d}>", .{positional_count});
        defer allocator.free(pos_str);
        try shape_buf.appendSlice(allocator, pos_str);
    }

    for (shape_parts.items) |flag| {
        if (shape_buf.items.len > 0) try shape_buf.append(allocator, ' ');
        try shape_buf.appendSlice(allocator, flag);
    }

    const args_shape = try shape_buf.toOwnedSlice(allocator);

    return ParsedArgs{ .verb_path = verb_path, .args_shape = args_shape };
}

// =========================================================================
// Main capture entry point
// =========================================================================

/// Record one cli_invocations row. Fail-open: any error is swallowed.
/// Must be called AFTER the command has completed so exit_code is final.
///
/// `exit_code`: the exit code the process will use (0 = success).
/// `err`: the domain error that caused the failure, null on success.
/// `start_ns`: wall-clock (best-effort) nanosecond timestamp at process start,
///   used to compute duration_ms. Pass 0 to omit duration.
/// Whether the resolved database file already exists.
///
/// Telemetry attaches to an existing install; it never brings one into being.
fn dbFileExists(ctx: anytype) bool {
    const path: []const u8 = ctx.db_path;
    if (path.len == 0) return false;
    std.Io.Dir.cwd().access(ctx.io, path, .{}) catch return false;
    return true;
}

pub fn record(exit_code: u8, err: ?anyerror, start_ns: i128) void {
    recordInner(exit_code, err, start_ns) catch {};
}

fn recordInner(exit_code: u8, err: ?anyerror, start_ns: i128) !void {
    const ctx = runtime.current();

    // Resolve config path and read the file (best-effort; fail open).
    const cfg_path = config_path_mod.resolveConfigPath(ctx.allocator, ctx.environ) catch return;
    defer ctx.allocator.free(cfg_path);

    const file_content: ?[]u8 = std.Io.Dir.cwd().readFileAlloc(
        ctx.io,
        cfg_path,
        ctx.allocator,
        .unlimited,
    ) catch |e| switch (e) {
        error.FileNotFound => null,
        else => null, // fail open
    };
    defer if (file_content) |fc| ctx.allocator.free(fc);

    var resolved = engine.config.resolve(ctx.allocator, file_content, ctx.environ, null) catch return;
    defer resolved.deinit(ctx.allocator);

    if (!resolved.config.introspection.cli_log) return; // logging off

    // Acquire the DB WITHOUT applying migrations, and fail open if that is
    // not possible.
    //
    // This must never be `ensureDb`. Telemetry runs on EVERY invocation,
    // including `--help`, `schema`, and `version`, which touch no database of
    // their own — and `ensureDb` migrates on open. That combination silently
    // advanced an operator's live database to a dev build's schema (twice),
    // breaking every other installed binary with SchemaVersionAhead until the
    // migration was rolled back by hand. Because this path is `catch return`,
    // the damage was invisible: help printed normally while the migration
    // landed.
    //
    // `record` only runs post-dispatch, so a verb that genuinely needs the
    // database has already opened and migrated it through `ensureDb` by now;
    // this call then returns that same cached handle. When no such verb ran,
    // a database needing migration simply goes unlogged — the correct
    // trade for telemetry, which must not have schema side effects.
    // Never CREATE a database either. Opening one brings it into existence,
    // so a bare `--help` against a fresh machine would leave an empty database
    // behind purely as a side effect of telemetry. Logging an invocation is
    // not a reason to create the thing being logged about; if no database
    // exists yet, there is nothing worth recording against.
    if (!dbFileExists(ctx)) return;

    const db = runtime.ensureDbConsumerQuiet() catch return;

    // Parse verb_path and args_shape from process argv.
    const argv_tail: []const []const u8 = if (ctx.argv.len > 1) ctx.argv[1..] else &.{};
    const parsed = parseArgs(ctx.allocator, argv_tail) catch return;
    defer ctx.allocator.free(parsed.verb_path);
    defer ctx.allocator.free(parsed.args_shape);

    // Determine error category (null for successful invocations).
    const cat: ?ErrorCategory = if (exit_code != 0) blk: {
        break :blk if (err) |e| categoryFor(e) else .internal;
    } else null;

    // Duration in milliseconds (null if start_ns was 0).
    const duration_ms_val: ?i64 = if (start_ns != 0) blk: {
        const now_ns = nowNanos();
        if (now_ns == 0) break :blk null;
        const elapsed_ns = now_ns - start_ns;
        const ms: i64 = @intCast(@max(0, elapsed_ns) / std.time.ns_per_ms);
        break :blk ms;
    } else null;

    const retention_days = resolved.config.introspection.retention_days;

    // Stateless retention prune (fail open).
    pruneIfNeeded(db, ctx.allocator, retention_days) catch {};

    // INSERT the invocation row. `recorded_at` is set via SQLite's
    // strftime so we never need to format a timestamp in Zig.
    const cat_param: db_mod.sqlite.Param = if (cat) |c|
        .{ .text = c.asStr() }
    else
        .{ .null = {} };

    const dur_param: db_mod.sqlite.Param = if (duration_ms_val) |d|
        .{ .int = d }
    else
        .{ .null = {} };

    const sql: [:0]const u8 =
        \\insert into cli_invocations
        \\  (verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at)
        \\values (?, ?, ?, ?, null, ?, strftime('%Y-%m-%dT%H:%M:%fZ','now'))
    ;

    _ = db.execParams(sql, &.{
        .{ .text = parsed.verb_path },
        .{ .text = parsed.args_shape },
        .{ .int = @as(i64, exit_code) },
        cat_param,
        dur_param,
    }) catch {};
}

/// Stateless retention prune: delete rows older than `retention_days`.
/// Uses SQLite date arithmetic — no last-prune bookkeeping anywhere.
/// Fail-open: the caller catches and discards any error.
fn pruneIfNeeded(
    db: *db_mod.sqlite.Db,
    allocator: std.mem.Allocator,
    retention_days: i64,
) !void {
    // First check cheaply (indexed) whether anything is expired.
    const check_str = try std.fmt.allocPrint(
        allocator,
        "select count(*) from cli_invocations where recorded_at < date('now', '-{d} days')",
        .{retention_days},
    );
    defer allocator.free(check_str);
    const check_sql = try allocator.dupeZ(u8, check_str);
    defer allocator.free(check_sql);

    const expired_count = db.intQuery(check_sql) catch return;
    if (expired_count == 0) return; // nothing to prune

    const prune_str = try std.fmt.allocPrint(
        allocator,
        "delete from cli_invocations where recorded_at < date('now', '-{d} days')",
        .{retention_days},
    );
    defer allocator.free(prune_str);

    try db.execSlice(allocator, prune_str);
}

/// Read a wall-clock (best-effort) nanosecond timestamp.
/// Uses clock_gettime(REALTIME) via the C layer (there is no
/// std.time.nanoTimestamp in this Zig version). Returns 0 on failure.
/// Private form used inside this module for the end-of-invocation read.
fn nowNanos() i128 {
    return nowNanosPublic();
}

/// Public form so main.zig can capture the start timestamp at startup.
/// Identical implementation to nowNanos; kept separate so callers can
/// import it without pulling in the full capture machinery.
/// Uses clock_gettime(REALTIME) — wall-clock (best-effort), not monotonic.
pub fn nowNanosPublic() i128 {
    var ts: std.c.timespec = undefined;
    if (std.c.clock_gettime(.REALTIME, &ts) != 0) return 0;
    return @as(i128, @intCast(ts.sec)) * std.time.ns_per_s +
        @as(i128, @intCast(ts.nsec));
}

// =========================================================================
// Unit tests
// =========================================================================

test "categoryFor: maps domain errors to categories" {
    const t = std.testing;
    try t.expectEqual(ErrorCategory.usage, categoryFor(error.InvalidInput).?);
    try t.expectEqual(ErrorCategory.usage, categoryFor(error.InvalidEntityRef).?);
    try t.expectEqual(ErrorCategory.scope, categoryFor(error.ScopeMismatch).?);
    try t.expectEqual(ErrorCategory.not_found, categoryFor(error.NotFound).?);
    try t.expectEqual(ErrorCategory.conflict, categoryFor(error.Conflict).?);
    try t.expectEqual(ErrorCategory.conflict, categoryFor(error.SlugConflict).?);
    try t.expectEqual(ErrorCategory.conflict, categoryFor(error.AlreadyExists).?);
    try t.expectEqual(ErrorCategory.internal, categoryFor(error.NotImplemented).?);
    try t.expectEqual(ErrorCategory.internal, categoryFor(error.SchemaVersionAhead).?);
}

test "parseArgs: simple verb + positional + flags" {
    const a = std.testing.allocator;
    const argv = [_][]const u8{ "task", "add", "my-title", "--plan", "42", "--json" };
    const parsed = try parseArgs(a, &argv);
    defer a.free(parsed.verb_path);
    defer a.free(parsed.args_shape);

    try std.testing.expectEqualStrings("task add", parsed.verb_path);
    // One positional (my-title), two flag names (--plan and --json).
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "<pos:1>") != null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--plan") != null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--json") != null);
    // Values must NOT appear.
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "42") == null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "my-title") == null);
}

test "parseArgs: no args — verb only" {
    const a = std.testing.allocator;
    const argv = [_][]const u8{"health"};
    const parsed = try parseArgs(a, &argv);
    defer a.free(parsed.verb_path);
    defer a.free(parsed.args_shape);

    try std.testing.expectEqualStrings("health", parsed.verb_path);
    try std.testing.expectEqualStrings("", parsed.args_shape);
}

test "parseArgs: hostile flag values — values never in shape" {
    const a = std.testing.allocator;
    // Flag values containing spaces (separate tokens), quotes, and a
    // --prefixed string (which would look like a flag but is the VALUE
    // of --plan and is consumed+discarded).
    const argv = [_][]const u8{ "task", "add", "--title", "hello world", "--body", "\"quoted\"", "--plan", "123" };
    const parsed = try parseArgs(a, &argv);
    defer a.free(parsed.verb_path);
    defer a.free(parsed.args_shape);

    try std.testing.expectEqualStrings("task add", parsed.verb_path);
    // Values must NOT appear.
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "hello") == null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "world") == null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "quoted") == null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "123") == null);
    // Flag names must appear.
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--title") != null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--body") != null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--plan") != null);
}

test "parseArgs: multibyte flag value — value never in shape" {
    const a = std.testing.allocator;
    const argv = [_][]const u8{ "artifact", "add", "--title", "日本語タイトル", "--json" };
    const parsed = try parseArgs(a, &argv);
    defer a.free(parsed.verb_path);
    defer a.free(parsed.args_shape);

    try std.testing.expectEqualStrings("artifact add", parsed.verb_path);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "日本語") == null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--title") != null);
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--json") != null);
}

test "parseArgs: inline flag value (--flag=value) never leaks value into shape" {
    const a = std.testing.allocator;
    // etcli-zig accepts --flag=value as a single token (inline_value form).
    // The value after = must NOT appear in args_shape; only the flag name
    // up to (and including) = should be recorded as a marker.
    const sentinel = "SENTINEL_MUST_NOT_LEAK";
    const argv = [_][]const u8{ "task", "add", "--plan=SENTINEL_MUST_NOT_LEAK", "--json" };
    const parsed = try parseArgs(a, &argv);
    defer a.free(parsed.verb_path);
    defer a.free(parsed.args_shape);

    try std.testing.expectEqualStrings("task add", parsed.verb_path);
    // Sentinel value must NOT appear in args_shape.
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, sentinel) == null);
    // Flag name must appear.
    try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--plan") != null);
}

test "parseArgs: short-flag attached value (-pVALUE and -p=VALUE) do not leak value" {
    // No planar flag currently uses .short, but the structural closure is
    // load-bearing for the privacy invariant: even if a short flag with an
    // attached value arrives, the value must never appear in args_shape.
    const a = std.testing.allocator;
    const sentinel = "SENTINEL_MUST_NOT_LEAK";

    // -pSENTINEL_MUST_NOT_LEAK form (attached without =)
    {
        const tok = "-p" ++ sentinel;
        const argv = [_][]const u8{ "task", "add", tok };
        const parsed = try parseArgs(a, &argv);
        defer a.free(parsed.verb_path);
        defer a.free(parsed.args_shape);

        try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, sentinel) == null);
        // The short flag name itself ("-p") IS recorded.
        try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "-p") != null);
    }

    // -p=SENTINEL_MUST_NOT_LEAK form (attached with =)
    {
        const tok = "-p=" ++ sentinel;
        const argv2 = [_][]const u8{ "task", "add", tok };
        const parsed2 = try parseArgs(a, &argv2);
        defer a.free(parsed2.verb_path);
        defer a.free(parsed2.args_shape);

        try std.testing.expect(std.mem.indexOf(u8, parsed2.args_shape, sentinel) == null);
        try std.testing.expect(std.mem.indexOf(u8, parsed2.args_shape, "-p") != null);
    }
}

test "parseArgs: positional count zero, one, many" {
    const a = std.testing.allocator;

    // Zero positionals (flags only).
    {
        const argv = [_][]const u8{ "plan", "list", "--json" };
        const parsed = try parseArgs(a, &argv);
        defer a.free(parsed.verb_path);
        defer a.free(parsed.args_shape);
        try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "<pos:") == null);
        try std.testing.expect(std.mem.indexOf(u8, parsed.args_shape, "--json") != null);
    }

    // One positional.
    {
        const argv2 = [_][]const u8{ "plan", "show", "my-plan" };
        const parsed2 = try parseArgs(a, &argv2);
        defer a.free(parsed2.verb_path);
        defer a.free(parsed2.args_shape);
        try std.testing.expect(std.mem.indexOf(u8, parsed2.args_shape, "<pos:1>") != null);
    }

    // Many positionals (after --).
    {
        const argv3 = [_][]const u8{ "task", "add", "--", "first", "second", "third" };
        const parsed3 = try parseArgs(a, &argv3);
        defer a.free(parsed3.verb_path);
        defer a.free(parsed3.args_shape);
        try std.testing.expect(std.mem.indexOf(u8, parsed3.args_shape, "<pos:3>") != null);
    }
}
