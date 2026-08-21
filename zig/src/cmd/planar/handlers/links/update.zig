//! handlers/links/update — `planar links update <link-id> --sync <direction>`
//!
//! STUB — deferred to M11 (external-plane work).
//!
//! Go's `links update` mutates the sync_direction column on an existing
//! external_links row (not entity_links). This is M11 (external-plane) work
//! per the M2 locked decision D-update-deferred. The handler is present for
//! surface parity but is hidden and returns NotImplemented. The top-level
//! `unlink` / `link` pair is the only current recovery, and it is destructive:
//! it cannot preserve the old row's URL, config, sync state, or event history.
//!
//! Follow-up task slug: handlers-links-update-m11 (plan 316).

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "links", "update" }, args_ptr);
    const ctx = runtime.current();
    _ = args;
    exit.die(ctx, error.NotImplemented, "links update is deferred to M11 (external-plane); no lossless CLI update exists (top-level `unlink` + `link` is destructive)", .{});
}
