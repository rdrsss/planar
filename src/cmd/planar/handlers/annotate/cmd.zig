//! handlers/annotate/cmd.zig — `planar annotate {...}`
//!
//! Annotations are line-anchored notes on source code with a four-state
//! lifecycle (active → resolved / dismissed / archived). Schema lives in
//! migrations/00012_annotations.up.sql; engine module is
//! src/engine/planning/annotation.zig. The handlers under this directory
//! are thin: parse args, call the engine, render output.

const cli = @import("cli");

const add = @import("add.zig");
const show = @import("show.zig");
const list = @import("list.zig");
const update = @import("update.zig");
const remove = @import("remove.zig");
const tag = @import("tag.zig");
const resolve = @import("resolve.zig");
const dismiss = @import("dismiss.zig");
const archive = @import("archive.zig");
const bulk_resolve = @import("bulk_resolve.zig");
const bulk_dismiss = @import("bulk_dismiss.zig");
const bulk_archive = @import("bulk_archive.zig");
const verify = @import("verify.zig");
const sweep = @import("sweep.zig");

pub const verb: cli.Cmd = .{
    .name = "annotate",
    .desc = "Manage source annotations.",
    .long_desc = "Manage line-anchored annotations on source code.\n\n  Status lifecycle: active → resolved / dismissed / archived.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Create a new annotation.",
            .flags = &.{
                .{ .long = "--anchor-path", .kind = .string },
                .{ .long = "--line-start", .kind = .int },
                .{ .long = "--line-end", .kind = .int },
                .{ .long = "--commit-sha", .kind = .string },
                .{ .long = "--text-hash", .kind = .string },
                .{ .long = "--text", .kind = .string },
                .{ .long = "--title", .kind = .string },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--body", .kind = .string },
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--tags", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(add.handle),
        },
        .{
            .name = "show",
            .desc = "Show an annotation.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "annotation-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "list",
            .desc = "List annotations.",
            .flags = &.{
                .{ .long = "--anchor-path", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--tag", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "update",
            .desc = "Update an annotation.",
            .flags = &.{
                .{ .long = "--title", .kind = .string },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--body", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "annotation-id", .kind = .string, .required = true }},
            .run = cli.handler(update.handle),
        },
        .{
            .name = "remove",
            .desc = "Remove an annotation.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "annotation-id", .kind = .string, .required = true }},
            .run = cli.handler(remove.handle),
        },
        .{
            .name = "tag",
            .desc = "Add or remove a tag on an annotation.",
            .flags = &.{
                .{ .long = "--remove", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "annotation-id", .kind = .string, .required = true },
                .{ .name = "tag", .kind = .string, .required = true },
            },
            .run = cli.handler(tag.handle),
        },
        .{
            .name = "resolve",
            .desc = "Mark an annotation as resolved.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "annotation-id", .kind = .string, .required = true }},
            .run = cli.handler(resolve.handle),
        },
        .{
            .name = "dismiss",
            .desc = "Dismiss an annotation.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "annotation-id", .kind = .string, .required = true }},
            .run = cli.handler(dismiss.handle),
        },
        .{
            .name = "archive",
            .desc = "Archive an annotation.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "annotation-id", .kind = .string, .required = true }},
            .run = cli.handler(archive.handle),
        },
        .{
            .name = "bulk-resolve",
            .desc = "Resolve every active annotation matching the filter.",
            .flags = &.{
                .{ .long = "--anchor-path", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--tag", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(bulk_resolve.handle),
        },
        .{
            .name = "bulk-dismiss",
            .desc = "Dismiss every active annotation matching the filter.",
            .flags = &.{
                .{ .long = "--anchor-path", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--tag", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(bulk_dismiss.handle),
        },
        .{
            .name = "bulk-archive",
            .desc = "Archive every annotation matching the filter (including non-active rows).",
            .flags = &.{
                .{ .long = "--anchor-path", .kind = .string },
                .{ .long = "--plan", .kind = .int },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--tag", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(bulk_archive.handle),
        },
        .{
            .name = "verify",
            .desc = "Verify annotation anchors against workspace state.",
            .flags = &.{
                .{ .long = "--anchor-path", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(verify.handle),
        },
        .{
            .name = "sweep",
            .desc = "Sweep stale annotations (resolved/dismissed older than --since-days).",
            .flags = &.{
                .{ .long = "--since-days", .kind = .int, .default = .{ .int = 30 } },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(sweep.handle),
        },
    },
};
