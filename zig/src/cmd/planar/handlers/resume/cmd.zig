//! handlers/resume/cmd.zig — `planar resume [<task-id>]` + `planar resume validate`
//!
//! Top-level run produces the 8-section resume packet. When task-id is
//! omitted, falls back to the most-recent active task in cwd scope.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");

const validate = @import("validate.zig");

pub const verb: cli.Cmd = .{
    .name = "resume",
    .desc = "Produce a structured resume packet for the specified task.",
    .long_desc = "Produce a structured 8-section resume packet for the specified\n  task.\n\n  The packet contains:\n    1. Identity       — task id, plan id, title, scope\n    2. State          — status, next_action, last action\n    3. Plan position  — parent plan, completed/current/remaining steps\n    4. Operational    — external_links for the task; refreshed if stale\n    5. Recent activity — session entries from recent sessions\n    6. Decisions and questions\n    7. Linked artifacts\n    8. Audit footer   — previous session vendor and timestamp, plus\n                        the active claim's worktree path (when held)\n                        so the resumer can prepend `cd <path>`",
    .flags = &.{
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "task-id", .kind = .string, .required = false },
    },
    .run = cli.handler(handle),
    .cmds = &.{
        .{
            .name = "validate",
            .desc = "Check if a task is resumable.",
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(validate.handle),
        },
    },
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"resume"}, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var task_id: i64 = 0;
    if (args.task_id) |raw| {
        task_id = std.fmt.parseInt(i64, raw, 10) catch
            exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{raw});
    } else {
        const cwd = scope_mod.operatorCwd(ctx.allocator, ctx.io) catch |e|
            exit.die(ctx, e, "resolve cwd for resume: {s}", .{@errorName(e)});
        defer ctx.allocator.free(cwd);
        const read_scopes = scope_mod.resolveForReadSet(ctx, cwd, null) catch |e|
            exit.die(ctx, e, "resolve scope for resume: {s}", .{@errorName(e)});
        defer ctx.allocator.free(read_scopes);
        if (read_scopes.len == 0) {
            exit.die(
                ctx,
                error.NoReadScope,
                "cwd is not inside any registered Planar scope; cd into a registered scope or pass <task-id> explicitly",
                .{},
            );
        }

        // Sessions are the activity clock for resume. Walk newest-first and
        // select the first still-active task whose stored scope is in the
        // cwd-derived read set. Global tasks only match an explicitly global
        // read scope; they never leak into a repo/association cwd.
        const sql: [:0]const u8 =
            \\select s.task_id, t.scope_kind, t.scope_id
            \\from sessions s
            \\join tasks t on t.id = s.task_id
            \\where s.task_id is not null
            \\  and t.status in ('todo', 'doing', 'blocked')
            \\order by s.started_at desc, s.id desc
        ;
        var stmt = d.prepare(sql) catch |e| exit.die(ctx, e, "lookup recent session: {s}", .{@errorName(e)});
        defer stmt.finalize();
        while (true) {
            switch (stmt.step() catch |e| exit.die(ctx, e, "lookup step: {s}", .{@errorName(e)})) {
                .done => exit.die(
                    ctx,
                    error.NotFound,
                    "no active task in cwd-derived scope; pass <task-id> explicitly",
                    .{},
                ),
                .row => {
                    const kind = stmt.columnTextAlloc(1, ctx.allocator) catch |e|
                        exit.die(ctx, e, "read task scope: {s}", .{@errorName(e)});
                    defer ctx.allocator.free(kind);
                    const scope_id = stmt.columnIntOpt(2);
                    if (matchesReadScope(kind, scope_id, read_scopes)) {
                        task_id = stmt.columnInt(0);
                        break;
                    }
                },
            }
        }
    }

    const packet = engine.runtime.@"resume".buildPacket(d, ctx.allocator, task_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "task {d} not found", .{task_id}),
        else => exit.die(ctx, e, "resume: {s}", .{@errorName(e)}),
    };
    defer engine.runtime.@"resume".deinitPacket(packet, ctx.allocator);

    if (args.json) {
        try renderJson(ctx, packet);
        return;
    }
    try renderText(ctx, packet);
}

