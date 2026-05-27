//! handlers/association/detect — `planar assoc detect [--apply]`
//!
//! Propose (or apply) auto-detected associations for the current cwd.
//! Signals inspected: git remote origin (host:* and org:*), parent
//! directory basename (path:*), top-level ecosystem markers (lang:*).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "assoc", "detect" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const cwd = std.Io.Dir.realPathFileAlloc(.cwd(), ctx.io, ".", ctx.allocator) catch |e|
        exit.die(ctx, e, "getting cwd: {s}", .{@errorName(e)});
    defer ctx.allocator.free(cwd);

    const proposals = engine.identity.association.detectProposals(ctx.io, ctx.allocator, cwd) catch |e|
        exit.die(ctx, e, "detecting proposals: {s}", .{@errorName(e)});
    defer engine.identity.association.deinitProposals(proposals, ctx.allocator);

    // Enrich with DB state so the human / json output shows the action.
    engine.identity.association.enrichProposals(d, ctx.allocator, proposals, cwd) catch |e|
        exit.die(ctx, e, "enriching proposals: {s}", .{@errorName(e)});

    if (args.apply) {
        engine.identity.association.applyProposals(d, ctx.allocator, proposals, cwd) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "no project registered at cwd ({s}); run `planar init` first", .{cwd}),
            else => exit.die(ctx, e, "applying proposals: {s}", .{@errorName(e)}),
        };
        // After apply, re-enrich so member_exists reflects the new rows.
        engine.identity.association.enrichProposals(d, ctx.allocator, proposals, cwd) catch {};
    }

    if (args.json) {
        if (proposals.len == 0) {
            try ctx.stdout.print("{{\"proposals\":[]}}\n", .{});
            return;
        }
        for (proposals) |p| {
            try ctx.stdout.print(
                "{{\"slug\":\"{s}\",\"kind\":\"{s}\",\"source\":\"{s}\",\"reason\":\"{s}\",\"assoc_exists\":{},\"member_exists\":{},\"action\":\"{s}\"}}\n",
                .{
                    p.slug,
                    @tagName(p.kind),
                    p.source.toText(),
                    p.reason,
                    p.assoc_exists,
                    p.member_exists,
                    actionLabel(p),
                },
            );
        }
        return;
    }

    if (proposals.len == 0) {
        try ctx.stdout.print("no proposed associations\n", .{});
        return;
    }
    try ctx.stdout.print("proposed associations:\n", .{});
    for (proposals) |p| {
        try ctx.stdout.print("  {s:<24} ({s})  [{s}]\n", .{ p.slug, p.reason, actionLabel(p) });
    }
}

fn actionLabel(p: engine.identity.association.Proposal) []const u8 {
    if (p.member_exists) return "already a member";
    if (p.assoc_exists) return "already exists, will add";
    return "will create";
}
