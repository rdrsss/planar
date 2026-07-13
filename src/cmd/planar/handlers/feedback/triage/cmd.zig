const cli = @import("cli");
const list = @import("list.zig");
const show = @import("show.zig");
const set = @import("set.zig");
pub const verb: cli.Cmd = .{ .name = "triage", .desc = "Review structured feedback triage.", .cmds = &.{
    .{ .name = "list", .desc = "List triaged findings.", .flags = &.{ .{ .long = "--plan", .kind = .int }, .{ .long = "--severity", .kind = .string }, .{ .long = "--disposition", .kind = .string }, .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } } }, .run = cli.handler(list.handle) },
    .{ .name = "show", .desc = "Show a triaged finding.", .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }}, .positionals = &.{.{ .name = "finding", .kind = .string, .required = true }}, .run = cli.handler(show.handle) },
    .{ .name = "set", .desc = "Set operator-confirmed triage fields.", .flags = &.{ .{ .long = "--severity", .kind = .string, .required = true }, .{ .long = "--disposition", .kind = .string, .required = true }, .{ .long = "--reproduction", .kind = .string, .required = true }, .{ .long = "--duplicate-of", .kind = .string }, .{ .long = "--evidence", .kind = .string }, .{ .long = "--scope", .kind = .string }, .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } } }, .positionals = &.{.{ .name = "finding", .kind = .string, .required = true }}, .run = cli.handler(set.handle) },
} };
