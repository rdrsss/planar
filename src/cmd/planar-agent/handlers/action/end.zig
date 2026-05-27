//! handlers/action/end — `planar-agent action end --action <id>`
//!
//! Close a previously-started nested action. JSON: { ok, action_id, action }.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");
const json = @import("../json.zig");

const store = engine.runtime.agentactivity.store;
const types = engine.runtime.agentactivity.types;

pub const verb: cli.Cmd = .{
    .name = "end",
    .desc = "Close a nested action started under a claim.",
    .flags = &.{
        .{ .long = "--action", .kind = .int, .required = true, .desc = "Action id returned by `action start`" },
        .{ .long = "--outcome", .kind = .string, .default = .{ .string = "ok" }, .desc = "ok | error | aborted | timeout (default ok)" },
        .{ .long = "--summary", .kind = .string, .desc = "Optional free-text summary recorded on the action" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "action", "end" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const outcome = types.Outcome.fromText(args.outcome) orelse
        exit.die(ctx, error.InvalidInput, "unknown --outcome '{s}'", .{args.outcome});

    store.endAction(d, ctx.allocator, args.action, outcome, args.summary) catch |e|
        exit.die(ctx, e, "endAction: {s}", .{@errorName(e)});

    const action = store.getActionById(d, ctx.allocator, args.action) catch |e|
        exit.die(ctx, e, "getActionById: {s}", .{@errorName(e)});
    defer action.deinit(ctx.allocator);

    if (args.json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"action_id\":{d},\"action\":", .{args.action});
        try json.writeAction(w, action);
        try w.print("}}\n", .{});
    } else {
        try ctx.stdout.print("ok action:{d} outcome:{s}\n", .{ args.action, outcome.toText() });
    }
}
