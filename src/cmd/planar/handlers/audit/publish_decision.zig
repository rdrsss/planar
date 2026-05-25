//! handlers/audit/publish-decision — `planar audit publish-decision <decision-id>`
//!
//! M7 surface stub. Full behavior (post decision body to every linked
//! operational-plane target) requires the external plane / adapter
//! registry, which lands in M8. Until then, the verb exits with a
//! clear NotImplemented so operators don't think it silently no-op'd.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "audit", "publish-decision" }, args_ptr);
    const ctx = runtime.current();
    exit.die(
        ctx,
        error.NotImplemented,
        "audit publish-decision requires the external plane (M8); not available in this build",
        .{},
    );
}
