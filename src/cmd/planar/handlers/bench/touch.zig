//! handlers/bench/touch — `planar bench touch <run-uid> --task <id>
//!   --path <p> --kind <declared|actual>`
//!
//! Records one (task, path) touch on the named run. `kind` is
//! enum-validated at this layer: only "declared" and "actual" are
//! accepted.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "bench", "touch" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Enum-validate kind at the parse layer.
    const kind = engine.runs.lifecycle.TouchKind.fromText(args.kind) orelse
        exit.die(
            ctx,
            error.InvalidInput,
            "bench touch: invalid --kind '{s}'; expected declared or actual",
            .{args.kind},
        );

    // Look up the run by uid.
    const run = engine.runs.lifecycle.showByUid(d, ctx.allocator, args.run_uid) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "bench touch: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "bench touch: {s}", .{@errorName(e)}),
    };
    defer engine.runs.lifecycle.deinit(run, ctx.allocator);

    _ = engine.runs.lifecycle.touch(d, run.id, args.task, args.path, kind) catch |e|
        exit.die(ctx, e, "bench touch: {s}", .{@errorName(e)});

    try ctx.stdout.print("ok\n", .{});
}
