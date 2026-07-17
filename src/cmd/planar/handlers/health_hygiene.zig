//! handlers/health_hygiene.zig — `planar health hygiene`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "hygiene",
    .desc = "Report stale plan, task, and question lifecycle state without mutating it.",
    .long_desc = "Find draft plans with zero tasks or only terminal tasks, tasks\n  left doing beyond a threshold, and questions left open beyond a\n  threshold. Suggested repair commands are reported but never run.\n\n  This reporter always exits 0 when the report is produced, even when\n  findings are present.",
    .flags = &.{
        .{ .long = "--scope", .kind = .string, .desc = "Limit findings to one association slug" },
        .{ .long = "--stale-doing", .kind = .int, .default = .{ .int = 7 }, .desc = "Doing-task age threshold in days" },
        .{ .long = "--stale-open", .kind = .int, .default = .{ .int = 30 }, .desc = "Open-question age threshold in days" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "health", "hygiene" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const report = engine.health.hygiene(d, ctx.allocator, .{
        .scope = args.scope,
        .stale_doing_days = args.stale_doing,
        .stale_open_days = args.stale_open,
    }) catch |err| switch (err) {
        error.InvalidThreshold => exit.die(ctx, err, "stale thresholds must be non-negative", .{}),
        error.UnsupportedScope => exit.die(ctx, err, "--scope must name one association", .{}),
        error.SlugNotFound => exit.die(ctx, err, "scope slug not found", .{}),
        else => exit.die(ctx, err, "health hygiene failed: {s}", .{@errorName(err)}),
    };
    defer report.deinit(ctx.allocator);
    if (args.json) {
        try ctx.stdout.print("{f}\n", .{std.json.fmt(report, .{})});
    } else {
        try engine.health.renderHygieneText(report, ctx.stdout);
    }
}
