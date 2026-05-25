//! engine/templates/init — extract embedded baseline templates to disk.
//!
//! Mirrors Go `internal/templates/init.go`. Writes every embedded template
//! to `<root>/default/<system>/<kind>.json` unless that file already exists.
//! Idempotent — existing files are never overwritten.

const std = @import("std");
const embed = @import("templates_embed");

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

/// initOnDisk extracts the embedded baseline templates beneath
/// `<root>/default/<system>/<kind>.json`. Returns the list of files that
/// were newly written. The caller owns each `[]const u8` in the returned
/// slice and the slice itself (free both via `freeCreatedList`).
pub fn initOnDisk(allocator: std.mem.Allocator, root: []const u8) ![]const []const u8 {
    if (root.len == 0) return error.InvalidInput;

    const io = fsIo();
    var created: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (created.items) |p| allocator.free(p);
        created.deinit(allocator);
    }

    // Ensure <root>/default/ exists.
    const default_root = try std.fs.path.join(allocator, &.{ root, "default" });
    defer allocator.free(default_root);
    try std.Io.Dir.cwd().createDirPath(io, default_root);

    for (embed.all) |entry| {
        const sys_dir = try std.fs.path.join(allocator, &.{ default_root, entry.system });
        defer allocator.free(sys_dir);
        try std.Io.Dir.cwd().createDirPath(io, sys_dir);

        const filename = try std.fmt.allocPrint(allocator, "{s}.json", .{entry.kind});
        defer allocator.free(filename);
        const full_path = try std.fs.path.join(allocator, &.{ sys_dir, filename });

        // Skip if it already exists.
        const exists = blk: {
            std.Io.Dir.cwd().access(io, full_path, .{}) catch break :blk false;
            break :blk true;
        };
        if (exists) {
            allocator.free(full_path);
            continue;
        }

        try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = full_path, .data = entry.body });
        try created.append(allocator, full_path);
    }

    return try created.toOwnedSlice(allocator);
}

/// freeCreatedList releases every allocation returned from `initOnDisk`.
pub fn freeCreatedList(list: []const []const u8, allocator: std.mem.Allocator) void {
    for (list) |p| allocator.free(p);
    allocator.free(list);
}

test "initOnDisk writes embedded templates idempotently" {
    const a = std.testing.allocator;
    const io = fsIo();
    // Tests run single-threaded under zig test; a fixed scratch path is fine
    // because we delete the tree before the test exits.
    const root = try a.dupe(u8, "/tmp/planar-tmpl-init-test");
    _ = std.Io.Dir.cwd().deleteTree(io, root) catch {};
    defer a.free(root);
    defer std.Io.Dir.cwd().deleteTree(io, root) catch {};

    const first = try initOnDisk(a, root);
    defer freeCreatedList(first, a);
    try std.testing.expect(first.len >= 3);

    const second = try initOnDisk(a, root);
    defer freeCreatedList(second, a);
    try std.testing.expectEqual(@as(usize, 0), second.len);
}
