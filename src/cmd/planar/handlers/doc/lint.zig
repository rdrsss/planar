const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "doc", "lint" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch null;
    const root = if (args.path) |p| p else engine.docs.manifest.default_docs_root;

    var issues: std.ArrayList(engine.docs.lint.Issue) = .empty;
    errdefer {
        for (issues.items) |issue| engine.docs.lint.deinitIssue(issue, ctx.allocator);
        issues.deinit(ctx.allocator);
    }
    try walk(root, d, ctx.allocator, .{ .no_urls = args.no_refs, .refs_only = args.refs_only }, &issues);

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":{},\"issues\":[", .{issues.items.len == 0});
        for (issues.items, 0..) |issue, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"type\":", .{});
            try output.writeJsonString(ctx.stdout, issue.type.toText());
            try ctx.stdout.print(",\"path\":", .{});
            try output.writeJsonString(ctx.stdout, issue.path);
            try ctx.stdout.print(",\"detail\":", .{});
            try output.writeJsonString(ctx.stdout, issue.detail);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
    } else if (issues.items.len == 0) {
        try ctx.stdout.print("doc lint: clean\n", .{});
    } else {
        for (issues.items) |issue| {
            try ctx.stdout.print("{s}: {s}: {s}\n", .{ issue.path, issue.type.toText(), issue.detail });
        }
        try ctx.stdout.print("doc lint: {d} issue(s)\n", .{issues.items.len});
    }
    if (issues.items.len > 0) exit.die(ctx, error.InvalidInput, "doc lint: {d} issue(s)", .{issues.items.len});
    for (issues.items) |issue| engine.docs.lint.deinitIssue(issue, ctx.allocator);
    issues.deinit(ctx.allocator);
}

fn walk(
    root: []const u8,
    d: ?*db.sqlite.Db,
    allocator: std.mem.Allocator,
    opts: engine.docs.lint.Options,
    out: *std.ArrayList(engine.docs.lint.Issue),
) !void {
    var dir = try std.Io.Dir.cwd().openDir(runtime.current().io, root, .{ .iterate = true });
    defer dir.close(runtime.current().io);
    var it = dir.iterate();
    while (try it.next(runtime.current().io)) |entry| {
        const path = try std.fs.path.join(allocator, &.{ root, entry.name });
        defer allocator.free(path);
        switch (entry.kind) {
            .directory => try walk(path, d, allocator, opts, out),
            .file => if (std.ascii.endsWithIgnoreCase(entry.name, ".md")) {
                const found = try engine.docs.lint.validateFile(path, d, allocator, opts);
                defer allocator.free(found);
                for (found) |issue| try out.append(allocator, issue);
            },
            else => {},
        }
    }
}

const db = @import("db");
