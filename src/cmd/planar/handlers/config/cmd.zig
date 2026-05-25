//! handlers/config/cmd.zig — `planar config {show, edit, validate, init, path}`

const cli = @import("cli");

const show = @import("show.zig");
const edit = @import("edit.zig");
const validate = @import("validate.zig");
const init = @import("init.zig");
const path = @import("path.zig");

pub const verb: cli.Cmd = .{
    .name = "config",
    .desc = "Manage Planar configuration.",
    .cmds = &.{
        .{
            .name = "show",
            .desc = "Print the resolved configuration.",
            .flags = &.{
                .{ .long = "--effective", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--raw", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--defaults", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--format", .kind = .string, .default = .{ .string = "text" } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(show.handle),
        },
        .{ .name = "edit", .desc = "Edit the configuration file in $EDITOR.", .run = cli.handler(edit.handle) },
        .{ .name = "validate", .desc = "Validate configuration file syntax.", .run = cli.handler(validate.handle) },
        .{ .name = "init", .desc = "Initialize the configuration file.", .run = cli.handler(init.handle) },
        .{ .name = "path", .desc = "Show the configuration file path.", .run = cli.handler(path.handle) },
    },
};
