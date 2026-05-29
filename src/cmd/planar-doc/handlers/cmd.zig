//! cmd/planar-doc/handlers/cmd — verb tree for `planar-doc`.

const cli = @import("cli");

const build_h = @import("build.zig");
const verify_h = @import("verify.zig");
const diff_h = @import("diff.zig");
const cover_h = @import("cover.zig");
const nodoc_h = @import("nodoc.zig");
const lint_h = @import("lint.zig");
const schema_h = @import("schema.zig");

pub const verbs: []const cli.Cmd = &.{
    .{
        .name = "build",
        .desc = "Recompute the manifest from the current working tree.",
        .flags = &.{
            .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        },
        .run = cli.handler(build_h.handle),
    },
    .{
        .name = "verify",
        .desc = "Compare the stored manifest root against the current tree.",
        .flags = &.{
            .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        },
        .run = cli.handler(verify_h.handle),
    },
    .{
        .name = "diff",
        .desc = "List drift records (regenerate-candidate / hand-edit / new-authoring / deletion / nodoc-stale).",
        .flags = &.{
            .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        },
        .run = cli.handler(diff_h.handle),
    },
    .{
        .name = "cover",
        .desc = "Add or remove a source path on a doc entry.",
        .flags = &.{
            .{ .long = "--remove", .kind = .bool, .default = .{ .bool = false }, .desc = "Remove instead of add" },
        },
        .positionals = &.{
            .{ .name = "doc", .kind = .string, .required = true },
            .{ .name = "source", .kind = .string, .required = true },
        },
        .run = cli.handler(cover_h.handle),
    },
    .{
        .name = "nodoc",
        .desc = "Add or remove a path in the nodoc map.",
        .flags = &.{
            .{ .long = "--remove", .kind = .bool, .default = .{ .bool = false }, .desc = "Remove instead of add" },
        },
        .positionals = &.{
            .{ .name = "source", .kind = .string, .required = true },
        },
        .run = cli.handler(nodoc_h.handle),
    },
    .{
        .name = "lint",
        .desc = "Validate citations and reference declarations in markdown docs.",
        .flags = &.{
            .{ .long = "--path", .kind = .string },
            .{ .long = "--no-refs", .kind = .bool, .default = .{ .bool = false } },
            .{ .long = "--refs-only", .kind = .bool, .default = .{ .bool = false } },
            .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
        },
        .run = cli.handler(lint_h.handle),
    },
    schema_h.verb,
};
