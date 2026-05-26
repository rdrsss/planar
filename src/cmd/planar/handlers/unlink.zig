//! handlers/unlink.zig — `planar unlink <link-id>`
//!
//! Removes an external_links row by its link id. Mirrors Go's
//! `external.UnlinkCmd` (src/cmd/planar/internal/external/link.go).
//!
//! The link row is deleted; associated sync_events rows cascade.
//! A best-effort `session_entries` row records the action so the audit
//! trail captures the unlink even after the link row is gone. The
//! cross-scope guard from Go is deferred — zig's `link` handler also
//! accepts `--scope` for parity but does not yet enforce a guard.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("../runtime.zig");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "unlink",
    .desc = "Remove an external_links row by link id.",
    .long_desc = "Remove an external_links row by its link id.\n\n  Associated sync_events rows are also removed (cascade).",
    .flags = &.{
        .{ .long = "--scope", .kind = .string, .desc = "Scope for the cross-scope guard (currently informational)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "link-id", .kind = .string, .required = true, .desc = "External-link id (integer)" },
    },
    .run = cli.handler(handle),
};

const UnlinkJSON = struct {
    ok: bool,
    id: i64,
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"unlink"}, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    _ = args.scope;

    const link_id = std.fmt.parseInt(i64, args.link_id, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid link id '{s}'", .{args.link_id});

    // Resolve the link first so a missing id yields a precise error
    // BEFORE the delete attempt would otherwise report "not found".
    const lnk = engine.external.link.show(d, ctx.allocator, link_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "link {d} not found", .{link_id}),
        else => exit.die(ctx, e, "unlink: lookup link {d}: {s}", .{ link_id, @errorName(e) }),
    };
    engine.external.link.deinit(lnk, ctx.allocator);

    engine.external.link.delete(d, link_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "link {d} not found", .{link_id}),
        else => exit.die(ctx, e, "unlink: delete link {d}: {s}", .{ link_id, @errorName(e) }),
    };

    // Best-effort audit append. A missing/failed session is non-fatal
    // (mirrors Go's `if sessErr == nil { _ = AppendEntry(...) }`).
    appendAuditEntry(ctx, d, link_id);

    if (args.json) {
        const out = UnlinkJSON{ .ok = true, .id = link_id };
        try std.json.Stringify.value(out, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("unlinked: external link {d} removed\n", .{link_id});
    }
}

/// appendAuditEntry writes a session_entries row recording the unlink.
/// Errors are swallowed: the unlink itself already succeeded, and a
/// missing sessions table or absent active session must not fail the
/// CLI call.
fn appendAuditEntry(ctx: *const runtime.Ctx, d: anytype, link_id: i64) void {
    const vendor: []const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR")) |v|
        (if (v.len > 0) @as([]const u8, v) else "cli")
    else
        "cli";
    const vsid: ?[]const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR_SESSION_ID")) |v|
        (if (v.len > 0) @as([]const u8, v) else null)
    else
        null;

    const sid = engine.runtime.session.ensureActive(d, ctx.allocator, vendor, vsid) catch return;
    const body = std.fmt.allocPrint(
        ctx.allocator,
        "unlink: removed external link {d}",
        .{link_id},
    ) catch return;
    defer ctx.allocator.free(body);
    engine.runtime.session.appendEntry(d, sid, "action", body) catch return;
}
