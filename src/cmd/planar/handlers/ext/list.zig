//! handlers/ext/list — `planar ext list [--json]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "ext", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const systems = engine.external.system.list(d, ctx.allocator) catch |e|
        exit.die(ctx, e, "ext list: {s}", .{@errorName(e)});
    defer engine.external.system.deinitMany(systems, ctx.allocator);

    if (args.json) {
        // One JSON object per line (mirrors Go's ObjectLine).
        for (systems) |s| {
            try ctx.stdout.print("{{\"id\":{d},\"kind\":", .{s.id});
            try output.writeJsonString(ctx.stdout, s.kind.toText());
            try ctx.stdout.print(",\"slug\":", .{});
            try output.writeJsonString(ctx.stdout, s.slug);
            if (s.base_url) |u| {
                try ctx.stdout.print(",\"base_url\":", .{});
                try output.writeJsonString(ctx.stdout, u);
            }
            if (s.default_project) |p| {
                try ctx.stdout.print(",\"default_project\":", .{});
                try output.writeJsonString(ctx.stdout, p);
            }
            try ctx.stdout.print(",\"auth_method\":", .{});
            try output.writeJsonString(ctx.stdout, s.auth_method.toText());
            try ctx.stdout.print(",\"created_at\":", .{});
            try output.writeJsonString(ctx.stdout, s.created_at);
            try ctx.stdout.print("}}\n", .{});
        }
        return;
    }

    if (systems.len == 0) {
        try ctx.stdout.print("no external systems registered\n", .{});
        return;
    }
    try ctx.stdout.print("{s:<20}  {s:<16}  {s:<36}  {s}\n", .{ "slug", "kind", "base-url", "project" });
    for (systems) |s| {
        const base = s.base_url orelse "";
        const project = s.default_project orelse "";
        try ctx.stdout.print("{s:<20}  {s:<16}  {s:<36}  {s}\n", .{ s.slug, s.kind.toText(), base, project });
    }
}
