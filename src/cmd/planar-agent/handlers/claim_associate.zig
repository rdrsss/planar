//! handlers/claim_associate — `planar-agent claim-associate --claim <token> --run <id> [--stage <s>]`
//!
//! Associates a pre-acquired claim with a workflow_runs row (and optionally a
//! stage name) at dispatch time.  Used by `planar-execute` so that a claim
//! pulled before the run row was opened can have its `run_id` / `stage`
//! backfilled, making `planar-agent context add --claim` work correctly
//! (context add needs run_id stamped on the claim — decision 447/450/457).
//!
//! Design notes (decision 457 / Q602):
//!   - Best-effort from the caller's perspective; errors here never block
//!     the agent spawn.
//!   - `--run` is required; `--stage` is optional (empty → NULL in DB).
//!   - A no-op (claim token not found or not active) exits 0 with
//!     `{"ok":true,"updated":0}` — the caller log-and-continues.
//!   - Only claims in `status = 'active'` are updated (guard in SQL).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");

const store = engine.runtime.agentactivity.store;

pub const verb: cli.Cmd = .{
    .name = "claim-associate",
    .desc = "Associate a pre-acquired active claim with a workflow run (and optional stage). Used by planar-execute at dispatch time.",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token to associate" },
        .{ .long = "--run", .kind = .int, .required = true, .desc = "workflow_runs.id to stamp on the claim" },
        .{ .long = "--stage", .kind = .string, .desc = "Stage name to record (e.g. code, review); omit for NULL" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"claim-associate"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // --run is declared required above; the parser enforces its presence.
    // --stage is optional; null → NULL in DB (associate with run only).
    const stage: ?[]const u8 = args.stage;

    const updated = store.associateClaimRun(d, args.claim, args.run, stage) catch |e|
        exit.die(ctx, e, "claim-associate: {s}", .{@errorName(e)});

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"updated\":{d}}}\n", .{updated});
    } else {
        try ctx.stdout.print("ok updated:{d} claim:{s}\n", .{ updated, args.claim });
    }
}
