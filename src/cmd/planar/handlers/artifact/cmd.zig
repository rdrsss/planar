//! handlers/artifact/cmd.zig — `planar artifact {add, show, list, update, edit, view, diff, link}`

const cli = @import("cli");

const add = @import("add.zig");
const show = @import("show.zig");
const list = @import("list.zig");
const update = @import("update.zig");
const edit = @import("edit.zig");
const view = @import("view.zig");
const diff = @import("diff.zig");
const review = @import("review.zig");
const link = @import("link.zig");

pub const verb: cli.Cmd = .{
    .name = "artifact",
    .desc = "Manage artifacts (tech specs, ADRs, design notes, etc.).",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Register a new artifact.",
            .flags = &.{
                .{ .long = "--body", .kind = .string },
                .{ .long = "--kind", .kind = .string, .required = true },
                .{ .long = "--from-file", .kind = .string },
                .{ .long = "--source-path", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string, .default = .{ .string = "draft" } },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--editor", .kind = .bool, .default = .{ .bool = true } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "title", .kind = .string, .required = true }},
            .run = cli.handler(add.handle),
        },
        .{
            .name = "show",
            .desc = "Show an artifact's metadata and body.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "artifact-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "list",
            .desc = "List artifacts.",
            .flags = &.{
                .{ .long = "--kind", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "update",
            .desc = "Update mutable fields on an artifact.",
            .flags = &.{
                .{ .long = "--title", .kind = .string },
                .{ .long = "--body", .kind = .string },
                .{ .long = "--source-path", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "artifact-id", .kind = .string, .required = true }},
            .run = cli.handler(update.handle),
        },
        .{
            .name = "edit",
            .desc = "Edit an artifact in $EDITOR (editor-first flow).",
            .flags = &.{
                .{ .long = "--no-pull", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "artifact-id", .kind = .string, .required = true }},
            .run = cli.handler(edit.handle),
        },
        .{
            .name = "view",
            .desc = "View the artifact's workbench file in $PAGER.",
            .positionals = &.{.{ .name = "artifact-id", .kind = .string, .required = true }},
            .run = cli.handler(view.handle),
        },
        .{
            .name = "diff",
            .desc = "Show a unified diff between the DB's artifact content and the workbench file.",
            .positionals = &.{.{ .name = "artifact-id", .kind = .string, .required = true }},
            .run = cli.handler(diff.handle),
        },
        .{
            .name = "review",
            .desc = "Reviewer entry point for artifact diff.",
            .flags = &.{
                .{ .long = "--approve", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--request-changes", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "artifact-id", .kind = .string, .required = true }},
            .run = cli.handler(review.handle),
        },
        .{
            .name = "link",
            .desc = "Create an entity link from an artifact to another entity.",
            .flags = &.{
                .{ .long = "--relationship", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "artifact-id", .kind = .string, .required = true },
                .{ .name = "ref", .kind = .string, .required = true },
            },
            .run = cli.handler(link.handle),
        },
    },
};
