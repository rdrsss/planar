//! handlers/skills/cmd.zig — `planar skills render [slug...]`

const cli = @import("cli");
const render = @import("render.zig");

pub const verb: cli.Cmd = .{
    .name = "skills",
    .desc = "Render unified skill sources into per-vendor output trees.",
    .long_desc = "Manage the unified skill source tree.\n\n  Skill sources live under skills/src/<slug>.md as Markdown with YAML\n  frontmatter. The 'render' subcommand produces the per-vendor output\n  trees (commands/claude/, skills/codex/, skills/copilot/) from those\n  sources.",
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
    },
};
