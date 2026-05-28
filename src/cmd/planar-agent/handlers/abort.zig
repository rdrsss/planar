//! handlers/abort — `planar-agent abort --claim <token> [--reason <text>]`
//!
//! Operator force-release of a stuck claim from ANY session. Writes an
//! audit row recording the aborting session as the actor. Does NOT
//! touch tasks.status — the operator decides whether to revive the
//! task afterwards.
//!
//! JSON: { ok, claim_token, claim, aborting_session }.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const json = @import("json.zig");

const store = engine.runtime.agentactivity.store;
const types = engine.runtime.agentactivity.types;
const session_mod = engine.runtime.session;

pub const verb: cli.Cmd = .{
    .name = "abort",
    .desc = "Operator force-release of a stuck claim (any session, not just the owner).",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token to force-release" },
        .{ .long = "--reason", .kind = .string, .desc = "Optional reason recorded on the claim and audit row" },
        .{ .long = "--vendor", .kind = .string, .default = .{ .string = "planar-agent" }, .desc = "Vendor tag for the aborting session" },
        .{ .long = "--vendor-session", .kind = .string, .desc = "Vendor session id for the aborting session" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"abort"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // The aborting session — distinct from the claim's owning session.
    // We open it BEFORE the transaction so the audit row's session_id
    // resolves cleanly.
    const aborting_sid = session_mod.ensureActive(d, ctx.allocator, args.vendor, args.vendor_session) catch |e|
        exit.die(ctx, e, "ensureActive (aborting): {s}", .{@errorName(e)});

    d.exec("BEGIN IMMEDIATE") catch |e| exit.die(ctx, e, "BEGIN IMMEDIATE: {s}", .{@errorName(e)});

    const c = store.abortClaim(d, ctx.allocator, args.claim, args.reason) catch |e| {
        d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "abort: {s}", .{@errorName(e)});
    };
    defer c.deinit(ctx.allocator);

    // Audit row: action_kind='other', vendor='planar', summary='aborted by operator'
    // attributing to the aborting session.
    const audit_summary = blk: {
        if (args.reason) |r| break :blk std.fmt.allocPrint(ctx.allocator, "aborted by operator: {s}", .{r}) catch
            exit.die(ctx, error.OutOfMemory, "audit summary OOM", .{});
        break :blk std.fmt.allocPrint(ctx.allocator, "aborted by operator", .{}) catch
            exit.die(ctx, error.OutOfMemory, "audit summary OOM", .{});
    };
    defer ctx.allocator.free(audit_summary);

    const audit_id = store.startAction(d, ctx.allocator, .{
        .session_id = aborting_sid,
        .claim_id = c.id,
        .action_kind = .other,
        .vendor = "planar-agent",
    }) catch |e| {
        d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "audit startAction: {s}", .{@errorName(e)});
    };
    store.endAction(d, ctx.allocator, audit_id, types.Outcome.aborted, audit_summary) catch |e| {
        d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "audit endAction: {s}", .{@errorName(e)});
    };

    d.exec("COMMIT") catch |e| exit.die(ctx, e, "COMMIT: {s}", .{@errorName(e)});

    if (args.json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"claim_token\":", .{});
        try std.json.Stringify.encodeJsonString(c.claim_token, .{}, w);
        try w.print(",\"claim\":", .{});
        try json.writeClaim(w, c, null);
        try w.print(",\"aborting_session\":{d}}}\n", .{aborting_sid});
    } else {
        try ctx.stdout.print("aborted claim:{s} by session:{d}\n", .{ c.claim_token, aborting_sid });
    }
}
