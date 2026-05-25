//! handlers/resume/validate — `planar resume validate <task-id>`
//!
//! Reports failures and exits 1 if not resumable (cperr.User path).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "resume", "validate" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    const r = engine.runtime.@"resume".validate(d, ctx.allocator, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "task {d} not found", .{id}),
        else => exit.die(ctx, e, "resume validate: {s}", .{@errorName(e)}),
    };
    defer engine.runtime.@"resume".deinitResult(r, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print(
            "{{\"task_id\":{d},\"resumable\":{s},\"failures\":",
            .{ r.task_id, if (r.resumable) "true" else "false" },
        );
        // Match Go's resume validate --json shape: empty failures serialize
        // as `null`, not `[]`. Cross-binary parity is load-bearing here.
        if (r.failures.len == 0) {
            try ctx.stdout.print("null}}\n", .{});
        } else {
            try ctx.stdout.print("[", .{});
            for (r.failures, 0..) |f, i| {
                if (i > 0) try ctx.stdout.print(",", .{});
                try ctx.stdout.print("{{\"check\":", .{});
                try output.writeJsonString(ctx.stdout, f.check);
                try ctx.stdout.print(",\"message\":", .{});
                try output.writeJsonString(ctx.stdout, f.message);
                try ctx.stdout.print(",\"remediation\":", .{});
                try output.writeJsonString(ctx.stdout, f.remediation);
                try ctx.stdout.print("}}", .{});
            }
            try ctx.stdout.print("]}}\n", .{});
        }
        if (!r.resumable) exit.die(ctx, error.NotFound, "task {d} is not resumable", .{id});
        return;
    }

    if (r.resumable) {
        try ctx.stdout.print("OK task:{d} is resume-ready\n", .{@as(u64, @intCast(id))});
        return;
    }
    try ctx.stdout.print("FAIL task:{d} is not resumable:\n", .{@as(u64, @intCast(id))});
    for (r.failures) |f| {
        try ctx.stdout.print("  - {s} → run: {s}\n", .{ f.message, f.remediation });
    }
    exit.die(ctx, error.NotFound, "task {d} is not resumable", .{id});
}
