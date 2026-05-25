//! handlers/local/cmd.zig — `planar local {list, link, unlink, import, migrate}`

const cli = @import("cli");

const list = @import("list.zig");
const link = @import("link.zig");
const unlink = @import("unlink.zig");
const import = @import("import.zig");
const migrate = @import("migrate.zig");

pub const verb: cli.Cmd = .{
    .name = "local",
    .desc = "Manage user-local sandbox skills and agents under ~/.planar/local/.",
    .cmds = &.{
        .{
            .name = "list",
            .desc = "List locally-installed skills and agents.",
            .flags = &.{
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "link",
            .desc = "Create or reuse symlinks from vendor paths to local source.",
            .flags = &.{
                .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--reconcile", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{ .{ .name = "name", .kind = .string, .required = false } },
            .run = cli.handler(link.handle),
        },
        .{
            .name = "unlink",
            .desc = "Remove symlinks from vendor paths.",
            .flags = &.{
                .{ .long = "--purge", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{ .{ .name = "name", .kind = .string, .required = true } },
            .run = cli.handler(unlink.handle),
        },
        .{
            .name = "import",
            .desc = "Import a skill or agent from an external directory.",
            .flags = &.{
                .{ .long = "--kind", .kind = .string },
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--no-link", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{ .{ .name = "path", .kind = .string, .required = true } },
            .run = cli.handler(import.handle),
        },
        .{
            .name = "migrate",
            .desc = "Migrate skills/agents to new Planar version.",
            .flags = &.{
                .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(migrate.handle),
        },
    },
};
