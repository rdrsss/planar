//! handlers/ingest — `planar-agent ingest --vendor <v> --event @<file|->`
//!
//! Reads one vendor hook event from a file or stdin, dispatches it
//! through the matching vendor adapter (M4: `claude` only; M6 adds a
//! second vendor), and emits `{ok, sessions_created, claims_created,
//! actions_created, events_processed}` per the tech-spec.
//!
//! Error model — the two failure modes are deliberately distinct:
//!
//!   - **Malformed payload** (JSON parse failed, missing envelope
//!     fields, bad outcome value): exit code 2 (InvalidInput),
//!     "malformed event payload" message. No DB writes.
//!   - **Unknown event_type**: exit code 2 (InvalidInput), distinct
//!     message naming the offending value. No DB writes.
//!   - **Dispatch failure** (DB write failed mid-event): exit code 1,
//!     surrounding `BEGIN IMMEDIATE` is rolled back so no partial
//!     state lands.
//!
//! Vendor allowlist: M4 wires `claude`; M6 wires `copilot`. `codex`
//! parses the flag but exits InvalidInput — no adapter is wired (the
//! second-vendor slot landed Copilot per the plan-85 M6 decision
//! artifact).

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");

const agentingest = engine.external.agentingest;

pub const verb: cli.Cmd = .{
    .name = "ingest",
    .desc = "Translate a vendor hook event into store primitives (claude + copilot adapters wired; codex reserved).",
    .flags = &.{
        .{ .long = "--vendor", .kind = .string, .required = true, .desc = "Vendor tag (claude|copilot wired; codex reserved)" },
        .{ .long = "--event", .kind = .string, .required = true, .desc = "Event JSON: @<file> reads from path; @- reads from stdin" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"ingest"}, args_ptr);
    const ctx = runtime.current();

    // Vendor allowlist + adapter resolution. M4 wires `claude`.
    const vendor_tag = agentingest.interface.Vendor.fromText(args.vendor) orelse
        exit.die(ctx, error.InvalidInput, "unknown vendor '{s}'; supported: claude|codex|copilot", .{args.vendor});

    const parse_fn: agentingest.interface.ParseFn = switch (vendor_tag) {
        .claude => agentingest.claude.parse,
        .copilot => agentingest.copilot.parse,
        .codex => exit.die(
            ctx,
            error.InvalidInput,
            "vendor '{s}' adapter not wired (claude + copilot are; codex is reserved)",
            .{args.vendor},
        ),
    };

    // Open the DB (writable; ensureDbReadOnly is misnamed — it opens R/W
    // but does not apply migrations, which matches the planar-agent
    // schema-consumer role).
    const d = runtime.ensureDbReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Read the @<src> payload.
    if (args.event.len < 1 or args.event[0] != '@') {
        exit.die(ctx, error.InvalidInput, "--event must be '@<file>' or '@-' (got '{s}')", .{args.event});
    }
    const src = args.event[1..];
    const payload = readEventSource(ctx, src) catch |e|
        exit.die(ctx, e, "read event ({s}): {s}", .{ src, @errorName(e) });
    defer ctx.allocator.free(payload);

    // Parse via the vendor adapter. Distinguish malformed vs unknown
    // event_type so the operator's hook script can react differently.
    const event = parse_fn(ctx.allocator, payload) catch |e| switch (e) {
        agentingest.interface.ParseError.Malformed => exit.die(
            ctx,
            error.InvalidInput,
            "malformed event payload (vendor={s}): JSON parse failed or required envelope field missing",
            .{args.vendor},
        ),
        agentingest.interface.ParseError.UnknownEventType => exit.die(
            ctx,
            error.InvalidInput,
            "unknown event_type in payload (vendor={s}); see the adapter's known event_type set",
            .{args.vendor},
        ),
        else => exit.die(ctx, e, "parse: {s}", .{@errorName(e)}),
    };
    defer event.deinit(ctx.allocator);

    // Apply the event under BEGIN IMMEDIATE so any underlying-store
    // failure leaves the DB unchanged. The dispatch layer itself does
    // not start a transaction — that's the handler's job, matching
    // every other planar-agent verb.
    var counts: agentingest.dispatch.Counts = .{};
    d.exec("BEGIN IMMEDIATE") catch |e| exit.die(ctx, e, "BEGIN IMMEDIATE: {s}", .{@errorName(e)});
    agentingest.dispatch.apply(d, ctx.allocator, args.vendor, event, &counts) catch |e| {
        d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "ingest dispatch: {s}", .{@errorName(e)});
    };
    d.exec("COMMIT") catch |e| {
        d.exec("ROLLBACK") catch {};
        exit.die(ctx, e, "COMMIT: {s}", .{@errorName(e)});
    };

    try emit(ctx, args.json, args.vendor, counts);
}

fn emit(
    ctx: *const runtime.Ctx,
    use_json: bool,
    vendor: []const u8,
    counts: anytype,
) !void {
    if (use_json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"sessions_created\":{d},\"claims_created\":{d},\"actions_created\":{d},\"events_processed\":{d}}}\n",
            .{
                counts.sessions_created,
                counts.claims_created,
                counts.actions_created,
                counts.events_processed,
            },
        );
    } else {
        try ctx.stdout.print(
            "ingest: vendor={s} sessions_created={d} actions_created={d} events_processed={d}\n",
            .{ vendor, counts.sessions_created, counts.actions_created, counts.events_processed },
        );
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

comptime {
    _ = db;
}
