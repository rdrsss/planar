//! handlers/plan/step/cmd.zig — `planar plan step {add, list, done, skip, link}`

const cli = @import("cli");

const add = @import("add.zig");
const list = @import("list.zig");
const done = @import("done.zig");
const skip = @import("skip.zig");
const link = @import("link.zig");

pub const verb: cli.Cmd = .{
    .name = "step",
    .desc = "Manage plan steps.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Append a new step to a plan.",
            .flags = &.{
                .{ .long = "--after", .kind = .int },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "plan-id", .kind = .string, .required = true },
                .{ .name = "body", .kind = .string, .required = true },
            },
            .run = cli.handler(add.handle),
        },
        .{
            .name = "list",
            .desc = "List steps of a plan.",
            .flags = &.{ .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } } },
            .positionals = &.{ .{ .name = "plan-id", .kind = .string, .required = true } },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "done",
            .desc = "Mark a plan step as done.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{ .{ .name = "step-id", .kind = .string, .required = true } },
            .run = cli.handler(done.handle),
        },
        .{
            .name = "skip",
            .desc = "Mark a plan step as skipped.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{ .{ .name = "step-id", .kind = .string, .required = true } },
            .run = cli.handler(skip.handle),
        },
        .{
            .name = "link",
            .desc = "Associate a plan step with the task that materializes it.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "step-id", .kind = .string, .required = true },
                .{ .name = "task-id", .kind = .string, .required = true },
            },
            .run = cli.handler(link.handle),
        },
    },
};
