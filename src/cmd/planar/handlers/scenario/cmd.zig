//! handlers/scenario/cmd.zig — `planar scenario {add, edit, view, diff, verify, retire, list, show, link}`

const cli = @import("cli");

const add = @import("add.zig");
const edit = @import("edit.zig");
const view = @import("view.zig");
const diff = @import("diff.zig");
const review = @import("review.zig");
const verify = @import("verify.zig");
const retire = @import("retire.zig");
const list = @import("list.zig");
const show = @import("show.zig");
const link = @import("link.zig");

pub const verb: cli.Cmd = .{
    .name = "scenario",
    .desc = "Manage test scenarios.",
    .long_desc = "Manage test scenarios — verification artifacts tied to specs,\n  plans, or tasks.\n\n  Planar records scenarios and their outcomes; it does not execute\n  them.\n  Status lifecycle: draft → ready → verified / failing → retired.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Create a new test scenario.",
            .flags = &.{
                .{ .long = "--body", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--related", .kind = .int },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--editor", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "title", .kind = .string, .required = true }},
            .run = cli.handler(add.handle),
        },
        .{
            .name = "edit",
            .desc = "Edit a scenario in $EDITOR (editor-first flow).",
            .flags = &.{
                .{ .long = "--no-pull", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "scenario-id", .kind = .string, .required = true }},
            .run = cli.handler(edit.handle),
        },
        .{
            .name = "view",
            .desc = "View scenario's workbench file.",
            .positionals = &.{.{ .name = "scenario-id", .kind = .string, .required = true }},
            .run = cli.handler(view.handle),
        },
        .{
            .name = "diff",
            .desc = "Diff scenario against database version.",
            .positionals = &.{.{ .name = "scenario-id", .kind = .string, .required = true }},
            .run = cli.handler(diff.handle),
        },
        .{
            .name = "review",
            .desc = "Reviewer entry point for scenario diff.",
            .flags = &.{
                .{ .long = "--approve", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--request-changes", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "scenario-id", .kind = .string, .required = true }},
            .run = cli.handler(review.handle),
        },
        .{
            .name = "verify",
            .desc = "Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass).",
            .flags = &.{
                .{ .long = "--outcome", .kind = .string },
                .{ .long = "--summary", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "scenario-id", .kind = .string, .required = true }},
            .run = cli.handler(verify.handle),
        },
        .{
            .name = "retire",
            .desc = "Mark a scenario as retired.",
            .flags = &.{
                .{ .long = "--reason", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "scenario-id", .kind = .string, .required = true }},
            .run = cli.handler(retire.handle),
        },
        .{
            .name = "list",
            .desc = "List scenarios.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--related", .kind = .int },
                .{ .long = "--touches", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "show",
            .desc = "Show a scenario's details.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "scenario-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "link",
            .desc = "Create an entity link from a scenario to another entity.",
            .flags = &.{
                .{ .long = "--relationship", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "scenario-id", .kind = .string, .required = true },
                .{ .name = "ref", .kind = .string, .required = true },
            },
            .run = cli.handler(link.handle),
        },
    },
};
