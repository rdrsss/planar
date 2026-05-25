//! handlers/artifact/add — `planar artifact add <title> --kind <k> [--body|--from-file --source-path --status --scope --plan --editor --json]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const editor = @import("../../editor.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "artifact", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const kind = engine.planning.artifact.Kind.fromText(args.kind) orelse
        exit.die(ctx, error.InvalidInput, "unknown kind '{s}'", .{args.kind});

    const status = engine.planning.artifact.Status.fromText(args.status) orelse
        exit.die(ctx, error.InvalidInput, "unknown status '{s}'", .{args.status});

    // --body and --from-file are mutually exclusive (mirrors Go).
    if (args.body != null and args.from_file != null) {
        exit.die(ctx, error.InvalidInput, "--body and --from-file are mutually exclusive", .{});
    }

    // Resolve body and source_path. --body supports @path prefix; --from-file
    // reads a file and also sets source-path unless --source-path is given.
    var body_owned: ?[]u8 = null;
    defer if (body_owned) |b| ctx.allocator.free(b);
    var body_value: ?[]const u8 = null;
    const stdout_is_tty = std.Io.File.stdout().isTty(ctx.io) catch false;

    var source_path: ?[]const u8 = args.source_path;

    if (args.body) |b| {
        body_owned = engine.planning.artifact.readBody(ctx.allocator, ctx.io, b) catch |e|
            exit.die(ctx, error.InvalidInput, "read --body: {s}", .{@errorName(e)});
        body_value = body_owned;
    } else if (args.from_file) |path| {
        body_owned = readFile(ctx, path) catch |e|
            exit.die(ctx, error.InvalidInput, "read --from-file {s}: {s}", .{ path, @errorName(e) });
        body_value = body_owned;
        if (source_path == null) source_path = path;
    } else if (args.editor and stdout_is_tty) {
        const res = editor.invoke(ctx.io, ctx.allocator, "", .{}) catch |e|
            exit.die(ctx, e, "opening editor failed: {s}", .{@errorName(e)});
        defer res.deinit(ctx.allocator);
        if (res.editor_exit_code != 0) {
            exit.die(ctx, error.InvalidInput, "editor exited with code {d}", .{res.editor_exit_code});
        }
        body_owned = try ctx.allocator.dupe(u8, res.content);
        body_value = body_owned;
    }

    const art = engine.planning.artifact.create(d, ctx.allocator, .{
        .title = args.title,
        .kind = kind,
        .body = body_value,
        .source_path = source_path,
        .status = status,
        .plan_id = args.plan,
        .scope = args.scope,
    }) catch |e| exit.die(ctx, e, "artifact add: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.planning.artifact, art, .{ .json = args.json });
}

fn readFile(ctx: *const runtime.Ctx, path: []const u8) ![]u8 {
    return try std.Io.Dir.cwd().readFileAlloc(ctx.io, path, ctx.allocator, .unlimited);
}
