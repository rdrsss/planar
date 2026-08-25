//! handlers/dispatch/confirm — `planar-agent dispatch confirm` verb.
//!
//! Revalidates every value the preview froze against what the caller observes
//! now, then writes the immutable dispatch snapshot and consumes the token in
//! one transaction. Any drift, expiry, or reuse returns `stale_preview` and
//! writes nothing.
//!
//! The caller passes what it currently observes rather than having this verb
//! re-derive it. That is deliberate: re-deriving here would compare current
//! state against itself and always agree, which is exactly the check the
//! two-step contract exists to make.
//!
//! JSON output: { ok, dispatch_id } or { ok: false, error: "stale_preview",
//! reason }.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");

const dispatch = engine.routing.dispatch;
const store = engine.routing.store;

pub const verb: cli.Cmd = .{
    .name = "confirm",
    .desc = "Revalidate a preview token against current state and atomically write the dispatch snapshot.",
    .flags = &.{
        .{ .long = "--token", .kind = .string, .required = true, .desc = "Preview token to spend" },
        .{ .long = "--dispatch-key", .kind = .string, .required = true, .desc = "Unique key for the resulting dispatch" },
        .{ .long = "--now", .kind = .string, .required = true, .desc = "RFC3339 instant to evaluate expiry against" },
        .{ .long = "--packet-digest", .kind = .string, .required = true, .desc = "Currently observed packet digest" },
        .{ .long = "--profile-digest", .kind = .string, .required = true, .desc = "Currently observed profile digest" },
        .{ .long = "--policy-digest", .kind = .string, .required = true, .desc = "Currently observed policy digest" },
        .{ .long = "--capability-digest", .kind = .string, .required = true, .desc = "Currently observed capability digest" },
        .{ .long = "--candidate", .kind = .string, .required = true, .desc = "Currently resolved candidate row id" },
        .{ .long = "--vendor", .kind = .string, .required = true, .desc = "Currently resolved vendor" },
        .{ .long = "--role", .kind = .string, .required = true, .desc = "Currently resolved role" },
        .{ .long = "--tier", .kind = .string, .required = true, .desc = "Currently resolved tier" },
        .{ .long = "--work-type", .kind = .string, .required = true, .desc = "Currently resolved work type" },
        .{ .long = "--complexity", .kind = .string, .required = true, .desc = "Currently resolved complexity" },
        .{ .long = "--validation-policy", .kind = .string, .required = true, .desc = "Currently active validation policy version" },
        .{ .long = "--routing-policy", .kind = .string, .required = true, .desc = "Currently active routing policy version" },
        .{ .long = "--claim", .kind = .string, .desc = "Currently held claim token" },
        .{ .long = "--claim-status", .kind = .string, .desc = "Currently observed claim status" },
        .{ .long = "--reviewer", .kind = .string, .desc = "Reviewer disposition to record (default required)" },
        .{ .long = "--decision", .kind = .string, .desc = "confirmed|overridden (default confirmed)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn parseEnum(comptime T: type, ctx: anytype, flag: []const u8, raw: []const u8) T {
    return std.meta.stringToEnum(T, raw) orelse
        exit.die(ctx, error.InvalidInput, "invalid {s} '{s}'", .{ flag, raw });
}

/// Optional string flags arrive as `?[]const u8`; an explicitly empty value is
/// treated the same as an absent one so `--claim ""` cannot bind a claim.
fn optional(v: ?[]const u8) ?[]const u8 {
    const s = v orelse return null;
    return if (s.len == 0) null else s;
}

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "dispatch", "confirm" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const candidate_id = std.fmt.parseInt(i64, args.candidate, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid --candidate '{s}': expected integer", .{args.candidate});

    const current: dispatch.CurrentState = .{
        .now = args.now,
        .packet_digest = args.packet_digest,
        .profile_digest = args.profile_digest,
        .policy_digest = args.policy_digest,
        .capability_digest = args.capability_digest,
        .requested_candidate_id = candidate_id,
        .claim_token = optional(args.claim),
        .claim_status = optional(args.claim_status),
        .vendor = args.vendor,
        .role = args.role,
        .tier = parseEnum(store.Tier, ctx, "--tier", args.tier),
        .work_type = parseEnum(store.WorkType, ctx, "--work-type", args.work_type),
        .complexity = parseEnum(store.Complexity, ctx, "--complexity", args.complexity),
        .validation_policy_version = args.validation_policy,
        .routing_policy_version = args.routing_policy,
    };

    const reviewer: store.ReviewerDisposition = if (optional(args.reviewer)) |r|
        parseEnum(store.ReviewerDisposition, ctx, "--reviewer", r)
    else
        .required;
    const decision: store.OperatorDecision = if (optional(args.decision)) |dec|
        parseEnum(store.OperatorDecision, ctx, "--decision", dec)
    else
        .confirmed;

    var reason: dispatch.StaleReason = undefined;
    const done = dispatch.confirm(d, ctx.allocator, .{
        .token = args.token,
        .dispatch_key = args.dispatch_key,
        .current = current,
        .reviewer_disposition = reviewer,
        .operator_decision = decision,
    }, &reason) catch |e| switch (e) {
        // A stale preview is an expected operator outcome, not a crash: report
        // which binding moved so they can decide whether to re-preview.
        error.StalePreview => {
            if (args.json) {
                try std.json.Stringify.value(.{
                    .ok = false,
                    .@"error" = "stale_preview",
                    .reason = reason.text(),
                }, .{}, ctx.stdout);
                try ctx.stdout.writeAll("\n");
            } else {
                try ctx.stdout.print("stale_preview: {s}\n", .{reason.text()});
            }
            exit.die(ctx, error.Conflict, "stale_preview: {s}", .{reason.text()});
        },
        error.UnknownPreview => exit.die(ctx, error.NotFound, "unknown preview token", .{}),
        error.InvalidValue => exit.die(
            ctx,
            error.InvalidInput,
            "value rejected: identifiers must not contain control characters",
            .{},
        ),
        else => exit.die(ctx, e, "confirm: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        try std.json.Stringify.value(.{
            .ok = true,
            .dispatch_id = done.dispatch_id,
            .contract = dispatch.dispatch_contract_version,
        }, .{}, ctx.stdout);
        try ctx.stdout.writeAll("\n");
    } else {
        try ctx.stdout.print("dispatch: {d}\n", .{done.dispatch_id});
    }
}
