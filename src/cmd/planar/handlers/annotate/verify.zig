//! handlers/annotate/verify — verify annotation anchors against
//! workspace state.
//!
//! For each active annotation in the filter, compute an anchor-state
//! report: `fresh` (file exists, hash matches), `drifted` (file
//! exists, hash differs), `stale` (file missing). Reports are emitted
//! as a flat JSON array (or one-line-per-row text).
//!
//! This is a M3-style minimum implementation. The Go side computed
//! line-range freshness too; line-range matching is captured as a
//! follow-up if/when the operator needs it.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

const VerifyState = enum { fresh, drifted, stale };

const VerifyRow = struct {
    id: i64,
    anchor_path: []const u8,
    state: VerifyState,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "verify" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var filter: engine.planning.annotation.ListFilter = .{
        .anchor_path = args.anchor_path,
        .scope = args.scope,
        .status = .active,
    };
    _ = &filter;

    const items = engine.planning.annotation.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "annotate verify: list failed: {s}", .{@errorName(e)});
    defer engine.planning.annotation.deinitMany(items, ctx.allocator);

    var rows: std.ArrayList(VerifyRow) = .empty;
    defer rows.deinit(ctx.allocator);

    for (items) |a| {
        const state = classify(ctx, a);
        try rows.append(ctx.allocator, .{
            .id = a.id,
            .anchor_path = a.anchor.path,
            .state = state,
        });
    }

    if (args.json) {
        try ctx.stdout.print("{{\"ok\":true,\"rows\":[", .{});
        for (rows.items, 0..) |r, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print(
                "{{\"id\":{d},\"anchor_path\":",
                .{r.id},
            );
            try std.json.Stringify.encodeJsonString(r.anchor_path, .{}, ctx.stdout);
            try ctx.stdout.print(",\"state\":\"{s}\"}}", .{@tagName(r.state)});
        }
        try ctx.stdout.print("]}}\n", .{});
    } else {
        for (rows.items) |r| {
            try ctx.stdout.print("annotation:{d}  {s}  [{s}]\n", .{ r.id, r.anchor_path, @tagName(r.state) });
        }
        if (rows.items.len == 0) try ctx.stdout.print("(no active annotations)\n", .{});
    }
}

fn classify(ctx: *const runtime.Ctx, a: engine.planning.annotation.Annotation) VerifyState {
    // Stale: file doesn't exist (or isn't readable) at anchor_path.
    const body = std.Io.Dir.cwd().readFileAlloc(ctx.io, a.anchor.path, ctx.allocator, .unlimited) catch return .stale;
    defer ctx.allocator.free(body);

    // If no text_hash was recorded, treat as fresh (no drift signal).
    if (a.anchor.text_hash.len == 0) return .fresh;

    // Compute SHA-256 of the file body and compare against the
    // stored hash prefix. The stored hash is operator-opaque — we
    // just byte-compare. Drift signal is "stored hash != computed".
    var hasher = std.crypto.hash.sha2.Sha256.init(.{});
    hasher.update(body);
    var digest_buf: [32]u8 = undefined;
    hasher.final(&digest_buf);
    var hex_buf: [64]u8 = undefined;
    _ = std.fmt.bufPrint(&hex_buf, "{x}", .{&digest_buf}) catch return .stale;

    const stored = a.anchor.text_hash;
    const computed = hex_buf[0..@min(stored.len, hex_buf.len)];
    return if (std.mem.eql(u8, stored, computed)) .fresh else .drifted;
}
