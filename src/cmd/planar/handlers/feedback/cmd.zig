const cli = @import("cli");
const triage = @import("triage/cmd.zig");
pub const verb: cli.Cmd = .{ .name = "feedback", .desc = "Manage structured feedback.", .cmds = &.{triage.verb} };
