//! handlers/workbench/cmd.zig — `planar workbench {pull, push, status, resolve, sync,
//!   archive, restore, list, publish, extract-questions, edit}`

const cli = @import("cli");

const pull = @import("pull.zig");
const push = @import("push.zig");
const status = @import("status.zig");
const resolve = @import("resolve.zig");
const sync = @import("sync.zig");
const archive = @import("archive.zig");
const restore = @import("restore.zig");
const list = @import("list.zig");
const publish = @import("publish.zig");
const extract_questions = @import("extract_questions.zig");
const edit = @import("edit.zig");
const gc = @import("gc.zig");

pub const verb: cli.Cmd = .{
    .name = "workbench",
    .desc = "Manage workbench sync for plan feature directories.",
    .long_desc = "Manage the bidirectional sync surface between the workbench\n  filesystem and the Planar database.\n\n  The workbench root defaults to ~/.planar/workbench/ and can be\n  overridden with the PLANAR_WORKBENCH_ROOT environment variable.",
    .cmds = &.{
        .{
            .name = "pull",
            .desc = "Apply FS→DB changes; report DB→FS drift.",
            .flags = &.{
                .{ .long = "--verbose", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            .run = cli.handler(pull.handle),
        },
        .{
            .name = "push",
            .desc = "Apply DB→FS changes atomically; report FS→DB drift.",
            .flags = &.{
                .{ .long = "--verbose", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--filter-mode", .kind = .string, .desc = "Terminal-status filter: 'failures' (default) or 'all'" },
                .{ .long = "--apply-cleanup", .kind = .bool, .default = .{ .bool = false }, .desc = "Remove pre-existing FS files for entities this push would have filtered" },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            .run = cli.handler(push.handle),
        },
        .{
            .name = "status",
            .desc = "Show drift and conflicts without writing.",
            .flags = &.{
                .{ .long = "--verbose", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = false }},
            .run = cli.handler(status.handle),
        },
        .{
            .name = "resolve",
            .desc = "Settle a sync conflict by choosing FS or DB.",
            .flags = &.{
                .{ .long = "--prefer", .kind = .string, .required = true, .desc = "Which side to prefer (fs|db)" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "event-id", .kind = .string, .required = true }},
            .run = cli.handler(resolve.handle),
        },
        .{
            .name = "sync",
            .desc = "Atomically apply FS and DB changes via a unified sync.",
            .flags = &.{
                .{ .long = "--verbose", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            .run = cli.handler(sync.handle),
        },
        .{
            .name = "archive",
            .desc = "Archive a feature's workbench filesystem tree.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--filter-mode", .kind = .string, .desc = "Terminal-status filter: 'failures' (default) or 'all'" },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            .run = cli.handler(archive.handle),
        },
        .{
            .name = "restore",
            .desc = "Restore an archived feature's workbench tree.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--filter-mode", .kind = .string, .desc = "Terminal-status filter: 'failures' (default) or 'all'" },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            .run = cli.handler(restore.handle),
        },
        .{
            .name = "gc",
            .desc = "Remove FS files whose backing entity is terminal in the DB.",
            .flags = &.{
                .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false }, .desc = "Preview only; do not touch disk" },
                .{ .long = "--yes", .kind = .bool, .default = .{ .bool = false }, .desc = "Discard FS-content drift; remove drifted files anyway" },
                .{ .long = "--filter-mode", .kind = .string, .desc = "Terminal-status filter: 'failures' (default) or 'all'" },
                .{ .long = "--all-scopes", .kind = .bool, .default = .{ .bool = false }, .desc = "Walk every plan's workbench tree" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = false }},
            .run = cli.handler(gc.handle),
        },
        .{
            .name = "list",
            .desc = "List features with workbench trees.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(list.handle),
        },
        .{
            .name = "publish",
            .desc = "Render and push workbench files to external system.",
            .flags = &.{
                .{ .long = "--system", .kind = .string, .required = true },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            .run = cli.handler(publish.handle),
        },
        .{
            .name = "extract-questions",
            .desc = "Parse Open questions from top-level workbench specs (read-only).",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            .run = cli.handler(extract_questions.handle),
        },
        .{
            .name = "edit",
            .desc = "Edit a feature's workbench files in $EDITOR.",
            .flags = &.{
                .{ .long = "--editor", .kind = .string, .default = .{ .string = "" } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            .run = cli.handler(edit.handle),
        },
    },
};
