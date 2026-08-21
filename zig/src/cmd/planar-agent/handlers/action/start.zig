//! handlers/action/start — `planar-agent action start --claim <token> --kind <kind>`
//!
//! Insert a nested agent_actions row attached to an existing claim's
//! top-level action via parent_action_id. JSON: { ok, action_id, action }.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");
const json = @import("../json.zig");
const util = @import("../util.zig");

const store = engine.runtime.agentactivity.store;
const types = engine.runtime.agentactivity.types;

pub const verb: cli.Cmd = .{
    .name = "start",
    .desc = "Start a nested action under a claim (child of the claim's role action).",
    .flags = &.{
        .{ .long = "--claim", .kind = .string, .required = true, .desc = "Claim token the action attaches to" },
        .{ .long = "--kind", .kind = .string, .required = true, .desc = "Action kind (planner|coder|tool_call|heartbeat|...)" },
        .{ .long = "--entity", .kind = .string, .desc = "Optional entity ref kind:id" },
        .{ .long = "--vendor-role", .kind = .string, .desc = "Optional vendor role tag" },
        .{ .long = "--repo-root", .kind = .string, .desc = "Absolute path of checkout to probe locality against" },
        .{ .long = "--no-locality-probe", .kind = .bool, .default = .{ .bool = false }, .desc = "Skip the git locality probe" },
        .{ .long = "--metadata", .kind = .string, .desc = "Opaque text (typically JSON) persisted on the action row; validated as well-formed JSON when supplied" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "action", "start" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const claim = store.getClaimByToken(d, ctx.allocator, args.claim) catch |e|
        exit.die(ctx, e, "claim lookup: {s}", .{@errorName(e)});
    defer claim.deinit(ctx.allocator);

    const action_kind = types.ActionKind.fromText(args.kind) orelse
        exit.die(ctx, error.InvalidInput, "unknown action kind '{s}'", .{args.kind});

    // Per-action-kind probe default; --no-locality-probe overrides.
    const skip_probe = args.no_locality_probe or !action_kind.probeDefault();
    const loc = util.resolveLocality(ctx, args.repo_root, skip_probe);
    defer loc.deinit(ctx.allocator);

    // Resolve parent_action_id: the latest open action on this claim
    // (typically the role action started by pull). NULL when none.
    const parent_id: ?i64 = blk: {
        var stmt = d.prepare(
            "select id from agent_actions where claim_id = ? and ended_at is null order by started_at desc limit 1",
        ) catch break :blk null;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = claim.id }}) catch break :blk null;
        switch (stmt.step() catch break :blk null) {
            .row => break :blk stmt.columnInt(0),
            .done => break :blk null,
        }
    };

    // Optional entity ref.
    var entity_kind_a: ?types.ActionEntityKind = null;
    var entity_id_a: ?i64 = null;
    if (args.entity) |e| {
        const colon = std.mem.indexOfScalar(u8, e, ':') orelse
            exit.die(ctx, error.InvalidInput, "invalid --entity '{s}'; expected kind:id", .{e});
        const kind_str = e[0..colon];
        const id_str = e[colon + 1 ..];
        const eid = std.fmt.parseInt(i64, id_str, 10) catch
            exit.die(ctx, error.InvalidInput, "invalid --entity id '{s}'", .{id_str});
        const ek = types.ActionEntityKind.fromText(kind_str) orelse
            exit.die(ctx, error.InvalidInput, "unknown entity kind '{s}'", .{kind_str});
        entity_kind_a = ek;
        entity_id_a = eid;
    }

    // Validate --metadata is well-formed JSON at the CLI parse layer so
    // callers see a friendly error instead of opaque text round-tripping
    // through the engine. The engine stores it as opaque text.
    if (args.metadata) |m| {
        var parsed = std.json.parseFromSlice(std.json.Value, ctx.allocator, m, .{}) catch
            exit.die(ctx, error.InvalidInput, "--metadata is not valid JSON: {s}", .{m});
        parsed.deinit();
    }

    const id = store.startAction(d, ctx.allocator, .{
        .session_id = claim.session_id,
        .parent_action_id = parent_id,
        .claim_id = claim.id,
        .action_kind = action_kind,
        .entity_kind = entity_kind_a,
        .entity_id = entity_id_a,
        .vendor = claim.vendor,
        .vendor_role = args.vendor_role,
        .locality = loc,
        .metadata = args.metadata,
    }) catch |e| exit.die(ctx, e, "startAction: {s}", .{@errorName(e)});

    const action = store.getActionById(d, ctx.allocator, id) catch |e|
        exit.die(ctx, e, "getActionById: {s}", .{@errorName(e)});
    defer action.deinit(ctx.allocator);

    if (args.json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"action_id\":{d},\"action\":", .{id});
        try json.writeAction(w, action);
        try w.print("}}\n", .{});
    } else {
        try ctx.stdout.print("action:{d} kind:{s} claim:{s}\n", .{ id, action_kind.toText(), args.claim });
    }
}
