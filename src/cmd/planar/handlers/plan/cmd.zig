//! handlers/plan/cmd.zig — `planar plan {create, show, list, update, link, recompute-status,
//!   edit, view, step {add, list, done, skip, link}}`

const cli = @import("cli");

const create = @import("create.zig");
const show = @import("show.zig");
const list = @import("list.zig");
const update = @import("update.zig");
const edit = @import("edit.zig");
const view = @import("view.zig");
const diff = @import("diff.zig");
const review = @import("review.zig");
const link = @import("link.zig");
const recompute_status = @import("recompute_status.zig");
const step = @import("step/cmd.zig");

pub const verb: cli.Cmd = .{
    .name = "plan",
    .desc = "Manage plans and plan steps.",
    .cmds = &.{
        .{
            .name = "create",
            .desc = "Create a new plan.",
            .flags = &.{
                .{ .long = "--summary", .kind = .string },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string, .default = .{ .string = "draft" } },
                .{ .long = "--parent", .kind = .int },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "title", .kind = .string, .required = true }},
            .run = cli.handler(create.handle),
        },
        .{
            .name = "show",
            .desc = "Show a plan's details, steps, and child plans.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "list",
            .desc = "List plans.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--parent", .kind = .int },
                .{ .long = "--touches", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "update",
            .desc = "Update mutable fields on a plan.",
            .flags = &.{
                .{ .long = "--title", .kind = .string },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--summary", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--parent", .kind = .int },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(update.handle),
        },
        .{
            .name = "edit",
            .desc = "Edit a plan in $EDITOR (editor-first flow).",
            .flags = &.{
                .{ .long = "--no-pull", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(edit.handle),
        },
        .{
            .name = "view",
            .desc = "View a plan's workbench file.",
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(view.handle),
        },
        .{
            .name = "diff",
            .desc = "Diff plan against database version.",
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(diff.handle),
        },
        .{
            .name = "review",
            .desc = "Reviewer entry point for plan diff.",
            .flags = &.{
                .{ .long = "--approve", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--request-changes", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(review.handle),
        },
        .{
            .name = "link",
            .desc = "Create an entity link from a plan to another entity.",
            .flags = &.{
                .{ .long = "--relationship", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "plan-id", .kind = .string, .required = true },
                .{ .name = "ref", .kind = .string, .required = true },
            },
            .run = cli.handler(link.handle),
        },
        .{
            .name = "recompute-status",
            .desc = "Recompute a plan's roll-up status (--plan <id> or --all).",
            .flags = &.{
                .{ .long = "--plan", .kind = .int, .desc = "Recompute one plan by id." },
                .{ .long = "--all", .kind = .bool, .default = .{ .bool = false }, .desc = "Recompute every plan in the DB." },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(recompute_status.handle),
        },
        step.verb,
    },
};
