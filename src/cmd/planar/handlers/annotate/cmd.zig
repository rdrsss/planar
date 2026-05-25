//! handlers/annotate/cmd.zig — `planar annotate {...}`

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
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Create a new annotation.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
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
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(list.handle),
        },
        .{
            .name = "update",
            .desc = "Update an annotation.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
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
            .desc = "Add or remove tags on an annotation.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
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
            .desc = "Resolve multiple annotations.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(bulk_resolve.handle),
        },
        .{
            .name = "bulk-dismiss",
            .desc = "Dismiss multiple annotations.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(bulk_dismiss.handle),
        },
        .{
            .name = "bulk-archive",
            .desc = "Archive multiple annotations.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(bulk_archive.handle),
        },
        .{
            .name = "verify",
            .desc = "Verify annotation anchors against workspace state.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(verify.handle),
        },
        .{
            .name = "sweep",
            .desc = "Sweep stale annotations.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(sweep.handle),
        },
    },
};
