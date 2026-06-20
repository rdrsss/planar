//! handlers/workflow/cmd.zig — `planar workflow {list, show}`
//!
//! Discovery layer for user-authored and shipped Lua workflows.
//! Both subverbs are READ-ONLY filesystem scans — they never open SQLite.

const cli = @import("cli");

const list = @import("list.zig");
const show = @import("show.zig");

pub const verb: cli.Cmd = .{
    .name = "workflow",
    .desc = "Discover and inspect Lua workflows for planar-execute.",
    .long_desc = "Enumerate and inspect shipped and sandbox Lua workflows.\n\n" ++
        "  Shipped workflows live at $PLANAR_HOME/workflows/ (default\n" ++
        "  ~/.planar/workflows/).  Sandbox workflows live at\n" ++
        "  ~/.planar/local/workflows/ and are marked `local`.\n\n" ++
        "  These commands are READ-ONLY — they do not open SQLite.",
    .cmds = &.{
        .{
            .name = "list",
            .desc = "List shipped and sandbox workflows.",
            .flags = &.{
                .{ .long = "--local", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "show",
            .desc = "Show @meta and source path for a named workflow.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "name", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
    },
};
