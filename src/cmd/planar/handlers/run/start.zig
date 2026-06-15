//! handlers/run/start — `planar run start --plan <id> [--workflow <name>] --json`
//!
//! Mints a new operational run record and prints the run_uid as JSON.
//! The run_uid is generated via SQLite's randomblob(16) — callers do not
//! supply it. This follows the same pattern as claim_token generation in
//! engine/runtime/agentactivity/store.zig.
//!
//! Arm defaults to 'op' (operational surface, decision D10). The required
//! engine fields base_sha and config_hash are set to sentinel defaults
//! appropriate for an operational (non-measurement) run:
//!
//!   base_sha    = "" (empty — no code-corpus baseline for op runs)
//!   config_hash = "" (empty — no experiment config hash for op runs)
//!   config_json = null
//!   corpus_repo = null
//!
//! The workflow name (if given) is stored as a free-text arm override so
//! a workflow can distinguish its own runs from other op runs. This
//! matches the spec's "pilot/probe runs may use free-text arms" allowance.
//!
//! JSON output shape:
//!
//!   {"run_uid": "<hex-32>", "plan_id": <i64>, "arm": "op"}

const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "run", "start" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Generate the run_uid via SQLite's randomblob — same pattern as the
    // claim_token generation in agentactivity/store.zig.
    var uid_stmt = d.prepare("select lower(hex(randomblob(16)))") catch
        exit.die(ctx, error.Unexpected, "run start: uid generation query failed", .{});
    defer uid_stmt.finalize();
    switch (uid_stmt.step() catch exit.die(ctx, error.Unexpected, "run start: uid generation step failed", .{})) {
        .row => {},
        .done => exit.die(ctx, error.Unexpected, "run start: uid generation returned no row", .{}),
    }
    const run_uid = uid_stmt.columnTextAlloc(0, ctx.allocator) catch
        exit.die(ctx, error.OutOfMemory, "run start: uid alloc failed", .{});
    defer ctx.allocator.free(run_uid);

    // Arm: use workflow name when supplied, else "op".
    const arm: []const u8 = if (args.workflow) |w| w else "op";

    const res = engine.runs.lifecycle.start(d, ctx.allocator, .{
        .run_uid = run_uid,
        .plan_id = args.plan,
        .arm = arm,
        .base_sha = "",
        .config_hash = "",
    }) catch |e| switch (e) {
        error.DuplicateRunUid => exit.die(
            ctx,
            error.AlreadyExists,
            "run start: collision on generated run_uid (retry)",
            .{},
        ),
        else => exit.die(ctx, e, "run start: {s}", .{@errorName(e)}),
    };
    defer res.deinit(ctx.allocator);

    const w = ctx.stdout;
    try w.print("{{", .{});
    try w.print("\"run_uid\":", .{});
    try output.writeJsonString(w, res.run_uid);
    try w.print(",\"plan_id\":{d}", .{args.plan});
    try w.print(",\"arm\":", .{});
    try output.writeJsonString(w, arm);
    try w.print("}}\n", .{});
}
