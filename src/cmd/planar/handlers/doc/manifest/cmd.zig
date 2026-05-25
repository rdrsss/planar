//! handlers/doc/manifest/cmd.zig — `planar doc manifest {build, check, validate}`

const cli = @import("cli");

const build = @import("build.zig");
const check = @import("check.zig");
const validate = @import("validate.zig");

pub const verb: cli.Cmd = .{
    .name = "manifest",
    .desc = "Manage documentation manifest.",
    .cmds = &.{
        .{
            .name = "build",
            .desc = "Build manifest of doc hashes.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(build.handle),
        },
        .{
            .name = "check",
            .desc = "Check for doc drift against manifest.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(check.handle),
        },
        .{
            .name = "validate",
            .desc = "Validate manifest file.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(validate.handle),
        },
    },
};
