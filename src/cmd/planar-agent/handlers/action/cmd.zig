//! handlers/action/cmd — `planar-agent action {start, end}` parent group.
//!
//! Nested action lifecycle for sub-tool-calls or sub-phases inside an
//! existing claim. Optional — lightweight claims that don't need
//! per-call action rows skip these.

const cli = @import("cli");

const start = @import("start.zig");
const end = @import("end.zig");

pub const verb: cli.Cmd = .{
    .name = "action",
    .desc = "Nested action lifecycle (sub-tool-calls inside a claim).",
    .cmds = &.{
        start.verb,
        end.verb,
    },
};
