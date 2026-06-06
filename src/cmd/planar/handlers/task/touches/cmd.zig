//! handlers/task/touches/cmd.zig — `planar task touches {add, remove}`

const cli = @import("cli");

const add = @import("add.zig");
const remove = @import("remove.zig");
const list = @import("list.zig");

pub const verb: cli.Cmd = .{
    .name = "touches",
    .desc = "Manage repo-touches links on a task.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Link a task to a repo via a 'touches' relationship.",
            .long_desc = "Declare that a task touches a repo (and, with --path, a specific file).\n\n  Without --path: writes the repo-level entity_links 'touches' edge\n  (task -> repo). This is the coarse signal used by `task list --touches`.\n\n  With --path <p>: writes a path-level task_touch_paths row (task, repo,\n  path) AND the repo-level edge — a path-touch implies the repo-touch, so\n  the repo-level signal stays consistent. <p> is a repo-relative file path.\n  The parallelizability rules (`plan recommend-strategy`) read these\n  path-level declarations for rules 2/3/4 (disjoint touches, migration\n  touched, singleton file touched). Declare path touches per file (repeat\n  the verb), not as a list.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
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
            .name = "list",
            .desc = "List the repo- and path-level touches declared on a task.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "task-id", .kind = .string, .required = true },
            },
            .run = cli.handler(list.handle),
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
