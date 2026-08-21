//! handlers/report.zig — `planar report [--days <n>] [--tail <n>] [--json]`
//!
//! Builds and renders the diagnostic bundle from `engine/introspect.zig`.
//!
//! Flags:
//!   --days <n>  Window in days (default 30, must be > 0).
//!   --tail <n>  Failure-tail row count (default 20, must be > 0).
//!   --json      Emit stable machine-readable JSON.
//!
//! Exit codes:
//!   0  bundle rendered (or logging disabled — both are success paths).
//!   2  invalid flag value: --days 0, --days -5, --days abc, --tail 0, etc.
//!   1  database error.
//!
//! Privacy: all queries are structurally redacted in `engine/introspect.zig`.
//! The handler never touches entity text columns.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const db = @import("db");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");
const config_path_mod = @import("config/path.zig");

pub const verb: cli.Cmd = .{
    .name = "report",
    .desc = "Emit the diagnostic bundle: invocation and closed claim-failure aggregates plus health metrics.",
    .long_desc =
    \\Reads the cli_invocations capture log and the always-on observability
    \\tables (agent_actions, sync_events, agent_work_claims, handoffs) and
    \\renders a diagnostic bundle.
    \\
    \\Invocation and failure sections render "logging disabled" when
    \\[introspection].cli_log is off; the always-on sections (actions, sync,
    \\claims, claim failure categories, handoffs, health) render normally in
    \\either case.
    \\
    \\The bundle is structurally redacted: queries select only counts,
    \\categories, verb paths, statuses, and timestamps — never entity text.
    \\
    \\Exit codes:
    \\  0   bundle rendered successfully.
    \\  2   invalid flag value (--days or --tail must be a positive integer).
    \\  1   database error.
    ,
    .flags = &.{
        .{
            .long = "--days",
            .kind = .int,
            .default = .{ .int = 30 },
            .desc = "Window in days (must be > 0, default 30).",
        },
        .{
            .long = "--tail",
            .kind = .int,
            .default = .{ .int = 20 },
            .desc = "Number of failure-tail rows (must be > 0, default 20).",
        },
        .{
            .long = "--json",
            .kind = .bool,
            .default = .{ .bool = false },
            .desc = "Emit stable machine-readable JSON.",
        },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"report"}, args_ptr);
    const ctx = runtime.current();

    // Validate --days: must be a positive integer.
    if (args.days <= 0) {
        ctx.stderr.print(
            "error: --days must be a positive integer (got {d})\n",
            .{args.days},
        ) catch {};
        runtime.shutdown();
        std.process.exit(2);
    }

    // Validate --tail: must be a positive integer.
    if (args.tail <= 0) {
        ctx.stderr.print(
            "error: --tail must be a positive integer (got {d})\n",
            .{args.tail},
        ) catch {};
        runtime.shutdown();
        std.process.exit(2);
    }

    const d = runtime.ensureDb() catch |e|
        exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});

    var resolved = resolveConfig(ctx) orelse exit.die(ctx, error.InvalidConfig, "resolving report config", .{});
    defer resolved.deinit(ctx.allocator);
    const logging_enabled = resolved.config.introspection.cli_log;

    var bundle = engine.introspect.build(
        d,
        ctx.allocator,
        args.days,
        args.tail,
        logging_enabled,
        ctx.db_path,
    ) catch |e| exit.die(ctx, e, "building report: {s}", .{@errorName(e)});
    defer bundle.deinit(ctx.allocator);

    var cli_context = CliContext{ .db = d, .window_days = args.days };
    const home = ctx.environ.getPosix("HOME") orelse "";
    bundle.preview = engine.introspection_adapters.collectConfiguredPreview(
        ctx.allocator,
        home,
        resolved.config.introspection.transcripts,
        .{ .context = @ptrCast(&cli_context), .enabled = logging_enabled, .read = readCliPreview },
        .{},
    ) catch |e| exit.die(ctx, e, "collecting introspection preview: {s}", .{@errorName(e)});

    if (args.json) {
        engine.introspect.renderJson(bundle, ctx.stdout) catch |e|
            exit.die(ctx, e, "rendering JSON: {s}", .{@errorName(e)});
    } else {
        engine.introspect.renderText(bundle, ctx.stdout) catch |e|
            exit.die(ctx, e, "rendering text: {s}", .{@errorName(e)});
    }
}

/// Resolve whether [introspection].cli_log is enabled in config.toml.
/// Fail-open: if the config can't be read, returns false (no logging).
fn resolveConfig(ctx: *const runtime.Ctx) ?engine.config.Resolved {
    const cfg_path = config_path_mod.resolveConfigPath(ctx.allocator, ctx.environ) catch return null;
    defer ctx.allocator.free(cfg_path);

    const file_content: ?[]u8 = std.Io.Dir.cwd().readFileAlloc(
        ctx.io,
        cfg_path,
        ctx.allocator,
        .unlimited,
    ) catch |e| switch (e) {
        error.FileNotFound => null,
        else => null,
    };
    defer if (file_content) |fc| ctx.allocator.free(fc);

    return engine.config.resolve(ctx.allocator, file_content, ctx.environ, null) catch null;
}

const CliContext = struct { db: *db.sqlite.Db, window_days: i64 };

fn readCliPreview(raw: *anyopaque, allocator: std.mem.Allocator, max_bytes: usize) anyerror!?[]u8 {
    const context: *CliContext = @ptrCast(@alignCast(raw));
    return engine.introspect.cliPreviewJsonl(context.db, allocator, context.window_days, max_bytes);
}
