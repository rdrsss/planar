//! handlers/dispatch/preview — `planar-agent dispatch preview` verb.
//!
//! Freezes the current packet, profile, policy, and capability digests plus
//! the cohort, candidate, host, and claim target behind a single-use token.
//!
//! Every identifier here is opaque operator data. It is bound as a SQL
//! parameter and never interpolated into a command line, so shell punctuation
//! in a model id is content, not a threat — the only rejection is control
//! characters, which would corrupt the record itself.
//!
//! JSON output: { ok, preview_token, preview_id, expires_at, evidence_state }.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../../main.zig");
const exit = @import("../../exit.zig");

const dispatch = engine.routing.dispatch;
const store = engine.routing.store;

pub const verb: cli.Cmd = .{
    .name = "preview",
    .desc = "Bind current routing state to a single-use, expiry-bound confirmation token.",
    .flags = &.{
        .{ .long = "--task", .kind = .string, .desc = "Task id this dispatch targets" },
        .{ .long = "--work-item", .kind = .string, .required = true, .desc = "Logical work item id" },
        .{ .long = "--project", .kind = .string, .required = true, .desc = "Project id" },
        .{ .long = "--validation-policy", .kind = .string, .required = true, .desc = "Validation policy version" },
        .{ .long = "--routing-policy", .kind = .string, .required = true, .desc = "Routing policy version" },
        .{ .long = "--profile-rule", .kind = .string, .required = true, .desc = "Profile rule version" },
        .{ .long = "--vendor", .kind = .string, .required = true, .desc = "Vendor (opaque)" },
        .{ .long = "--role", .kind = .string, .required = true, .desc = "Role (opaque)" },
        .{ .long = "--tier", .kind = .string, .required = true, .desc = "small|medium|large" },
        .{ .long = "--work-type", .kind = .string, .required = true, .desc = "schema|engine|architectural|cli|feature|mechanical" },
        .{ .long = "--complexity", .kind = .string, .required = true, .desc = "bounded|standard|high-risk" },
        .{ .long = "--packet-digest", .kind = .string, .required = true, .desc = "Digest of the authoritative packet" },
        .{ .long = "--profile-digest", .kind = .string, .required = true, .desc = "Digest of the compiled profile" },
        .{ .long = "--policy-digest", .kind = .string, .required = true, .desc = "Digest of the policy snapshot" },
        .{ .long = "--capability-digest", .kind = .string, .required = true, .desc = "Digest of the host capability snapshot" },
        .{ .long = "--candidate", .kind = .string, .required = true, .desc = "Requested candidate row id" },
        .{ .long = "--host", .kind = .string, .required = true, .desc = "Host id whose capability snapshot was consulted" },
        .{ .long = "--class", .kind = .string, .required = true, .desc = "fallback|default|override|declared_experiment" },
        .{ .long = "--experiment", .kind = .string, .desc = "Experiment id (required for declared_experiment)" },
        .{ .long = "--claim", .kind = .string, .desc = "Claim token this dispatch is bound to" },
        .{ .long = "--claim-status", .kind = .string, .desc = "Claim status observed at preview time" },
        .{ .long = "--evidence-state", .kind = .string, .required = true, .desc = "evidential|observational" },
        .{ .long = "--expires-at", .kind = .string, .required = true, .desc = "RFC3339 instant after which the token is dead" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

/// Comma-joined list of a routing enum's accepted wire spellings, built at
/// comptime from the enum itself so the diagnostic cannot drift from what is
/// actually accepted.
fn acceptedValues(comptime T: type) []const u8 {
    return comptime blk: {
        var out: []const u8 = "";
        for (@typeInfo(T).@"enum".fields, 0..) |f, i| {
            const text = if (@hasDecl(T, "toText"))
                @as(T, @enumFromInt(f.value)).toText()
            else
                f.name;
            out = out ++ (if (i == 0) "" else ", ") ++ text;
        }
        break :blk out;
    };
}

fn parseEnum(comptime T: type, ctx: anytype, flag: []const u8, raw: []const u8) T {
    // Prefer the type's own wire mapping when it has one; `@tagName` is not a
    // valid wire spelling for every routing enum (store.Complexity, task 6092).
    const parsed = if (@hasDecl(T, "fromText"))
        T.fromText(raw)
    else
        std.meta.stringToEnum(T, raw);
    return parsed orelse exit.die(
        ctx,
        error.InvalidInput,
        "invalid {s} '{s}': expected one of {s}",
        .{ flag, raw, acceptedValues(T) },
    );
}

/// Optional string flags arrive as `?[]const u8`; an explicitly empty value is
/// treated the same as an absent one so `--claim ""` cannot bind a claim.
fn optional(v: ?[]const u8) ?[]const u8 {
    const s = v orelse return null;
    return if (s.len == 0) null else s;
}

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "dispatch", "preview" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbConsumer() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const project_id = std.fmt.parseInt(i64, args.project, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid --project '{s}': expected integer", .{args.project});
    const candidate_id = std.fmt.parseInt(i64, args.candidate, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid --candidate '{s}': expected integer", .{args.candidate});

    const task_id: ?i64 = if (optional(args.task)) |t|
        std.fmt.parseInt(i64, t, 10) catch
            exit.die(ctx, error.InvalidInput, "invalid --task '{s}': expected integer", .{t})
    else
        null;
    const experiment_id: ?i64 = if (optional(args.experiment)) |e|
        std.fmt.parseInt(i64, e, 10) catch
            exit.die(ctx, error.InvalidInput, "invalid --experiment '{s}': expected integer", .{e})
    else
        null;

    const binding: dispatch.Binding = .{
        .logical_work_item_id = args.work_item,
        .project_id = project_id,
        .validation_policy_version = args.validation_policy,
        .routing_policy_version = args.routing_policy,
        .profile_rule_version = args.profile_rule,
        .vendor = args.vendor,
        .role = args.role,
        .tier = parseEnum(store.Tier, ctx, "--tier", args.tier),
        .work_type = parseEnum(store.WorkType, ctx, "--work-type", args.work_type),
        .complexity = parseEnum(store.Complexity, ctx, "--complexity", args.complexity),
        .packet_digest = args.packet_digest,
        .profile_digest = args.profile_digest,
        .policy_digest = args.policy_digest,
        .capability_digest = args.capability_digest,
        .requested_candidate_id = candidate_id,
        .host_id = args.host,
        .assignment_class = parseEnum(store.AssignmentClass, ctx, "--class", args.class),
        .experiment_id = experiment_id,
        .claim_token = optional(args.claim),
        .claim_status = optional(args.claim_status),
        .evidence_state = parseEnum(dispatch.EvidenceState, ctx, "--evidence-state", args.evidence_state),
    };

    const result = dispatch.preview(d, ctx.allocator, .{
        .task_id = task_id,
        .binding = binding,
        .expires_at = args.expires_at,
    }) catch |e| switch (e) {
        error.InvalidValue => exit.die(
            ctx,
            error.InvalidInput,
            "value rejected: identifiers must not contain control characters",
            .{},
        ),
        else => exit.die(ctx, e, "preview: {s}", .{@errorName(e)}),
    };
    defer result.deinit(ctx.allocator);

    if (args.json) {
        try std.json.Stringify.value(.{
            .ok = true,
            .preview_id = result.id,
            .preview_token = result.token,
            .expires_at = args.expires_at,
            .evidence_state = args.evidence_state,
            .contract = dispatch.dispatch_contract_version,
        }, .{}, ctx.stdout);
        try ctx.stdout.writeAll("\n");
    } else {
        try ctx.stdout.print("preview: {s}\n", .{result.token});
        try ctx.stdout.print("  expires:  {s}\n", .{args.expires_at});
        try ctx.stdout.print("  evidence: {s}\n", .{args.evidence_state});
    }
}
