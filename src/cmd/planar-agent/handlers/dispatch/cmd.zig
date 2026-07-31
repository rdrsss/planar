//! handlers/dispatch/cmd — `planar-agent dispatch {preview, confirm}` group.
//!
//! The two-step authorization for a routing dispatch (plan 950 task 5528).
//! `preview` freezes what the operator is shown and returns a single-use,
//! expiry-bound token; `confirm` revalidates every bound value and writes the
//! immutable snapshot atomically, or refuses with `stale_preview`.
//!
//! These are agent-callable writes to the routing evidence plane, which is why
//! they live on `planar-agent` rather than the operator binary.

const cli = @import("cli");

const preview = @import("preview.zig");
const confirm = @import("confirm.zig");

pub const verb: cli.Cmd = .{
    .name = "dispatch",
    .desc = "Routing dispatch authorization (preview / confirm). Binds current state to a single-use token.",
    .cmds = &.{
        preview.verb,
        confirm.verb,
    },
};
