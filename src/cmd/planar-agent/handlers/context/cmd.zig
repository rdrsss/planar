//! handlers/context/cmd — `planar-agent context {add, list, resolve}` parent group.
//!
//! Run-scoped working-memory verbs for `context_records`. Workers holding
//! a claim write records via `add`; anyone can read them via `list`;
//! `resolve` transitions records through the active → consumed|superseded
//! lifecycle (the primitive that stage-close compaction, task 3905, will call).

const cli = @import("cli");

const add = @import("add.zig");
const list = @import("list.zig");
const resolve = @import("resolve.zig");

pub const verb: cli.Cmd = .{
    .name = "context",
    .desc = "Run-scoped working-memory records (add / list / resolve). Used by planar-execute workers.",
    .cmds = &.{
        add.verb,
        list.verb,
        resolve.verb,
    },
};
