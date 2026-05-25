//! handlers/decision/add — `planar decision add <title> [--body --rationale --plan --scope --editor]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const editor = @import("../../editor.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "decision", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    if (args.plan != null) {
        try ctx.stderr.print("warning: --plan accepted but not yet linked (entity_links not wired)\n", .{});
    }

    var body_owned: ?[]u8 = null;
    defer if (body_owned) |b| ctx.allocator.free(b);
    var body: ?[]const u8 = args.body;
    const stdout_is_tty = std.Io.File.stdout().isTty(ctx.io) catch false;
    if (body == null and args.editor and stdout_is_tty) {
        const res = editor.invoke(ctx.io, ctx.allocator, "", .{}) catch |e|
            exit.die(ctx, e, "opening editor failed: {s}", .{@errorName(e)});
        defer res.deinit(ctx.allocator);
        if (res.editor_exit_code != 0) {
            exit.die(ctx, error.InvalidInput, "editor exited with code {d}", .{res.editor_exit_code});
        }
        body_owned = try ctx.allocator.dupe(u8, res.content);
        body = body_owned;
    }
    const body_required = body orelse exit.die(ctx, error.InvalidInput, "--body is required (or run interactively to use the editor flow)", .{});

    const dec = engine.planning.decision.create(d, ctx.allocator, .{
        .title = args.title,
        .body = body_required,
        .rationale = args.rationale,
        .plan_id = args.plan,
        .scope = args.scope,
    }) catch |e| exit.die(ctx, e, "decision add: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.planning.decision, dec, .{ .json = args.json });
}
