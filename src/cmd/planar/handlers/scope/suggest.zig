//! handlers/scope/suggest — `planar scope suggest`
//!
//! Inspect the cwd and print the associations the cwd project is
//! already a member of. Read-only — to write to one of the proposed
//! scopes, pass `--scope <slug>` to any verb or cd into that scope.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "scope", "suggest" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const cwd = std.Io.Dir.realPathFileAlloc(.cwd(), ctx.io, ".", ctx.allocator) catch |e| {
        try ctx.stderr.print("error: getting cwd: {s}\n", .{@errorName(e)});
        return e;
    };
    defer ctx.allocator.free(cwd);

    const suggestions = engine.identity.scope.suggest(d, ctx.allocator, cwd) catch |e| {
        try ctx.stderr.print("error: generating scope suggestions: {s}\n", .{@errorName(e)});
        return e;
    };
    defer engine.identity.scope.deinitSuggestions(suggestions, ctx.allocator);

    if (args.json) {
        if (suggestions.len == 0) {
            try ctx.stdout.print("{{\"proposals\":[]}}\n", .{});
            return;
        }
        for (suggestions) |s| {
            try ctx.stdout.print(
                "{{\"slug\":\"{s}\",\"association_id\":{d},\"reason\":\"{s}\"}}\n",
                .{ s.slug, s.association_id, s.reason },
            );
        }
        return;
    }

    if (suggestions.len == 0) {
        try ctx.stdout.print("no scope suggestions for cwd\n", .{});
        return;
    }
    try ctx.stdout.print("suggested scope based on cwd:\n", .{});
    for (suggestions) |s| {
        try ctx.stdout.print("  {s}  ({s})\n", .{ s.slug, s.reason });
    }
}
