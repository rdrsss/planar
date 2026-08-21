//! handlers/claim — `planar-agent claim --entity <ref> [flags]`
//!
//! Direct claim path for orchestrator dispatch (caller already knows the
//! target entity by id). Task claims atomically transition `todo` → `doing`
//! by default. `--no-transition` retains the pure claim primitive; plan and
//! plan-step claims never alter entity state.
//!
//! `--entity <ref>` accepts `task:<id>` / `plan:<id>` / `plan_step:<id>`
//! and strictly refuses other prefixes (util.parseEntityRef).

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

pub const verb: cli.Cmd = .{
    .name = "claim",
    .desc = "Direct entity claim; task claims atomically transition todo to doing by default.",
    .flags = &.{
        .{ .long = "--entity", .kind = .string, .required = true, .desc = "Entity ref: task:<id> | plan:<id> | plan_step:<id>" },
        .{ .long = "--vendor", .kind = .string, .default = .{ .string = "planar-agent" }, .desc = "Vendor tag (default: planar-agent)" },
        .{ .long = "--vendor-session", .kind = .string, .desc = "Vendor session id (e.g. claude:s1)" },
        .{ .long = "--role", .kind = .string, .desc = "Role name (planner|coder|reviewer|test_coder|...)" },
        .{ .long = "--model", .kind = .string, .desc = "Model actually used, recorded verbatim as an opaque string. Never validated against a supported list." },
        .{ .long = "--ttl", .kind = .string, .default = .{ .string = "600" }, .desc = "Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)" },
        .{ .long = "--purpose", .kind = .string, .desc = "Free-text purpose recorded on the claim" },
        .{ .long = "--worktree", .kind = .string, .desc = "Worktree id or path for isolation context" },
        .{ .long = "--repo-root", .kind = .string, .desc = "Absolute path of checkout to probe locality against" },
        .{ .long = "--no-locality-probe", .kind = .bool, .default = .{ .bool = false }, .desc = "Skip the git locality probe" },
        .{ .long = "--no-transition", .kind = .bool, .default = .{ .bool = false }, .desc = "Claim without changing task status (plan and plan_step are always unchanged)" },
        .{ .long = "--force", .kind = .bool, .default = .{ .bool = false }, .desc = "Take over an existing live claim (operator recovery)" },
        .{ .long = "--run", .kind = .int, .desc = "workflow_runs.id to associate with this claim (populated by an external workflow harness; omit for interactive claims)" },
        .{ .long = "--stage", .kind = .string, .desc = "Workflow stage name (e.g. code, review) to record on the claim; requires --run" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"claim"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Validate: --stage requires --run (spec invariant).
    if (args.stage != null and args.run == null)
        exit.die(
            ctx,
            error.InvalidInput,
            "--stage requires --run: provide a workflow_runs.id via --run <id>",
            .{},
        );

    const eref = util.parseEntityRef(args.entity) catch |e|
        exit.die(ctx, e, "invalid --entity '{s}' ({s}); expected task:<id>|plan:<id>|plan_step:<id>", .{ args.entity, @errorName(e) });

    const loc = util.resolveLocality(ctx, args.repo_root, args.no_locality_probe);
    defer loc.deinit(ctx.allocator);

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

    const c = atomic.claimEntity(d, ctx.allocator, .{
        .session_id = session_id,
        .entity_kind = eref.kind,
        .entity_id = eref.id,
        .vendor = args.vendor,
        .vendor_session_id = args.vendor_session,
        .role = args.role,
        .model = args.model,
        .worktree_id = worktree_id,
        .worktree_path = worktree_path,
        .purpose = args.purpose,
        .ttl_secs = ttl_secs,
        .locality = loc,
        .force = args.force,
        .run_id = args.run,
        .stage = args.stage,
    }, !args.no_transition) catch |e|
        exit.die(ctx, e, "claim: {s}", .{@errorName(e)});
    defer c.deinit(ctx.allocator);

    try emitClaim(ctx, c, args.json);
}

fn emitClaim(ctx: *const runtime.Ctx, c: types.Claim, use_json: bool) !void {
    if (use_json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"claim_token\":", .{});
        try std.json.Stringify.encodeJsonString(c.claim_token, .{}, w);
        try w.print(",\"claim\":", .{});
        try json.writeClaim(w, c, null);
        try w.print("}}\n", .{});
    } else {
        try ctx.stdout.print("claim:{s} entity:{s}:{d} status:{s}\n", .{
            c.claim_token,
            c.entity_kind.toText(),
            c.entity_id,
            c.status.toText(),
        });
    }
}
