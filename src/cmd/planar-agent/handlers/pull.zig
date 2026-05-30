//! handlers/pull — `planar-agent pull <plan-id> [flags]`
//!
//! Atomic operation: pick the next eligible task, claim it exclusively,
//! flip task to `doing`, insert action row. All four side effects commit
//! together via `agentactivity.atomic.pullNext`.
//!
//! Returns either `{ok:true, no_work:true}` (no eligible task — writes
//! NOTHING) or `{ok:true, claim_token, claim, task, action_id}`.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const json = @import("json.zig");
const util = @import("util.zig");

const atomic = engine.runtime.agentactivity.atomic;
const types = engine.runtime.agentactivity.types;
const session_mod = engine.runtime.session;
const task_mod = engine.planning.task;

pub const verb: cli.Cmd = .{
    .name = "pull",
    .desc = "Atomically pick the next eligible task, claim it, and flip status to doing.",
    .flags = &.{
        .{ .long = "--vendor", .kind = .string, .default = .{ .string = "planar-agent" }, .desc = "Vendor tag (default: planar-agent)" },
        .{ .long = "--vendor-session", .kind = .string, .desc = "Vendor session id (e.g. claude:s1)" },
        .{ .long = "--role", .kind = .string, .desc = "Role name (planner|coder|reviewer|test_coder|...)" },
        .{ .long = "--ttl", .kind = .string, .default = .{ .string = "600" }, .desc = "Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)" },
        .{ .long = "--purpose", .kind = .string, .desc = "Free-text purpose recorded on the claim" },
        .{ .long = "--base-ref", .kind = .string, .desc = "Git ref the work is based on" },
        .{ .long = "--worktree", .kind = .string, .desc = "Worktree id or path for isolation context" },
        .{ .long = "--repo-root", .kind = .string, .desc = "Absolute path of checkout to probe locality against" },
        .{ .long = "--no-locality-probe", .kind = .bool, .default = .{ .bool = false }, .desc = "Skip the git locality probe" },
        .{ .long = "--metadata", .kind = .string, .desc = "Opaque text (typically JSON) persisted on the dispatch action row; validated as well-formed JSON when supplied" },
        .{ .long = "--parent-action", .kind = .int, .desc = "Parent action id; wires the new action as a child of this action in `planar-watch tree` (cross-session hierarchy)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "plan-id", .kind = .int, .required = true, .desc = "Plan id to pull from" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"pull"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Per-role probe default: planner/coder/reviewer/test_coder probe;
    // tool_call/heartbeat skip. pull picks a role kind (coder by
    // default) so its default is probe-on.
    const action_kind: types.ActionKind = if (args.role) |r|
        types.ActionKind.fromText(r) orelse .coder
    else
        .coder;
    const default_probe = action_kind.probeDefault();
    const skip_probe = args.no_locality_probe or !default_probe;
    const loc = util.resolveLocality(ctx, args.repo_root, skip_probe);
    defer loc.deinit(ctx.allocator);

    // Worktree id or path. If parseable as int → id; else path.
    var worktree_id: ?i64 = null;
    var worktree_path: ?[]const u8 = null;
    if (args.worktree) |w| {
        if (std.fmt.parseInt(i64, w, 10)) |n| {
            worktree_id = n;
        } else |_| {
            worktree_path = w;
        }
    }

    const session_id = session_mod.ensureActive(d, ctx.allocator, args.vendor, args.vendor_session) catch |e|
        exit.die(ctx, e, "ensureActive: {s}", .{@errorName(e)});

    const ttl_secs = cli.duration.parseSeconds(args.ttl) catch |e|
        exit.die(ctx, e, "invalid --ttl '{s}': expected bare seconds (e.g. 600) or suffixed duration (e.g. 10m, 1h, 500ms)", .{args.ttl});

    // Validate --metadata is well-formed JSON at parse time. Engine
    // stores it opaquely; failing here keeps malformed text out of the
    // database.
    if (args.metadata) |m| {
        var parsed = std.json.parseFromSlice(std.json.Value, ctx.allocator, m, .{}) catch
            exit.die(ctx, error.InvalidInput, "--metadata is not valid JSON: {s}", .{m});
        parsed.deinit();
    }

    // Validate --parent-action: must be a positive integer when supplied.
    // Optionally verify the parent action exists (cheap select 1 probe).
    if (args.parent_action) |pa| {
        if (pa <= 0) {
            exit.die(ctx, error.InvalidInput, "--parent-action must be a positive integer (got {d})", .{pa});
        }
        // Existence probe: reject an unknown parent action id to surface
        // wiring errors early (e.g. wrong action id in an orchestrator script).
        const exists = blk: {
            var stmt = d.prepare("select 1 from agent_actions where id = ? limit 1") catch break :blk false;
            defer stmt.finalize();
            stmt.bind(&.{.{ .int = pa }}) catch break :blk false;
            break :blk switch (stmt.step() catch break :blk false) {
                .row => true,
                .done => false,
            };
        };
        if (!exists) {
            exit.die(ctx, error.NotFound, "--parent-action {d}: action not found", .{pa});
        }
    }

    const result = atomic.pullNext(d, ctx.allocator, .{
        .plan_id = args.plan_id,
        .session_id = session_id,
        .vendor = args.vendor,
        .vendor_session_id = args.vendor_session,
        .role = args.role,
        .worktree_id = worktree_id,
        .worktree_path = worktree_path,
        .purpose = args.purpose,
        .base_ref = args.base_ref,
        .ttl_secs = ttl_secs,
        .locality = loc,
        .action_kind = action_kind,
        .metadata = args.metadata,
        .parent_action_id = args.parent_action,
    }) catch |e| exit.die(ctx, e, "pull: {s}", .{@errorName(e)});
    defer result.deinit(ctx.allocator);

    if (result.no_work) {
        if (args.json) {
            try ctx.stdout.print("{{\"ok\":true,\"no_work\":true}}\n", .{});
        } else {
            try ctx.stdout.print("no_work\n", .{});
        }
        return;
    }

    // Fetch the task row so the response includes the canonical Task.
    const task = task_mod.show(d, ctx.allocator, result.task_id) catch |e|
        exit.die(ctx, e, "task show: {s}", .{@errorName(e)});
    defer task_mod.deinit(task, ctx.allocator);

    if (args.json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"no_work\":false,\"claim_token\":", .{});
        try std.json.Stringify.encodeJsonString(result.claim.?.claim_token, .{}, w);
        try w.print(",\"claim\":", .{});
        try json.writeClaim(w, result.claim.?, null);
        try w.print(",\"task\":", .{});
        try json.writeTask(w, task);
        try w.print(",\"action_id\":{d}}}\n", .{result.action_id});
    } else {
        try ctx.stdout.print("pulled task:{d} claim:{s} action:{d}\n", .{
            result.task_id,
            result.claim.?.claim_token,
            result.action_id,
        });
    }
}
