//! handlers/scope/removed — plan 153 M5 redirect stubs for use/pop/clear.

const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handleUse(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "scope", "use" }, args_ptr);
    const ctx = runtime.current();
    exit.die(
        ctx,
        error.InvalidInput,
        "`planar scope use` was removed in plan 153 M5; the active scope stack is gone. Pass --scope <slug> to individual verbs, or cd into a registered scope. Run `planar scope show` to inspect the cwd-derived scope.",
        .{},
    );
}

pub fn handlePop(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "scope", "pop" }, args_ptr);
    const ctx = runtime.current();
    exit.die(
        ctx,
        error.InvalidInput,
        "`planar scope pop` was removed in plan 153 M5; the active scope stack is gone. Pass --scope <slug> to individual verbs, or cd into a registered scope. Run `planar scope show` to inspect the cwd-derived scope.",
        .{},
    );
}

pub fn handleClear(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "scope", "clear" }, args_ptr);
    const ctx = runtime.current();
    exit.die(
        ctx,
        error.InvalidInput,
        "`planar scope clear` was removed in plan 153 M5; the active scope stack is gone. Pass --scope <slug> to individual verbs, or cd into a registered scope. Run `planar scope show` to inspect the cwd-derived scope.",
        .{},
    );
}
