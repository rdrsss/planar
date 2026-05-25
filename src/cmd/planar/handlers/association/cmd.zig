//! handlers/association/cmd.zig — `planar assoc {list, create, add, remove, members, detect}`
//!
//! The verb name is `assoc` to match Go's canonical name. The Zig CLI
//! framework has no alias support; Go's `"association"` alias is dropped.

const cli = @import("cli");

const list = @import("list.zig");
const create = @import("create.zig");
const add = @import("add.zig");
const remove = @import("remove.zig");
const members = @import("members.zig");
const detect = @import("detect.zig");

pub const verb: cli.Cmd = .{
    .name = "assoc",
    .desc = "Manage associations (many-to-many scope tags for repos).",
    .cmds = &.{
        .{
            .name = "list",
            .desc = "List all known associations.",
            .flags = &.{
                .{ .long = "--kind", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "create",
            .desc = "Create a new association.",
            .flags = &.{
                .{ .long = "--name", .kind = .string },
                .{ .long = "--kind", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "slug", .kind = .string, .required = true }},
            .run = cli.handler(create.handle),
        },
        .{
            .name = "add",
            .desc = "Add a repo to an association.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{
                .{ .name = "slug", .kind = .string, .required = true },
                .{ .name = "repo-path", .kind = .string, .required = true },
            },
            .run = cli.handler(add.handle),
        },
        .{
            .name = "remove",
            .desc = "Remove a repo from an association.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{
                .{ .name = "slug", .kind = .string, .required = true },
                .{ .name = "repo-path", .kind = .string, .required = true },
            },
            .run = cli.handler(remove.handle),
        },
        .{
            .name = "members",
            .desc = "List all project members of an association.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "slug", .kind = .string, .required = true }},
            .run = cli.handler(members.handle),
        },
        .{
            .name = "detect",
            .desc = "Propose (or apply) auto-detected associations for the current directory.",
            .flags = &.{
                .{ .long = "--apply", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(detect.handle),
        },
    },
};
