//! handlers/decision/cmd.zig — `planar decision {add, show, list, accept, supersede, withdraw,
//!   edit, view, diff, link}`

const cli = @import("cli");

const add = @import("add.zig");
const show = @import("show.zig");
const list = @import("list.zig");
const accept = @import("accept.zig");
const supersede = @import("superseded.zig");
const withdraw = @import("withdraw.zig");
const edit = @import("edit.zig");
const view = @import("view.zig");
const diff = @import("diff.zig");
const review = @import("review.zig");
const link = @import("link.zig");

pub const verb: cli.Cmd = .{
    .name = "decision",
    .desc = "Manage decision records.",
    .long_desc = "Manage decision records — rationale for choices made during work.\n\n  Status lifecycle: proposed → accepted / superseded / withdrawn.\n  Terminal statuses: superseded, withdrawn.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Create a new decision record.",
            .flags = &.{
                .{ .long = "--body", .kind = .string },
                .{ .long = "--rationale", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--editor", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "title", .kind = .string, .required = true }},
            .run = cli.handler(add.handle),
        },
        .{
            .name = "show",
            .desc = "Show a decision's details.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "list",
            .desc = "List decisions.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "accept",
            .desc = "Accept a proposed decision.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(accept.handle),
        },
        .{
            .name = "supersede",
            .desc = "Mark a decision as superseded by a newer decision.",
            .flags = &.{
                .{ .long = "--by", .kind = .int, .required = true },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(supersede.handle),
        },
        .{
            .name = "withdraw",
            .desc = "Withdraw a decision.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(withdraw.handle),
        },
        .{
            .name = "edit",
            .desc = "Edit a decision in $EDITOR (editor-first flow).",
            .flags = &.{
                .{ .long = "--no-pull", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(edit.handle),
        },
        .{
            .name = "view",
            .desc = "View decision's workbench file.",
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(view.handle),
        },
        .{
            .name = "diff",
            .desc = "Diff decision against database version.",
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(diff.handle),
        },
        .{
            .name = "review",
            .desc = "Reviewer entry point for decision diff.",
            .flags = &.{
                .{ .long = "--approve", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--request-changes", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(review.handle),
        },
        .{
            .name = "link",
            .desc = "Create an entity link from a decision to another entity.",
            .flags = &.{
                .{ .long = "--relationship", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "decision-id", .kind = .string, .required = true },
                .{ .name = "ref", .kind = .string, .required = true },
            },
            .run = cli.handler(link.handle),
        },
    },
};
