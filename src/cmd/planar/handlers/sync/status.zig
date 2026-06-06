//! handlers/sync/status — `planar sync status [--entity <kind:id>] [--system <slug>] [--json]`
//!
//! Report sync status for one or more external links, optionally filtered by
//! local entity (`--entity`, e.g. `task:42`) and/or external system slug
//! (`--system`). The filter is an entity reference, NOT a scope slug — hence
//! `--entity`, not `--scope` (which means a scope slug on every other verb).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");
const sync_common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "sync", "status" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var filter: engine.external.link.ListFilter = .{
        .system_slug = args.system,
    };
    if (args.entity) |entity_ref| {
        const ref = sync_common.parseKindIDRef(entity_ref) catch
            exit.die(ctx, error.InvalidInput, "invalid --entity value '{s}'; expected <kind>:<integer-id>", .{entity_ref});
        filter.entity_kind = ref.kind;
        filter.entity_id = ref.id;
    }
    const rows = engine.external.sync.status(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "sync status: {s}", .{@errorName(e)});
    defer engine.external.sync.deinitStatusRows(rows, ctx.allocator);

    if (args.json) {
        for (rows) |row| {
            try ctx.stdout.print("{{\"link_id\":{d},\"entity_kind\":", .{row.link_id});
            try output.writeJsonString(ctx.stdout, @tagName(row.entity_kind));
            try ctx.stdout.print(",\"entity_id\":{d},\"external_id\":", .{row.entity_id});
            try output.writeJsonString(ctx.stdout, row.external_id);
            try ctx.stdout.print(",\"system_id\":{d}", .{row.system_id});
            if (row.last_synced_at) |ts| {
                try ctx.stdout.print(",\"last_synced_at\":", .{});
                try output.writeJsonString(ctx.stdout, ts);
            }
            try ctx.stdout.print(",\"last_sync_status\":", .{});
            try output.writeJsonString(ctx.stdout, row.last_sync_status.toText());
            try ctx.stdout.print("}}\n", .{});
        }
        return;
    }

    if (rows.len == 0) {
        try ctx.stdout.print("no external links\n", .{});
        return;
    }
    try ctx.stdout.print("{s:<6}  {s:<14}  {s:<18}  {s:<8}  {s:<24}  {s}\n", .{ "link", "entity", "external-id", "system", "last-sync", "status" });
    for (rows) |row| {
        const last = row.last_synced_at orelse "never";
        const link_u: u64 = @intCast(row.link_id);
        const entity_u: u64 = @intCast(row.entity_id);
        const system_u: u64 = @intCast(row.system_id);
        try ctx.stdout.print("{d:<6}  {s}:{d:<11}  {s:<18}  {d:<8}  {s:<24}  {s}\n", .{ link_u, @tagName(row.entity_kind), entity_u, row.external_id, system_u, last, row.last_sync_status.toText() });
    }
}
