//! handlers/links/update — `planar links update <link-id> --sync <direction>`
//!
//! STUB — deferred to M11 (external-plane work).
//!
//! Go's `links update` mutates the sync_direction column on an existing
//! external_links row (not entity_links). This is M11 (external-plane) work
//! per the M2 locked decision D-update-deferred. The handler is present for
//! surface parity (so `planar links` lists the verb) but returns NotImplemented.
//!
//! Follow-up task slug: handlers-links-update-m11 (plan 316).

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "links", "update" }, args_ptr);
    const ctx = runtime.current();
    _ = args;
    exit.die(ctx, error.NotImplemented, "links update is deferred to M11 (external-plane); use `links remove` + `links add` as a workaround", .{});
}
