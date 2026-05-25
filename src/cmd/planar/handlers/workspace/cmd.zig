//! handlers/workspace/cmd.zig — `planar workspace {init, doctor, routing {build, show}, regenerate}`

const cli = @import("cli");

const init = @import("init.zig");
const doctor = @import("doctor.zig");
const regenerate = @import("regenerate.zig");
const routing = @import("routing/cmd.zig");

pub const verb: cli.Cmd = .{
    .name = "workspace",
    .desc = "Manage workspace state directories and their AGENTS.md surfaces.",
    .cmds = &.{
        .{
            .name = "init",
            .desc = "Initialize a workspace (org-level association).",
            .flags = &.{
                .{ .long = "--name", .kind = .string },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--scan", .kind = .int, .default = .{ .int = 1 } },
                .{ .long = "--no-scan", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--enrich", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(init.handle),
        },
        .{
            .name = "doctor",
            .desc = "Scan and fix workspace registration and state consistency.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(doctor.handle),
        },
        routing.verb,
        .{
            .name = "regenerate",
            .desc = "Regenerate AGENTS.md from current state.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "workspace", .kind = .string, .required = false }},
            .run = cli.handler(regenerate.handle),
        },
    },
};
