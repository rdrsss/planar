//! handlers/plan/closeout — `planar plan closeout <plan-id> [--dry-run] [--json]`
//!
//! Delivery-evidence gate for plan closeout. Evaluates the DB-hard gate (all
//! tasks terminal, all descendants terminal, no live claims) and the advisory
//! git-evidence layer. In apply mode (no --dry-run), marks the plan `done`
//! when the hard gate passes.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "closeout" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    const dry_run = args.dry_run;
    const apply = !dry_run;
    const check_merge = args.check_merge;

    const result = engine.planning.closeout.evaluate(d, ctx.allocator, plan_id, apply, check_merge) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no plan with id {d}", .{plan_id}),
        else => exit.die(ctx, e, "plan closeout: {s}", .{@errorName(e)}),
    };
    defer result.deinit(ctx.allocator);

    if (args.json) {
        try emitJSON(ctx, result);
    } else {
        try emitText(ctx, result, dry_run);
    }

    // Non-zero exit when blocked — but ONLY on the apply path.
    // --dry-run is a preview: it exits 0 regardless of readiness so
    // callers (e.g. planar-execute workflows) can read the structured
    // {ready, blocked_by} JSON before deciding whether to proceed.
    // The apply path (no --dry-run) keeps the gated non-zero exit so
    // the operator sees a clear failure when the plan cannot be closed.
    if (!result.ready and apply) {
        // Flush before exit.
        exit.die(ctx, error.Conflict, "plan {d} is not ready to close ({d} reason(s))", .{
            plan_id,
            result.blocked_by.len,
        });
    }
}

// =========================================================================
// Text output
// =========================================================================

fn emitText(ctx: *const runtime.Ctx, result: engine.planning.closeout.CloseoutResult, dry_run: bool) !void {
    const w = ctx.stdout;

    const mode_label: []const u8 = if (dry_run) "[dry-run] " else "";

    if (result.applied) {
        try w.print("{s}plan {d}: marked done\n", .{ mode_label, result.plan_id });
    } else if (result.ready) {
        try w.print("{s}plan {d}: ready (already terminal — no change)\n", .{ mode_label, result.plan_id });
    } else {
        try w.print("{s}plan {d}: NOT ready to close\n", .{ mode_label, result.plan_id });
    }

    // Hard-evidence summary.
    const tc = result.hard_evidence.tasks;
    const dc = result.hard_evidence.descendants;
    const cc = result.hard_evidence.claims;

    const ft = result.hard_evidence.finalization_tasks;

    try w.print("\nhard gate:\n", .{});
    try w.print("  tasks:       open={d}  done={d}  cancelled={d}\n", .{ tc.open, tc.done, tc.cancelled });
    if (ft > 0) {
        try w.print("    (finalization tasks: {d} — merge/reconcile/finalize-prefixed)\n", .{ft});
    }
    try w.print("  descendants: open={d}  terminal={d}\n", .{ dc.open, dc.terminal });
    try w.print("  claims:      live={d}  stale={d}\n", .{ cc.live, cc.stale });

    if (result.blocked_by.len > 0) {
        try w.print("\nblocked by:\n", .{});
        for (result.blocked_by) |reason| {
            try w.print("  - {s}\n", .{reason});
        }
    }

    if (result.warnings.len > 0) {
        try w.print("\nwarnings:\n", .{});
        for (result.warnings) |warn| {
            try w.print("  ! {s}\n", .{warn});
        }
    }

    if (result.git_evidence.len > 0) {
        try w.print("\ngit evidence (advisory):\n", .{});
        for (result.git_evidence) |ev| {
            const branch_str = ev.branch orelse "(none)";
            const target_str = ev.target_branch orelse "(unknown)";
            try w.print("  repo: {s}\n", .{ev.repo_root});
            try w.print("    branch:  {s}  target: {s}\n", .{ branch_str, target_str });
            try w.print("    note:    {s}\n", .{ev.note});
        }
    }

    if (result.epic_merge) |em| {
        try w.print("\nepic-branch merge check (advisory):\n", .{});
        try w.print("  target: {s}\n", .{em.target_branch});
        try w.print("  note:   {s}\n", .{em.note});
    }
}

// =========================================================================
// JSON output
// =========================================================================

const GitEvidenceJSON = struct {
    repo_root: []const u8,
    branch: ?[]const u8,
    target_branch: ?[]const u8,
    base_merged: ?bool,
    branch_merged: ?bool,
    note: []const u8,
};

const HardEvidenceJSON = struct {
    tasks: struct {
        open: i64,
        done: i64,
        cancelled: i64,
    },
    descendants: struct {
        open: i64,
        terminal: i64,
    },
    claims: struct {
        live: i64,
        stale: i64,
    },
    /// Count of tasks with finalization slug prefixes (finalize-/merge-/reconcile-).
    /// Advisory — same terminal rules apply; this is labeling metadata only.
    finalization_tasks: i64,
};

const EpicMergeRollupJSON = struct {
    target_branch: []const u8,
    total_branches: i64,
    merged_count: i64,
    note: []const u8,
};

const CloseoutResultJSON = struct {
    plan: i64,
    ready: bool,
    applied: bool,
    hard_evidence: HardEvidenceJSON,
    blocked_by: []const []const u8,
    git_evidence: []const GitEvidenceJSON,
    /// Present when --check-merge was supplied; null otherwise.
    epic_merge: ?EpicMergeRollupJSON,
    warnings: []const []const u8,
};

fn emitJSON(ctx: *const runtime.Ctx, result: engine.planning.closeout.CloseoutResult) !void {
    const tc = result.hard_evidence.tasks;
    const dc = result.hard_evidence.descendants;
    const cc = result.hard_evidence.claims;

    // Build git_evidence slice for JSON serialization.
    var evs = try ctx.allocator.alloc(GitEvidenceJSON, result.git_evidence.len);
    defer ctx.allocator.free(evs);
    for (result.git_evidence, 0..) |e, i| {
        evs[i] = .{
            .repo_root = e.repo_root,
            .branch = e.branch,
            .target_branch = e.target_branch,
            .base_merged = e.base_merged,
            .branch_merged = e.branch_merged,
            .note = e.note,
        };
    }

    const epic_merge_json: ?EpicMergeRollupJSON = if (result.epic_merge) |em| .{
        .target_branch = em.target_branch,
        .total_branches = em.total_branches,
        .merged_count = em.merged_count,
        .note = em.note,
    } else null;

    const out = CloseoutResultJSON{
        .plan = result.plan_id,
        .ready = result.ready,
        .applied = result.applied,
        .hard_evidence = .{
            .tasks = .{ .open = tc.open, .done = tc.done, .cancelled = tc.cancelled },
            .descendants = .{ .open = dc.open, .terminal = dc.terminal },
            .claims = .{ .live = cc.live, .stale = cc.stale },
            .finalization_tasks = result.hard_evidence.finalization_tasks,
        },
        .blocked_by = result.blocked_by,
        .git_evidence = evs,
        .epic_merge = epic_merge_json,
        .warnings = result.warnings,
    };

    try std.json.Stringify.value(out, .{}, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}
