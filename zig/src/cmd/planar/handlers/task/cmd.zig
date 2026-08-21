//! handlers/task/cmd.zig — `planar task {add, show, list, update, edit, view, diff, review,
//!   done, cancel, block, link, reopen, touches {add, remove}}`

const cli = @import("cli");

const add = @import("add.zig");
const show = @import("show.zig");
const list = @import("list.zig");
const update = @import("update.zig");
const edit = @import("edit.zig");
const view = @import("view.zig");
const diff = @import("diff.zig");
const review = @import("review.zig");
const done = @import("done.zig");
const cancel = @import("cancel.zig");
const block = @import("block.zig");
const link = @import("link.zig");
const reopen = @import("reopen.zig");
const touches = @import("touches/cmd.zig");
const packet = @import("packet.zig");

pub const verb: cli.Cmd = .{
    .name = "task",
    .desc = "Manage tasks.",
    .long_desc = "Manage tasks — the discrete units of work.\n\n  Tasks may belong to a plan (--plan) or another task (--parent), and\n  carry the next_action field required by resume validate.\n  Status lifecycle: todo → doing → done / cancelled; blocked is set\n  via task block.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Create a new task.",
            .flags = &.{
                .{ .long = "--body", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--next-action", .kind = .string },
                .{ .long = "--due", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--parent", .kind = .int },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--priority", .kind = .int, .default = .{ .int = 100 } },
                .{ .long = "--editor", .kind = .bool, .default = .{ .bool = true } },
                .{ .long = "--no-auto-promote", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "title", .kind = .string, .required = true }},
            .run = cli.handler(add.handle),
        },
        .{
            .name = "show",
            .desc = "Show full task details.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "packet",
            .desc = "Compile the authoritative current routing packet for a task.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(packet.handle),
        },
        .{
            .name = "list",
            .desc = "List tasks.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--priority-max", .kind = .int },
                .{ .long = "--touches", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "update",
            .desc = "Update mutable fields on a task.",
            .flags = &.{
                .{ .long = "--title", .kind = .string },
                .{ .long = "--body", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--next-action", .kind = .string },
                .{ .long = "--due", .kind = .string },
                .{ .long = "--priority", .kind = .int },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--reason", .kind = .string },
                .{ .long = "--no-auto-promote", .kind = .bool, .default = .{ .bool = false } },
                // --editor is a no-op on `task update` (the editor-driven path is `task edit`).
                // Accepted here so scripts that pass `--editor=false` alongside other flags
                // do not get UnknownFlag. The value is intentionally unused by update.handle.
                .{ .long = "--editor", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(update.handle),
        },
        .{
            .name = "edit",
            .desc = "Edit a task in $EDITOR (editor-first flow).",
            .flags = &.{
                .{ .long = "--no-pull", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(edit.handle),
        },
        .{
            .name = "view",
            .desc = "View task's workbench file.",
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(view.handle),
        },
        .{
            .name = "diff",
            .desc = "Diff task against its database-stored version.",
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(diff.handle),
        },
        .{
            .name = "review",
            .desc = "Reviewer entry point for task diff.",
            .flags = &.{
                .{ .long = "--approve", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--request-changes", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(review.handle),
        },
        .{
            .name = "done",
            .desc = "Mark a task as done (single-arg form; Go supports variadic).",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false }, .desc = "Override active-claim guard and flip status anyway." },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(done.handle),
        },
        .{
            .name = "cancel",
            .desc = "Cancel a task (single-arg form; Go supports variadic).",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(cancel.handle),
        },
        .{
            .name = "block",
            .desc = "Mark a task as blocked and record the blocking relationship.",
            .flags = &.{
                .{ .long = "--on", .kind = .int, .required = true, .desc = "Blocking task id" },
                .{ .long = "--reason", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false }, .desc = "Override active-claim guard and flip status anyway." },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(block.handle),
        },
        .{
            .name = "link",
            .desc = "Create an entity link from a task to another entity.",
            .flags = &.{
                .{ .long = "--relationship", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "task-id", .kind = .string, .required = true },
                .{ .name = "ref", .kind = .string, .required = true },
            },
            .run = cli.handler(link.handle),
        },
        .{
            .name = "reopen",
            .desc = "Reopen a done or cancelled task with an audit-trail entry.",
            .flags = &.{
                .{ .long = "--status", .kind = .string },
                .{ .long = "--reason", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false }, .desc = "Override active-claim guard and flip status anyway." },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(reopen.handle),
        },
        touches.verb,
    },
};
