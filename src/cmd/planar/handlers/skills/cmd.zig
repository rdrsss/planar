//! handlers/skills/cmd.zig — `planar skills render [slug...]`

const cli = @import("cli");
const render = @import("render.zig");

pub const verb: cli.Cmd = .{
    .name = "skills",
    .desc = "Render unified skill sources into per-vendor output trees.",
    .cmds = &.{
        .{
            .name = "render",
            .desc = "Render unified skill sources into per-vendor output trees.",
            .flags = &.{
                .{ .long = "--src", .kind = .string, .default = .{ .string = "skills/src" } },
                .{ .long = "--out", .kind = .string, .default = .{ .string = "." } },
                .{ .long = "--check", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--diff", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "slug", .kind = .string, .required = false }},
            .allow_extra_positionals = true,
            .run = cli.handler(render.handle),
        },
    },
};
