//! handlers/peek — `planar-agent peek <plan-id>`
//!
//! Read-only "what's next" — runs the same SELECT pullNext uses as its
//! candidate filter but commits no writes. JSON shape:
//!   { ok: bool, no_work: bool, task?: Task }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const json = @import("json.zig");

const atomic = engine.runtime.agentactivity.atomic;
const task_mod = engine.planning.task;

pub const verb: cli.Cmd = .{
    .name = "peek",
    .desc = "Read-only what's-next selector (same query as pull, no writes).",
    .flags = &.{
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "plan_id", .kind = .int, .required = true, .desc = "Plan id to peek into" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"peek"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const result = atomic.peekNext(d, args.plan_id) catch |e|
        exit.die(ctx, e, "peek: {s}", .{@errorName(e)});

    if (result.no_work) {
        if (args.json) {
            try ctx.stdout.print("{{\"ok\":true,\"no_work\":true}}\n", .{});
        } else {
            try ctx.stdout.print("no_work\n", .{});
        }
        return;
    }

    const task = task_mod.show(d, ctx.allocator, result.task_id) catch |e|
        exit.die(ctx, e, "task show: {s}", .{@errorName(e)});
    defer task_mod.deinit(task, ctx.allocator);

    if (args.json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"no_work\":false,\"task\":", .{});
        try json.writeTask(w, task);
        try w.print("}}\n", .{});
    } else {
        try ctx.stdout.print("next: task:{d} status:{s}\n", .{ task.id, @tagName(task.status) });
    }
}
