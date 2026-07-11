//! DB-free Markdown reference linter for planar-doc.

const std = @import("std");
const cli = @import("cli");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

const c = @cImport({
    @cInclude("dirent.h");
    @cInclude("sys/stat.h");
});

const Issue = struct {
    path: []const u8,
    type: []const u8,
    detail: []const u8,

    fn deinit(self: Issue, allocator: std.mem.Allocator) void {
        allocator.free(self.path);
        allocator.free(self.detail);
    }
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"lint"}, args_ptr);
    const ctx = runtime.current();
    const root = args.path orelse "docs";
    var issues: std.ArrayList(Issue) = .empty;
    defer {
        for (issues.items) |issue| issue.deinit(ctx.allocator);
        issues.deinit(ctx.allocator);
    }

    walkDocs(ctx.allocator, ctx.io, root, &issues) catch |e|
        exit.die(ctx, e, "lint walk failed for '{s}': {s}", .{ root, @errorName(e) });

    if (args.json) {
        try std.json.Stringify.value(.{ .ok = issues.items.len == 0, .issues = issues.items }, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else if (issues.items.len == 0) {
        try ctx.stdout.print("planar-doc lint: clean\n", .{});
    } else {
        for (issues.items) |issue| try ctx.stdout.print("{s}: {s}: {s}\n", .{ issue.path, issue.type, issue.detail });
        try ctx.stdout.print("planar-doc lint: {d} issue(s)\n", .{issues.items.len});
    }
    if (issues.items.len > 0) {
        runtime.flush() catch {};
        std.process.exit(1);
    }
}

fn walkDocs(allocator: std.mem.Allocator, io: std.Io, dir: []const u8, issues: *std.ArrayList(Issue)) !void {
    const dir_z = try allocator.dupeZ(u8, dir);
    defer allocator.free(dir_z);
    const dp = c.opendir(dir_z.ptr) orelse return error.OpenFailed;
    defer _ = c.closedir(dp);
    while (c.readdir(dp)) |ent| {
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&ent.*.d_name)));
        if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..") or name[0] == '.') continue;
        const child = try std.fs.path.join(allocator, &.{ dir, name });
        defer allocator.free(child);
        const child_z = try allocator.dupeZ(u8, child);
        defer allocator.free(child_z);
        var st: c.struct_stat = undefined;
        if (c.lstat(child_z.ptr, &st) != 0) return error.StatFailed;
        if ((st.st_mode & c.S_IFMT) == c.S_IFDIR) {
            try walkDocs(allocator, io, child, issues);
        } else if ((st.st_mode & c.S_IFMT) == c.S_IFREG and std.mem.endsWith(u8, name, ".md")) {
            const content = try std.Io.Dir.cwd().readFileAlloc(io, child, allocator, .limited(16 * 1024 * 1024));
            defer allocator.free(content);
            try lintContent(allocator, child, content, issues);
        }
    }
}

fn lintContent(allocator: std.mem.Allocator, path: []const u8, content: []const u8, issues: *std.ArrayList(Issue)) !void {
    var declared: std.StringHashMapUnmanaged(void) = .empty;
    defer declared.deinit(allocator);
    var lines = std.mem.splitScalar(u8, content, '\n');
    while (lines.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t\r");
        if (!std.mem.startsWith(u8, trimmed, "[^")) continue;
        const close = std.mem.indexOf(u8, trimmed, "]:") orelse continue;
        const id = trimmed[2..close];
        if (id.len == 0) continue;
        try declared.put(allocator, id, {});
        const target = std.mem.trim(u8, trimmed[close + 2 ..], " \t");
        if (target.len > 0 and !std.mem.startsWith(u8, target, "http://") and !std.mem.startsWith(u8, target, "https://")) {
            try appendIssue(allocator, issues, path, "invalid-url", target);
        }
    }

    var cursor: usize = 0;
    while (std.mem.indexOfPos(u8, content, cursor, "[^")) |start| {
        const close = std.mem.indexOfScalarPos(u8, content, start + 2, ']') orelse break;
        cursor = close + 1;
        if (cursor < content.len and content[cursor] == ':') continue;
        const id = content[start + 2 .. close];
        if (id.len > 0 and !declared.contains(id)) try appendIssue(allocator, issues, path, "undeclared-citation", id);
    }
}

fn appendIssue(allocator: std.mem.Allocator, issues: *std.ArrayList(Issue), path: []const u8, kind: []const u8, detail: []const u8) !void {
    try issues.append(allocator, .{
        .path = try allocator.dupe(u8, path),
        .type = kind,
        .detail = try allocator.dupe(u8, detail),
    });
}
