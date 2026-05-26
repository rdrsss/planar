//! handlers/question/cmd.zig — `planar question {add, edit, view, diff, review, answer, wontfix, list, show, link}`

const cli = @import("cli");

const add = @import("add.zig");
const edit = @import("edit.zig");
const view = @import("view.zig");
const diff = @import("diff.zig");
const review = @import("review.zig");
const answer = @import("answer.zig");
const wontfix = @import("wontfix.zig");
const list = @import("list.zig");
const show = @import("show.zig");
const link = @import("link.zig");

pub const verb: cli.Cmd = .{
    .name = "question",
    .desc = "Manage questions.",
    .long_desc = "Manage questions — open uncertainties surfaced during work.\n\n  Status lifecycle: open → answered (via 'question answer') / wontfix.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Create a new question.",
            .flags = &.{
                .{ .long = "--body", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--editor", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "title", .kind = .string, .required = true }},
            .run = cli.handler(add.handle),
        },
        .{
            .name = "edit",
            .desc = "Edit a question in $EDITOR (editor-first flow).",
            .flags = &.{
                .{ .long = "--no-pull", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "question-id", .kind = .string, .required = true }},
            .run = cli.handler(edit.handle),
        },
        .{
            .name = "view",
            .desc = "View question's workbench file.",
            .positionals = &.{.{ .name = "question-id", .kind = .string, .required = true }},
            .run = cli.handler(view.handle),
        },
        .{
            .name = "diff",
            .desc = "Diff question against database version.",
            .positionals = &.{.{ .name = "question-id", .kind = .string, .required = true }},
            .run = cli.handler(diff.handle),
        },
        .{
            .name = "review",
            .desc = "Reviewer entry point for question diff.",
            .flags = &.{
                .{ .long = "--approve", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--request-changes", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "question-id", .kind = .string, .required = true }},
            .run = cli.handler(review.handle),
        },
        .{
            .name = "answer",
            .desc = "Record an answer to a question.",
            .flags = &.{
                .{ .long = "--answer", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "question-id", .kind = .string, .required = true }},
            .run = cli.handler(answer.handle),
        },
        .{
            .name = "wontfix",
            .desc = "Mark a question as wontfix.",
            .flags = &.{
                .{ .long = "--reason", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "question-id", .kind = .string, .required = true }},
            .run = cli.handler(wontfix.handle),
        },
        .{
            .name = "list",
            .desc = "List questions.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--touches", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "show",
            .desc = "Show a question's details.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "question-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "link",
            .desc = "Create an entity link from a question to another entity.",
            .flags = &.{
                .{ .long = "--relationship", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "question-id", .kind = .string, .required = true },
                .{ .name = "ref", .kind = .string, .required = true },
            },
            .run = cli.handler(link.handle),
        },
    },
};
