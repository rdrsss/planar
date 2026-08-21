//! handlers/capture/commits — `planar capture commits [--session <id>] [--repo <dir>] (--since <ref> | <sha>...)`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "capture", "commits" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    if (args.since != null and args.shas.len > 0) {
        exit.die(ctx, error.InvalidInput, "cannot combine --since with explicit commit SHAs", .{});
    }
    if (args.since == null and args.shas.len == 0) {
        exit.die(ctx, error.InvalidInput, "provide --since <ref> or one or more commit SHAs", .{});
    }

    const session_id = resolveTargetSessionId(ctx, d, args.session);
    const repo_dir = args.repo orelse ".";

    var result = engine.runtime.capture.recordCommits(d, ctx.allocator, ctx.io, .{
        .session_id = session_id,
        .repo_dir = repo_dir,
        .since = args.since,
        .shas = args.shas,
    }) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "session {d} not found", .{session_id}),
        error.NotGit => exit.die(ctx, e, "repo is not a git repository: {s}", .{repo_dir}),
        error.GitFailed => {
            if (args.since) |since| {
                exit.die(ctx, e, "cannot resolve ref '{s}'", .{since});
            }
            exit.die(ctx, e, "one or more commit SHAs could not be resolved", .{});
        },
        else => exit.die(ctx, e, "capture commits: {s}", .{@errorName(e)}),
    };
    defer result.deinit(ctx.allocator);

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"session_id\":{d},\"repo_root\":",
            .{result.session_id},
        );
        try std.json.Stringify.encodeJsonString(result.repo_root, .{}, ctx.stdout);
        try ctx.stdout.print(
            ",\"commit_count\":{d},\"inserted_count\":{d}}}\n",
            .{ result.commit_count, result.inserted_count },
        );
        return;
    }

    try ctx.stdout.print(
        "session {d}: processed {d} commits ({d} new)\n",
        .{ @as(u64, @intCast(result.session_id)), result.commit_count, result.inserted_count },
    );
}

fn resolveTargetSessionId(
    ctx: *const runtime.Ctx,
    d: anytype,
    explicit_session_id: ?i64,
) i64 {
    if (explicit_session_id) |sid| return sid;

    const vendor: []const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR")) |v|
        (if (v.len > 0) @as([]const u8, v) else "cli")
    else
        "cli";
    const vsid: ?[]const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR_SESSION_ID")) |v|
        (if (v.len > 0) @as([]const u8, v) else null)
    else
        null;

    const found = engine.runtime.session.activeForVendor(d, ctx.allocator, vendor, vsid) catch |e|
        exit.die(ctx, e, "finding active session: {s}", .{@errorName(e)});
    if (found) |session| {
        defer engine.runtime.session.deinit(session, ctx.allocator);
        return session.id;
    }

    exit.die(ctx, error.InvalidInput, "no active session (run `planar capture session` first)", .{});
}
