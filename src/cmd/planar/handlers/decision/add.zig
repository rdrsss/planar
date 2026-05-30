//! handlers/decision/add — `planar decision add <title> [--body --rationale --plan --scope --editor]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const editor = @import("../../editor.zig");
const scope_mod = @import("../../scope.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "decision", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // cwd-derive fallback (plan 352 task 2450).
    const resolution = scope_mod.resolve(ctx, args.scope) catch |e|
        exit.die(ctx, e, "decision add: resolving scope failed: {s}", .{@errorName(e)});
    const effective_scope: ?[]const u8 = if (resolution.scope) |s| s else null;

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

    // Resolve the session id for the entity-create activity hook (plan 467
    // Phase 1). Same env-var resolution pattern as question/add.zig.
    const vendor: []const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR")) |v|
        (if (v.len > 0) @as([]const u8, v) else "cli")
    else
        "cli";
    const vsid: ?[]const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR_SESSION_ID")) |v|
        (if (v.len > 0) @as([]const u8, v) else null)
    else
        null;
    const session_id: ?i64 = engine.runtime.session.ensureActive(d, ctx.allocator, vendor, vsid) catch null;

    const dec = engine.planning.decision.create(d, ctx.allocator, .{
        .title = args.title,
        .body = body_required,
        .rationale = args.rationale,
        .plan_id = args.plan,
        .scope = effective_scope,
        .session_id = session_id,
    }) catch |e| exit.die(ctx, e, "decision add: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.planning.decision, dec, .{ .json = args.json });
}
