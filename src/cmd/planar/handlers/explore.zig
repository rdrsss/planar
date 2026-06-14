//! handlers/explore.zig — `planar explore [--plan <id>] [--task <id>] [--scope <s>] [--plain]`
//!
//! Explicit alias for the interactive cockpit. Equivalent to bare `planar`
//! on a TTY, but always available by name for discoverability and for
//! forcing the cockpit where bare-invocation TTY detection might not apply
//! (e.g. inside a tmux pane where argv[0] detection is indirect).
//!
//! M1: `--plan`, `--task`, and `--scope` flags parse correctly but seed
//! behavior is a stub — focus seeding is implemented in M2+ when views land.
//! `--plain` falls back to help/usage unconditionally.
//!
//! Falls back to help/usage whenever the terminal capability gate refuses:
//!   TERM=dumb, PLANAR_NO_TUI, non-TTY stdout, or --plain.

const std = @import("std");
const cli = @import("cli");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");
const cockpit_gate = @import("../cockpit/gate.zig");
const cockpit_app = @import("../cockpit/app.zig");

pub const verb: cli.Cmd = .{
    .name = "explore",
    .desc = "Launch the interactive cockpit (same as bare `planar` on a TTY).",
    .long_desc =
    \\Launch the interactive Planar cockpit.
    \\
    \\  Equivalent to invoking `planar` with no verb on a terminal. Use
    \\  `planar explore` when you want to force-launch the cockpit by name,
    \\  or from a context where bare-invocation detection may not fire.
    \\
    \\  --plan, --task, and --scope seed the initial focus. Seeding is a
    \\  stub in M1; the full view catalog lands in M2+.
    \\
    \\  Falls back to this help text when stdout is not a TTY, when TERM=dumb,
    \\  when PLANAR_NO_TUI is set, or when --plain is passed.
    ,
    .flags = &.{
        .{ .long = "--plan", .kind = .string, .desc = "Seed initial focus on this plan ID" },
        .{ .long = "--task", .kind = .string, .desc = "Seed initial focus on this task ID" },
        .{ .long = "--scope", .kind = .string, .desc = "Seed scope filter" },
        .{ .long = "--plain", .kind = .bool, .default = .{ .bool = false }, .desc = "Fall back to help/usage instead of launching the cockpit" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"explore"}, args_ptr);
    const ctx = runtime.current();

    const gate_result = cockpit_gate.check(ctx.io, ctx.environ, args.plain);
    switch (gate_result) {
        .launch_cockpit => {
            // M1: stub seed — plan/task/scope flags are parsed but not yet wired.
            // TODO(plan:609, task:4008): seed initial focus in M2+ when views land.
            _ = args.plan;
            _ = args.task;
            _ = args.scope;

            const env_map = ctx.environ_map orelse {
                // environ_map is always set in the planar binary; this branch
                // is a defensive fallback in case of unexpected initialization.
                exit.die(ctx, error.NoEnvironMap, "internal: environ_map not available", .{});
            };
            // M2: run without a DB until the DB path is wired through the
            // runtime context (M3). The full `run(io, alloc, env_map,
            // db_path, db_handle)` path is available but requires a live DB;
            // `runWithoutDb` is the scaffold entry point for now.
            // TODO(plan:591, task:4012): wire DB path from runtime context in M3.
            cockpit_app.runWithoutDb(ctx.io, ctx.allocator, env_map) catch |e| {
                exit.die(ctx, e, "cockpit error: {s}", .{@errorName(e)});
            };
        },
        .fallback_help => {
            // Print explore verb help and exit 0.
            const help_text = comptime cli.helpText(main.root, &.{"explore"});
            try ctx.stdout.print("{s}", .{help_text});
            try ctx.stdout.flush();
        },
    }
}
