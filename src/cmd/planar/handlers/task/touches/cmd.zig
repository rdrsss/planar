//! handlers/task/touches/cmd.zig — `planar task touches {add, remove}`

const cli = @import("cli");

const add = @import("add.zig");
const remove = @import("remove.zig");

pub const verb: cli.Cmd = .{
    .name = "touches",
    .desc = "Manage repo-touches links on a task.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Link a task to a repo via a 'touches' relationship.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "task-id", .kind = .string, .required = true },
                .{ .name = "repo-slug", .kind = .string, .required = true },
            },
            .run = cli.handler(add.handle),
        },
        .{
            .name = "remove",
            .desc = "Remove a 'touches' link between a task and a repo.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "task-id", .kind = .string, .required = true },
                .{ .name = "repo-slug", .kind = .string, .required = true },
            },
            .run = cli.handler(remove.handle),
        },
    },
};
