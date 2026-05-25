//! Shared helpers for the capture subverbs.

const std = @import("std");
const engine = @import("engine");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

/// Resolve the session id the caller's verb should target.
///   - If explicit `session_id_flag` is set, return it.
///   - Else look up the active session for the vendor tuple derived
///     from $PLANAR_VENDOR / $PLANAR_VENDOR_SESSION_ID, creating one
///     if none exists (matches Go `session.EnsureActive`).
pub fn resolveSessionId(
    ctx: *const runtime.Ctx,
    d: anytype,
    session_id_flag: ?i64,
) i64 {
    if (session_id_flag) |sid| return sid;

    const vendor: []const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR")) |v|
        (if (v.len > 0) @as([]const u8, v) else "cli")
    else
        "cli";
    const vsid: ?[]const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR_SESSION_ID")) |v|
        (if (v.len > 0) @as([]const u8, v) else null)
    else
        null;

    const sid = engine.runtime.session.ensureActive(d, ctx.allocator, vendor, vsid) catch |e|
        exit.die(ctx, e, "ensureActive: {s}", .{@errorName(e)});
    return sid;
}
