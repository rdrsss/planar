//! handlers/ingest — `planar-agent ingest --vendor claude --event @<file|->`
//!
//! M2 SKELETON. Wires the flag surface and the @file-or-stdin event
//! reader, validates the vendor tag against the locked allowlist, parses
//! the JSON envelope, and returns an empty-counts JSON shape so callers
//! and tests can rely on the contract today. Full adapter routing (the
//! Claude-first hook → store translation) lands in M4.
//!
//! TODO(plan:85, task:planar-agent-ingest): wire the Claude adapter from
//! `src/engine/external/agentingest/` once M4 lands.

const std = @import("std");
const cli = @import("cli");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "ingest",
    .desc = "Translate a vendor hook event into store primitives (M2 skeleton; full adapter in M4).",
    .flags = &.{
        .{ .long = "--vendor", .kind = .string, .required = true, .desc = "Vendor tag (claude|codex|copilot; M2 accepts claude; others land M4/M6)" },
        .{ .long = "--event", .kind = .string, .required = true, .desc = "Event JSON: @<file> reads from path; @- reads from stdin" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"ingest"}, args_ptr);
    const ctx = runtime.current();
    _ = runtime.ensureDbReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Vendor allowlist. M2 accepts `claude` (the spec's "M2 accepts
    // claude for now"). codex / copilot are rejected here so the
    // operator gets a clean error instead of a silent no-op.
    if (!std.mem.eql(u8, args.vendor, "claude")) {
        exit.die(ctx, error.InvalidInput, "vendor '{s}' not yet wired; M2 accepts 'claude' only (codex/copilot in M4/M6)", .{args.vendor});
    }

    // Read the event payload. `@<path>` reads the file; `@-` reads stdin.
    if (args.event.len < 1 or args.event[0] != '@') {
        exit.die(ctx, error.InvalidInput, "--event must be '@<file>' or '@-' (got '{s}')", .{args.event});
    }
    const src = args.event[1..];
    const payload = readEventSource(ctx, src) catch |e|
        exit.die(ctx, e, "read event ({s}): {s}", .{ src, @errorName(e) });
    defer ctx.allocator.free(payload);

    // Minimal validation that the payload is parseable JSON. We do not
    // walk the schema in M2; the adapter that lands in M4 owns that.
    var parsed = std.json.parseFromSlice(std.json.Value, ctx.allocator, payload, .{}) catch |e|
        exit.die(ctx, e, "event is not valid JSON: {s}", .{@errorName(e)});
    defer parsed.deinit();

    // M2 emits the documented JSON shape with zero counts; the adapter
    // increments them in M4.
    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"sessions_created\":0,\"claims_created\":0,\"actions_created\":0,\"events_processed\":0}}\n",
            .{},
        );
    } else {
        try ctx.stdout.print("ingest: M2 skeleton — adapter routing lands in M4 (vendor='{s}', payload bytes={d})\n", .{ args.vendor, payload.len });
    }
}

/// Read the @<src> payload. `src == "-"` reads stdin to EOF (cap 1 MiB);
/// otherwise treats `src` as a file path resolved relative to cwd (or
/// absolute when it starts with `/`).
fn readEventSource(ctx: *const runtime.Ctx, src: []const u8) ![]u8 {
    const cap: usize = 1 << 20;
    if (std.mem.eql(u8, src, "-")) {
        var buf: [4096]u8 = undefined;
        var stdin_reader = std.Io.File.Reader.init(.stdin(), ctx.io, &buf);
        return try stdin_reader.interface.allocRemaining(ctx.allocator, std.Io.Limit.limited(cap));
    }
    return try std.Io.Dir.cwd().readFileAlloc(
        ctx.io,
        src,
        ctx.allocator,
        std.Io.Limit.limited(cap),
    );
}
