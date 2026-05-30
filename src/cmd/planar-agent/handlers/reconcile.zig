//! handlers/reconcile — `planar-agent reconcile [--dry-run] [--stale-after] [--session]`
//!
//! Operator recovery: mark expired active claims stale, close orphaned
//! actions whose owning session has ended. Does NOT touch tasks.status.
//! `--session <id>` restricts the sweep to claims owned by that session
//! (plan 493 F2); default is unchanged (global sweep — preserves the
//! single-operator recovery contract bit-for-bit).
//! JSON: { ok, claims_marked_stale, actions_closed, candidates? }.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const json = @import("json.zig");

const store = engine.runtime.agentactivity.store;

pub const verb: cli.Cmd = .{
    .name = "reconcile",
    .desc = "Operator recovery: mark expired claims stale, close orphaned actions.",
    .flags = &.{
        .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false }, .desc = "Report candidates without writing" },
        .{ .long = "--stale-after", .kind = .string, .default = .{ .string = "0" }, .desc = "Additional grace beyond lease expiry (default 0s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)" },
        .{ .long = "--session", .kind = .int, .desc = "Restrict the sweep to claims owned by this session_id; default = global (plan 493 F2)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"reconcile"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const stale_after_secs = cli.duration.parseSeconds(args.stale_after) catch |e|
        exit.die(ctx, e, "invalid --stale-after '{s}': expected bare seconds (e.g. 0) or suffixed duration (e.g. 10m, 1h, 500ms)", .{args.stale_after});

    // Wrap in BEGIN IMMEDIATE for non-dry-run so reconcile is atomic
    // (partial failure leaves no half-marked-stale claims).
    if (!args.dry_run) {
        d.exec("BEGIN IMMEDIATE") catch |e| exit.die(ctx, e, "BEGIN IMMEDIATE: {s}", .{@errorName(e)});
    }

    const result = store.reconcileStale(d, ctx.allocator, .{
        .stale_after_secs = stale_after_secs,
        .dry_run = args.dry_run,
        .filter_session_id = args.session,
    }) catch |e| {
        if (!args.dry_run) d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "reconcile: {s}", .{@errorName(e)});
    };
    defer result.deinit(ctx.allocator);

    if (!args.dry_run) {
        d.exec("COMMIT") catch |e| exit.die(ctx, e, "COMMIT: {s}", .{@errorName(e)});
    }

    if (args.json) {
        const w = ctx.stdout;
        try w.print(
            "{{\"ok\":true,\"claims_marked_stale\":{d},\"actions_closed\":{d}",
            .{ result.claims_marked_stale, result.actions_closed },
        );
        if (args.dry_run) {
            try w.print(",\"candidates\":[", .{});
            for (result.candidates, 0..) |c, i| {
                if (i > 0) try w.print(",", .{});
                try w.print(
                    "{{\"kind\":\"{s}\",\"id\":{d},\"claim\":",
                    .{ c.entity_kind.toText(), c.entity_id },
                );
                try json.writeClaim(w, c, null);
                try w.print("}}", .{});
            }
            try w.print("]", .{});
        }
        try w.print("}}\n", .{});
    } else {
        if (args.dry_run) {
            try ctx.stdout.print("dry-run: {d} candidate(s)\n", .{result.candidates.len});
            for (result.candidates) |c| {
                try ctx.stdout.print("  {s}:{d} token:{s}\n", .{ c.entity_kind.toText(), c.entity_id, c.claim_token });
            }
        } else {
            try ctx.stdout.print("reconciled: {d} claim(s) stale, {d} action(s) closed\n", .{ result.claims_marked_stale, result.actions_closed });
        }
    }
}
