//! handlers/sync/cmd.zig — `planar sync {pull, push, status, resolve}`

const cli = @import("cli");

const pull = @import("pull.zig");
const push = @import("push.zig");
const status = @import("status.zig");
const resolve = @import("resolve.zig");

pub const verb: cli.Cmd = .{
    .name = "sync",
    .desc = "Pull and push data between the local plane and external systems.",
    .cmds = &.{
        .{
            .name = "pull",
            .desc = "Pull remote state for one or more external links.",
            .flags = &.{
                .{ .long = "--all", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--system", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "ref", .kind = .string, .required = false, .desc = "<link-id | kind:id>" }},
            .run = cli.handler(pull.handle),
        },
        .{
            .name = "push",
            .desc = "Push local changes for one or more external links.",
            .flags = &.{
                .{ .long = "--all", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--system", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "ref", .kind = .string, .required = false, .desc = "<link-id | kind:id>" }},
            .run = cli.handler(push.handle),
        },
        .{
            .name = "status",
            .desc = "Report sync status for links.",
            .flags = &.{
                .{ .long = "--entity", .kind = .string, .desc = "Filter by entity, e.g. task:42" },
                .{ .long = "--system", .kind = .string, .desc = "Filter by system slug" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(status.handle),
        },
        .{
            .name = "resolve",
            .desc = "Settle a sync conflict on a link.",
            .flags = &.{
                .{ .long = "--keep", .kind = .string, .required = true, .desc = "Which side to keep (local|remote)" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "event-id", .kind = .string, .required = true }},
            .run = cli.handler(resolve.handle),
        },
    },
};