fn matchesReadScope(kind: []const u8, id: ?i64, scopes: []const scope_mod.ReadScope) bool {
    for (scopes) |scope| {
        switch (scope.kind) {
            .global => if (std.mem.eql(u8, kind, "global") and id == null) return true,
            .association => if (std.mem.eql(u8, kind, "association") and id == scope.id) return true,
            .repo => if (std.mem.eql(u8, kind, "repo") and id == scope.id) return true,
        }
    }
    return false;
}

fn renderText(ctx: *const runtime.Ctx, p: engine.runtime.@"resume".Packet) !void {
    try ctx.stdout.print("=== Resume Packet: task {d} ===\n\n", .{@as(u64, @intCast(p.identity.task_id))});

    try ctx.stdout.print("## 1. Identity\n", .{});
    try ctx.stdout.print("  task:   {d}  \"{s}\"\n", .{ @as(u64, @intCast(p.identity.task_id)), p.identity.title });
    try ctx.stdout.print("  status: {s}\n", .{p.identity.status});
    try ctx.stdout.print("  scope:  {s}", .{p.identity.scope_kind});
    if (p.identity.scope_id) |sid| try ctx.stdout.print(":{d}", .{sid});
    try ctx.stdout.print("\n", .{});
    if (p.identity.plan_id) |pid| try ctx.stdout.print("  plan:   {d}\n", .{@as(u64, @intCast(pid))});
    try ctx.stdout.print("\n", .{});

    try ctx.stdout.print("## 2. State\n", .{});
    try ctx.stdout.print("  status:      {s}\n", .{p.state.status});
    if (p.state.next_action.len > 0) {
        try ctx.stdout.print("  next_action: {s}\n", .{p.state.next_action});
    } else {
        try ctx.stdout.print("  next_action: (not set)\n", .{});
    }
    if (p.state.last_action_at.len > 0) {
        try ctx.stdout.print("  last_action: {s}  [{s}]\n", .{ p.state.last_action_body, p.state.last_action_at });
    }
    try ctx.stdout.print("\n", .{});

    try ctx.stdout.print("## 3. Plan Position\n", .{});
    if (p.plan.plan_id) |pid| {
        try ctx.stdout.print("  plan: {d} \"{s}\"\n", .{ @as(u64, @intCast(pid)), p.plan.plan_title });
        try ctx.stdout.print("  completed: {d}  current: {d}  remaining: {d}\n", .{
            p.plan.completed.len, p.plan.current.len, p.plan.remaining.len,
        });
    } else {
        try ctx.stdout.print("  (no parent plan)\n", .{});
    }
    try ctx.stdout.print("\n", .{});

    try ctx.stdout.print("## 4. Operational Plane\n", .{});
    if (p.operational_plane.refresh_note.len > 0) {
        try ctx.stdout.print("  note: {s}\n", .{p.operational_plane.refresh_note});
    }
    if (p.operational_plane.links.len == 0) {
        try ctx.stdout.print("  (no external links)\n", .{});
    } else {
        for (p.operational_plane.links) |l| {
            try ctx.stdout.print(
                "  link {d}: {s} [{s}]  {s}\n",
                .{ @as(u64, @intCast(l.link_id)), l.external_id, l.sync_status, l.external_url },
            );
        }
    }
    try ctx.stdout.print("\n", .{});

    try ctx.stdout.print("## 5. Recent Activity ({d} entries)\n", .{p.recent_activity.len});
    if (p.recent_activity.len == 0) try ctx.stdout.print("  (none)\n", .{});
    for (p.recent_activity) |e| {
        try ctx.stdout.print("  [{s}]  session:{d}  {s}  — {s}\n", .{
            e.prefix, @as(u64, @intCast(e.session_id)), e.created_at, e.body,
        });
    }
    try ctx.stdout.print("\n", .{});

    try ctx.stdout.print("## 6. Decisions ({d}) and Questions ({d})\n", .{ p.decisions.len, p.questions.len });
    for (p.decisions) |d| try ctx.stdout.print("  decision {d}: \"{s}\"  [{s}]\n", .{ @as(u64, @intCast(d.id)), d.title, d.status });
    for (p.questions) |q| try ctx.stdout.print("  question {d}: \"{s}\"  [{s}]\n", .{ @as(u64, @intCast(q.id)), q.title, q.status });
    if (p.decisions.len == 0 and p.questions.len == 0) try ctx.stdout.print("  (none)\n", .{});
    try ctx.stdout.print("\n", .{});

    try ctx.stdout.print("## 7. Linked Artifacts ({d})\n", .{p.artifacts.len});
    for (p.artifacts) |a| try ctx.stdout.print("  {s} artifact:{d} \"{s}\"  [{s}]\n", .{ a.relationship, @as(u64, @intCast(a.artifact_id)), a.title, a.kind });
    if (p.artifacts.len == 0) try ctx.stdout.print("  (none)\n", .{});
    try ctx.stdout.print("\n", .{});

    try ctx.stdout.print("## 8. Audit Footer\n", .{});
    if (p.audit) |au| {
        try ctx.stdout.print("  session: {d}  vendor: {s}  started: {s}\n", .{
            @as(u64, @intCast(au.session_id)), au.vendor, au.started_at,
        });
    } else {
        try ctx.stdout.print("  (no prior session)\n", .{});
    }
    if (p.active_claim) |ac| {
        try ctx.stdout.print("  active claim: {d}  vendor: {s}\n", .{
            @as(u64, @intCast(ac.claim_id)), ac.vendor,
        });
        if (ac.worktree_path.len > 0) {
            try ctx.stdout.print("  worktree: {s}\n", .{ac.worktree_path});
            // Operator-facing cue. Skill prose (pl-resume) instructs the
            // resumer to prepend this `cd` to the resume flow.
            try ctx.stdout.print("  cd: {s}\n", .{ac.worktree_path});
        }
        if (ac.branch.len > 0) try ctx.stdout.print("  branch: {s}\n", .{ac.branch});
        if (ac.repo_root.len > 0) try ctx.stdout.print("  repo_root: {s}\n", .{ac.repo_root});
    }
    // Cold-start fallback: if no active claim carried a worktree path
    // but a prior handoff persisted one, surface it under a clearly
    // distinct label so the operator knows the value is recovered
    // from a released claim, not a live one. Plan 297 followup t#2947.
    if (p.from_handoff) |fh| {
        const ac_has_wt = if (p.active_claim) |ac| ac.worktree_path.len > 0 else false;
        if (!ac_has_wt) {
            try ctx.stdout.print("  from handoff: {d}\n", .{@as(u64, @intCast(fh.handoff_id))});
            if (fh.worktree_path.len > 0) {
                try ctx.stdout.print("  worktree: {s}\n", .{fh.worktree_path});
                try ctx.stdout.print("  cd: {s}\n", .{fh.worktree_path});
            }
            if (fh.branch.len > 0) try ctx.stdout.print("  branch: {s}\n", .{fh.branch});
            if (fh.repo_root.len > 0) try ctx.stdout.print("  repo_root: {s}\n", .{fh.repo_root});
        }
    }
    try ctx.stdout.print("\n", .{});
}

fn renderJson(ctx: *const runtime.Ctx, p: engine.runtime.@"resume".Packet) !void {
    // Use std.json.Stringify on the Packet — its fields are already
    // tagged with the wire names we want.
    try std.json.Stringify.value(p, .{}, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}
