//! handlers/explore.zig — `planar explore [--plan <id>] [--task <id>] [--scope <s>] [--plain]`
//!
//! Explicit alias for the interactive cockpit. Equivalent to bare `planar`
//! on a TTY, but always available by name for discoverability and for
//! forcing the cockpit where bare-invocation TTY detection might not apply
//! (e.g. inside a tmux pane where argv[0] detection is indirect).
//!
//! `--plan`, `--task`, and `--scope` seed the initial view and selection.
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
    \\  --plan, --task, and --scope seed the initial focus.
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
            const plan_id = if (args.plan) |raw| std.fmt.parseInt(i64, raw, 10) catch
                exit.die(ctx, error.InvalidInput, "--plan must be an integer", .{}) else null;
            const task_id = if (args.task) |raw| std.fmt.parseInt(i64, raw, 10) catch
                exit.die(ctx, error.InvalidInput, "--task must be an integer", .{}) else null;

            const env_map = ctx.environ_map orelse {
                // environ_map is always set in the planar binary; this branch
                // is a defensive fallback in case of unexpected initialization.
                exit.die(ctx, error.NoEnvironMap, "internal: environ_map not available", .{});
            };
            // M3 (task 4055): open the DB and run the live cockpit with
            // the wake-thread redraw loop. openDb surfaces clean errors
            // before the alt-screen is entered.
            // ctx.db_path is [:0]const u8 — coerce to []const u8.
            const db_path_slice: []const u8 = ctx.db_path;
            var db_handle = cockpit_app.openDb(ctx.io, db_path_slice, ctx.allocator) catch |e| {
                const msg = switch (e) {
                    cockpit_app.DbOpenError.DbMissing => "database not found — run `planar init` first",
                    cockpit_app.DbOpenError.DbLocked => "database is locked by another process",
                    cockpit_app.DbOpenError.DbSchemaMismatch => "database schema is newer than this binary — rebuild/reinstall planar",
                    cockpit_app.DbOpenError.DbOpenFailed => "failed to open database",
                };
                exit.die(ctx, e, "{s}", .{msg});
            };
            defer db_handle.close();
            cockpit_app.run(ctx.io, ctx.allocator, env_map, ctx.environ, db_path_slice, &db_handle, .{
                .plan_id = plan_id,
                .task_id = task_id,
                .scope = args.scope,
            }) catch |e| {
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
