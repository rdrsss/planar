//! handlers/ext/register/github — `planar ext register github <slug> --project <owner/repo> [--auth-env <var>]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");

const RegisterJSON = struct {
    ok: bool,
    id: i64,
    slug: []const u8,
    kind: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "ext", "register", "github" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const sys = engine.external.system.registerGithub(d, ctx.allocator, .{
        .slug = args.slug,
        .project = args.project,
        .auth_env = args.auth_env,
    }) catch |e| switch (e) {
        error.SlugExists => exit.die(ctx, error.SlugConflict, "external system '{s}' already registered", .{args.slug}),
        else => exit.die(ctx, e, "ext register github: {s}", .{@errorName(e)}),
    };
    defer engine.external.system.deinit(sys, ctx.allocator);

    if (args.json) {
        const out = RegisterJSON{
            .ok = true,
            .id = sys.id,
            .slug = sys.slug,
            .kind = sys.kind.toText(),
        };
        try std.json.Stringify.value(out, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("registered {s}  [github-issues, id:{d}]\n", .{ sys.slug, sys.id });
    }
}
