//! handlers/workspace/routing/cmd.zig — `planar workspace routing {build, show}`

const cli = @import("cli");

const build = @import("build.zig");
const show = @import("show.zig");

pub const verb: cli.Cmd = .{
    .name = "routing",
    .desc = "Manage workspace routing table.",
    .cmds = &.{
        .{
            .name = "build",
            .desc = "Build routing table from workspace membership.",
            .flags = &.{
                .{ .long = "--enrich", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "workspace", .kind = .string, .required = false }},
            .run = cli.handler(build.handle),
        },
        .{
            .name = "show",
            .desc = "Display current routing table.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "workspace", .kind = .string, .required = false }},
            .run = cli.handler(show.handle),
        },
    },
};
