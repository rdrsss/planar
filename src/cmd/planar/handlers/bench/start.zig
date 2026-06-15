//! handlers/bench/start — `planar bench start <run-uid> --plan <id>
//!   --arm <arm> --base-sha <sha> --config-hash <h>
//!   [--config-json <blob>] [--corpus-repo <name>]`
//!
//! Mints a new run record and prints the run_uid. The run_uid must be
//! supplied by the caller (ULID/uuid minted by the harness) so archived
//! transcripts can be keyed by it independently of the DB autoincrement.
//!
//! Arm validation: the known arms are strict, eligibility, grouped.
//! Pilot/probe runs may use free-text arms without a migration, so the
//! validator warns but does not refuse unknown values (spec §2).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

/// Known experiment arms — validated at the parse layer per the spec.
/// Pilot/probe runs may pass free-text; this is just a recognized set.
const known_arms = [_][]const u8{ "strict", "eligibility", "grouped" };

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "bench", "start" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Validate arm against known set (warn, do not refuse — pilot arms).
    // The spec says enum is enforced at the CLI parse layer but also notes
    // pilot/probe runs need no migration for new arm values; we warn only.
    const arm = args.arm;
    var arm_known = false;
    for (known_arms) |k| {
        if (std.mem.eql(u8, arm, k)) {
            arm_known = true;
            break;
        }
    }
    if (!arm_known) {
        ctx.stderr.print(
            "warn: bench start: unrecognized arm '{s}'; recognized arms: strict, eligibility, grouped\n",
            .{arm},
        ) catch {};
    }

    // Validate --config-json is well-formed JSON when supplied.
    if (args.config_json) |blob| {
        var parsed = std.json.parseFromSlice(std.json.Value, ctx.allocator, blob, .{}) catch
            exit.die(ctx, error.InvalidInput, "bench start: --config-json is not valid JSON: {s}", .{blob});
        parsed.deinit();
    }

    const res = engine.runs.lifecycle.start(d, ctx.allocator, .{
        .run_uid = args.run_uid,
        .plan_id = args.plan,
        .arm = arm,
        .base_sha = args.base_sha,
        .config_hash = args.config_hash,
        .config_json = args.config_json,
        .corpus_repo = args.corpus_repo,
    }) catch |e| switch (e) {
        error.DuplicateRunUid => exit.die(
            ctx,
            error.AlreadyExists,
            "bench start: run_uid '{s}' already exists",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "bench start: {s}", .{@errorName(e)}),
    };
    defer res.deinit(ctx.allocator);

    try ctx.stdout.print("{s}\n", .{res.run_uid});
}
