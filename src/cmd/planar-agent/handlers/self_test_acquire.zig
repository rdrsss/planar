//! handlers/self_test_acquire.zig — `planar-agent self-test-acquire`
//!
//! INTERNAL / M1-ONLY VERB. Exists only to give the cross-process
//! contention integration test (claim-concurrency-tests, t#2536) a
//! way to drive `agentactivity.atomic.pullNext` through the compiled
//! binary before M2 wires the real `pull` verb. M2 removes this file
//! and replaces it with the real `pull` handler.
//!
//! Contract:
//!   planar-agent self-test-acquire <plan-id> [--ttl <secs>] [--vendor <s>]
//!
//! Emits one JSON object on stdout:
//!   {"ok":true,"no_work":true}                      ← nothing to pull
//!   {"ok":true,"claim_token":"<hex>","task_id":N}   ← won the race
//!
//! Exit code is always 0 on success — the caller distinguishes the two
//! outcomes by parsing `no_work`. The concurrency test asserts exactly
//! one process gets a claim_token and the other gets no_work.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");
const main = @import("../main.zig");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "self-test-acquire",
    .desc = "INTERNAL: drive agentactivity.atomic.pullNext (M1 concurrency test scaffold).",
    .flags = &.{
        .{ .long = "--ttl", .kind = .int, .default = .{ .int = 600 }, .desc = "Lease TTL seconds" },
        .{ .long = "--vendor", .kind = .string, .default = .{ .string = "selftest" }, .desc = "Vendor tag" },
    },
    .positionals = &.{
        .{ .name = "plan_id", .kind = .int, .required = true, .desc = "Plan id to pull from" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"self-test-acquire"}, args_ptr);
    const ctx = runtime.current();

    // The integration test seeds the DB via the operator binary
    // (`planar init`, `planar plan add`, `planar task add`); we use
    // ensureDbReadOnly here to refuse a stale-schema DB cleanly. The
    // write happens via the engine layer below; the binary only opens
    // the DB once, and the singleton handle is shared between the
    // read (handshake) and the writes.
    const d = runtime.ensureDbReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Open a session so the claim has somewhere to hang. Reuse if a
    // matching vendor session already exists.
    const session_id = engine.runtime.session.ensureActive(d, ctx.allocator, args.vendor, null) catch |e|
        exit.die(ctx, e, "ensureActive: {s}", .{@errorName(e)});

    const result = engine.runtime.agentactivity.atomic.pullNext(d, ctx.allocator, .{
        .plan_id = args.plan_id,
        .session_id = session_id,
        .vendor = args.vendor,
        .ttl_secs = args.ttl,
    }) catch |e| exit.die(ctx, e, "pullNext: {s}", .{@errorName(e)});
    defer result.deinit(ctx.allocator);

    if (result.no_work) {
        try ctx.stdout.print("{{\"ok\":true,\"no_work\":true}}\n", .{});
        return;
    }
    try ctx.stdout.print(
        "{{\"ok\":true,\"claim_token\":\"{s}\",\"task_id\":{d},\"action_id\":{d}}}\n",
        .{ result.claim.?.claim_token, result.task_id, result.action_id },
    );
}
