//! handlers/capture/cmd.zig — `planar capture {session, end, note, command, file, snapshot}`

const cli = @import("cli");

const session = @import("session.zig");
const end = @import("end.zig");
const note = @import("note.zig");
const command = @import("command.zig");
const file = @import("file.zig");
const snapshot = @import("snapshot.zig");

pub const verb: cli.Cmd = .{
    .name = "capture",
    .desc = "Manage explicit session capture.",
    .cmds = &.{
        .{
            .name = "session",
            .desc = "Open or reuse a session for the current (vendor, vendor-session-id) tuple.",
            .flags = &.{
                .{ .long = "--vendor", .kind = .string },
                .{ .long = "--vendor-session-id", .kind = .string },
                .{ .long = "--model", .kind = .string },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(session.handle),
        },
        .{
            .name = "end",
            .desc = "End the active or specified session.",
            .flags = &.{
                .{ .long = "--session", .kind = .int },
                .{ .long = "--summary", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "session-id", .kind = .string, .required = false }},
            .run = cli.handler(end.handle),
        },
        .{
            .name = "note",
            .desc = "Append a narrative note to the active session.",
            .flags = &.{
                .{ .long = "--session", .kind = .int },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "body", .kind = .string, .required = true }},
            .run = cli.handler(note.handle),
        },
        .{
            .name = "command",
            .desc = "Append a command to the active session.",
            .flags = &.{
                .{ .long = "--session", .kind = .int },
                .{ .long = "--outcome", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "command", .kind = .string, .required = true }},
            .run = cli.handler(command.handle),
        },
        .{
            .name = "file",
            .desc = "Attach a file to the active session.",
            .flags = &.{
                .{ .long = "--session", .kind = .int },
                .{ .long = "--role", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "path", .kind = .string, .required = true }},
            .run = cli.handler(file.handle),
        },
        .{
            .name = "snapshot",
            .desc = "Create a context snapshot.",
            .flags = &.{
                .{ .long = "--session", .kind = .int },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--note", .kind = .string },
                .{ .long = "--next-action", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "body", .kind = .string, .required = false }},
            .run = cli.handler(snapshot.handle),
        },
    },
};
