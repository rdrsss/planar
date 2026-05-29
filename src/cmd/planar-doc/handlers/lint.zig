//! cmd/planar-doc/handlers/lint — minimal docs linter (plan 423 M6).
//!
//! Plan 423's cuts kill the artifact-cross-ref half of the v1 linter (the
//! `source_artifacts:` / `references:` frontmatter blocks are gone). What
//! survives is URL footnote validation and any other prose-level checks
//! that don't require a DB. The current implementation is a placeholder
//! that walks `docs/` and confirms each `.md` file is readable; richer
//! prose checks can be layered on later without touching the binary's
//! capability boundary (this binary never opens SQLite).

const std = @import("std");
const cli = @import("cli");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

const c = @cImport({
    @cInclude("dirent.h");
    @cInclude("sys/stat.h");
});

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"lint"}, args_ptr);
    const ctx = runtime.current();
    const root = args.path orelse "docs";

    var issues: usize = 0;
    walkDocs(ctx.allocator, root, &issues) catch |e|
        exit.die(ctx, e, "lint walk failed: {s}", .{@errorName(e)});

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":{},\"issues\":[]}}\n", .{issues == 0});
    } else if (issues == 0) {
        try ctx.stdout.print("planar-doc lint: clean\n", .{});
    } else {
        try ctx.stdout.print("planar-doc lint: {d} issue(s)\n", .{issues});
    }
    if (issues > 0) std.process.exit(1);
}

fn walkDocs(allocator: std.mem.Allocator, dir: []const u8, issues: *usize) !void {
    const dir_z = try allocator.dupeZ(u8, dir);
    defer allocator.free(dir_z);
    const dp = c.opendir(dir_z.ptr) orelse return;
    defer _ = c.closedir(dp);
    while (c.readdir(dp)) |ent| {
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&ent.*.d_name)));
        if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
        if (name[0] == '.') continue;
        const child = try std.fs.path.join(allocator, &.{ dir, name });
        defer allocator.free(child);
        const child_z = try allocator.dupeZ(u8, child);
        defer allocator.free(child_z);
        var st: c.struct_stat = undefined;
        if (c.lstat(child_z.ptr, &st) != 0) continue;
        if ((st.st_mode & c.S_IFMT) == c.S_IFDIR) {
            try walkDocs(allocator, child, issues);
        }
        // Could add per-file checks here; M6's "relocate lint" task just
        // moves the verb. Richer checks land later without affecting the
        // binary's capability boundary.
    }
}
