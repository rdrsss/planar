//! handlers/scope/cmd.zig — `planar scope {show, suggest}`

const cli = @import("cli");

const show = @import("show.zig");
const suggest = @import("suggest.zig");
const removed = @import("removed.zig");

pub const verb: cli.Cmd = .{
    .name = "scope",
    .desc = "Inspect the cwd-derived scope and suggest memberships.",
    .cmds = &.{
        .{
            .name = "show",
            .desc = "Show the cwd-derived scope (and any --scope override).",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(show.handle),
        },
        .{
            .name = "suggest",
            .desc = "Suggest scope associations based on cwd.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(suggest.handle),
        },
        .{
            .name = "use",
            .desc = "Removed in plan 153 M5 — see `planar scope show`.",
            .positionals = &.{.{ .name = "slug", .kind = .string, .required = false }},
            .allow_unknown_flags = true,
            .allow_extra_positionals = true,
            .run = cli.handler(removed.handleUse),
        },
        .{
            .name = "pop",
            .desc = "Removed in plan 153 M5 — see `planar scope show`.",
            .allow_unknown_flags = true,
            .allow_extra_positionals = true,
            .run = cli.handler(removed.handlePop),
        },
        .{
            .name = "clear",
            .desc = "Removed in plan 153 M5 — see `planar scope show`.",
            .allow_unknown_flags = true,
            .allow_extra_positionals = true,
            .run = cli.handler(removed.handleClear),
        },
    },
};
