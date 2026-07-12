//! handlers/skills/cmd.zig — `planar skills render [slug...]`

const cli = @import("cli");
const render = @import("render.zig");
const status = @import("status.zig");
const repair = @import("repair.zig");

pub const verb: cli.Cmd = .{
    .name = "skills",
    .desc = "Render, inspect, and repair installed vendor projections.",
    .long_desc = "Manage unified skills and agent projections. Render produces\n  vendor trees from authored sources. Status reads the versioned install\n  manifest and reports installed freshness without mutation. Repair is\n  manifest-owned and preview-first; --apply replaces only stale or missing\n  managed paths.",
    .cmds = &.{
        .{
            .name = "render",
            .desc = "Render unified skill sources into per-vendor output trees.",
            .flags = &.{
                .{ .long = "--src", .kind = .string, .default = .{ .string = "skills/src" } },
                .{ .long = "--out", .kind = .string, .default = .{ .string = "." } },
                .{ .long = "--check", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--diff", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "slug", .kind = .string, .required = false }},
            .allow_extra_positionals = true,
            .run = cli.handler(render.handle),
        },
        .{
            .name = "status",
            .desc = "Report manifest-owned installed projection freshness.",
            .flags = &.{
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(status.handle),
        },
        .{
            .name = "repair",
            .desc = "Preview or apply manifest-owned projection repairs.",
            .flags = &.{
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--apply", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "name", .kind = .string, .required = false }},
            .allow_extra_positionals = true,
            .run = cli.handler(repair.handle),
        },
    },
};
