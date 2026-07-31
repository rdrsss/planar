//! handlers/models.zig — `planar models` (plan 540/543 provider + model discovery)
//!
//! Five subverbs, none of which touches the database:
//!
//!   list       Probe installed provider CLIs (claude, codex) and print their
//!              curated model catalogs + the default role→tier→model routing.
//!   refresh    Same probe, plus write the result to
//!              ${PLANAR_HOME:-~/.planar}/models/catalog.json as a cache.
//!   routing    Resolve the effective config and print the role→vendor/
//!              tier/model routing table with provenance (read-only).
//!   apply      Scaffold the [models]/[roles] config block into the config
//!              file as an editable starting point.
//!   candidates Resolve the effective config and print each tier's candidate
//!              list plus the work-type routing map with provenance
//!              (plan 899, read-only).
//!   evals      Aggregate completed dispatch outcomes into a per-(work-type,
//!              candidate) scorecard, and a preview-only routing-map
//!              recommendation. Read-only — writes nothing (plan 898/904,
//!              tech-spec 520 D8).
//!
//! Discovery genuinely invokes `<bin> --version` per provider (instant +
//! auth-free); the model list itself comes from the curated catalog in
//! `engine.models` because the provider CLIs do not enumerate models.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const db = @import("db");
const main = @import("../main.zig");
const runtime = @import("runtime");
const output = @import("../output.zig");
const exit = @import("../exit.zig");
const config_path = @import("config/path.zig");

pub const verb: cli.Cmd = .{
    .name = "models",
    .desc = "Discover installed provider CLIs and their model catalogs.",
    .long_desc = "Probe the supported provider CLIs (claude, codex) for\n  installed-state + version, and report their curated model\n  catalogs and the default role→tier→model routing.\n\n  The provider CLIs do not expose a machine-readable model list,\n  so the per-vendor model catalog is curated in-repo; discovery\n  confirms which CLIs are callable on this machine.\n\n  Subcommands:\n    list       Probe + print (read-only).\n    refresh    Probe + print, and write the cache to\n               ${PLANAR_HOME:-~/.planar}/models/catalog.json.",
    .cmds = &.{
        .{
            .name = "evals",
            .desc = "Aggregate completed dispatch outcomes into a per-(work-type, candidate) scorecard and preview-only recommendation.",
            .long_desc = "Evidence-backed candidate ranking over declared-experiment\n  terminal samples in the exact cohort (plan 950 task 5530). Supply the\n  cohort flags to rank: results report sample and success counts, the raw\n  rate, the 95% Wilson lower bound, gate-failure rate, and expected excess\n  iterations. Candidates under --min-samples are labelled insufficient_data\n  and are never ranked or recommended; candidates below --quality-floor are\n  excluded before any iteration or gate-failure ordering, so a fast-but-wrong\n  candidate cannot outrank a slower correct one.\n\n  Without cohort flags this falls back to the LEGACY note-convention\n  scorecard below, which remains inspectable but is not evidence-backed:\n  it predates the routing evidence plane and carries no cohort or\n  independent-quality guarantee.\n\n  Legacy: read-only aggregation (plan 898/904, tech-spec 520 D8) over the\n  `dispatch_shape` / `model_choice` note convention in `session_entries`\n  (agents/orchestrator.md step 8a), joined with `agent_work_claims`\n  (terminal disposition) and `agent_actions` (test-coder expansion\n  outcome). Emits a per-(work-type, candidate) scorecard and a\n  recommended routing-map change. A pair with no completed-dispatch\n  history reports insufficient-data rather than a fabricated score.\n  Writes nothing: no routing-map mutation, no database write. Applying\n  a recommendation is a separate, explicit operator-gated action.",
            .flags = &.{
                .{ .long = "--vendor", .kind = .string, .desc = "Cohort vendor; enables evidence-backed ranking" },
                .{ .long = "--role", .kind = .string, .desc = "Cohort role" },
                .{ .long = "--tier", .kind = .string, .desc = "Cohort tier (small|medium|large)" },
                .{ .long = "--work-type", .kind = .string, .desc = "Cohort work type" },
                .{ .long = "--complexity", .kind = .string, .desc = "Cohort complexity (bounded|standard|high-risk)" },
                .{ .long = "--project", .kind = .string, .desc = "Cohort project id" },
                .{ .long = "--validation-policy", .kind = .string, .desc = "Cohort validation policy version" },
                .{ .long = "--routing-policy", .kind = .string, .desc = "Cohort routing policy version" },
                .{ .long = "--min-samples", .kind = .string, .desc = "Minimum samples before a candidate is ranked (default 5)" },
                .{ .long = "--quality-floor", .kind = .string, .desc = "Wilson lower-bound floor (default 0.5)" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleEvals),
        },
        .{
            .name = "registry",
            .desc = "Manage opaque operator candidates and host observations.",
            .cmds = &.{
                .{
                    .name = "list",
                    .desc = "List registrations, bindings, and latest observations.",
                    .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
                    .run = cli.handler(handleRegistryList),
                },
                .{
                    .name = "add",
                    .desc = "Register one exact opaque candidate identifier.",
                    .flags = &.{
                        .{ .long = "--vendor", .kind = .string, .required = true },
                        .{ .long = "--id", .kind = .string, .required = true },
                        .{ .long = "--order", .kind = .int, .required = true },
                        .{ .long = "--disabled", .kind = .bool, .default = .{ .bool = false } },
                    },
                    .run = cli.handler(handleRegistryAdd),
                },
                .{
                    .name = "update",
                    .desc = "Update enabled state and deterministic fallback order.",
                    .flags = &.{
                        .{ .long = "--candidate", .kind = .int, .required = true },
                        .{ .long = "--order", .kind = .int, .required = true },
                        .{ .long = "--disabled", .kind = .bool, .default = .{ .bool = false } },
                    },
                    .run = cli.handler(handleRegistryUpdate),
                },
                .{
                    .name = "remove",
                    .desc = "Remove a candidate when no immutable evidence references it.",
                    .flags = &.{.{ .long = "--candidate", .kind = .int, .required = true }},
                    .run = cli.handler(handleRegistryRemove),
                },
                .{
                    .name = "bind",
                    .desc = "Allow one role and tier for a candidate.",
                    .flags = &.{
                        .{ .long = "--candidate", .kind = .int, .required = true },
                        .{ .long = "--role", .kind = .string, .required = true },
                        .{ .long = "--tier", .kind = .string, .required = true },
                    },
                    .run = cli.handler(handleRegistryBind),
                },
                .{
                    .name = "unbind",
                    .desc = "Remove one explicit role and tier binding.",
                    .flags = &.{
                        .{ .long = "--candidate", .kind = .int, .required = true },
                        .{ .long = "--role", .kind = .string, .required = true },
                        .{ .long = "--tier", .kind = .string, .required = true },
                    },
                    .run = cli.handler(handleRegistryUnbind),
                },
                .{
                    .name = "observe",
                    .desc = "Append an exact, versioned host capability observation.",
                    .flags = &.{
                        .{ .long = "--candidate", .kind = .int, .required = true },
                        .{ .long = "--host", .kind = .string, .required = true },
                        .{ .long = "--version", .kind = .int, .required = true },
                        .{ .long = "--availability", .kind = .string, .required = true },
                        .{ .long = "--spawn-verification", .kind = .string, .required = true },
                        .{ .long = "--evidence-ref", .kind = .string, .required = true },
                        .{ .long = "--captured-at", .kind = .string, .required = true },
                        .{ .long = "--expires-at", .kind = .string, .required = true },
                    },
                    .run = cli.handler(handleRegistryObserve),
                },
                .{
                    .name = "eligibility",
                    .desc = "Report every independent eligibility gate and named exclusion reason.",
                    .flags = &.{
                        .{ .long = "--candidate", .kind = .int, .required = true },
                        .{ .long = "--host", .kind = .string, .required = true },
                        .{ .long = "--role", .kind = .string, .required = true },
                        .{ .long = "--tier", .kind = .string, .required = true },
                        .{ .long = "--now", .kind = .string, .required = true },
                        .{ .long = "--override-supported", .kind = .bool, .default = .{ .bool = false } },
                        .{ .long = "--policy-permits", .kind = .bool, .default = .{ .bool = false } },
                    },
                    .run = cli.handler(handleRegistryEligibility),
                },
                .{
                    .name = "verify-identity",
                    .desc = "Compare requested and actual spawn identity without aliasing.",
                    .flags = &.{
                        .{ .long = "--candidate", .kind = .int, .required = true },
                        .{ .long = "--actual-vendor", .kind = .string, .required = true },
                        .{ .long = "--actual-id", .kind = .string, .required = true },
                    },
                    .run = cli.handler(handleRegistryVerifyIdentity),
                },
                .{
                    .name = "export",
                    .desc = "Export the versioned registry compatibility document.",
                    .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
                    .run = cli.handler(handleRegistryExport),
                },
            },
        },
    },
};

fn parseTier(value: []const u8) ?engine.routing.store.Tier {
    inline for (std.meta.tags(engine.routing.store.Tier)) |tag|
        if (std.mem.eql(u8, value, @tagName(tag))) return tag;
    return null;
}

fn parseAvailability(value: []const u8) ?engine.routing.store.Availability {
    inline for (std.meta.tags(engine.routing.store.Availability)) |tag|
        if (std.mem.eql(u8, value, @tagName(tag))) return tag;
    return null;
}

fn parseVerification(value: []const u8) ?engine.routing.store.SpawnVerification {
    inline for (std.meta.tags(engine.routing.store.SpawnVerification)) |tag|
        if (std.mem.eql(u8, value, @tagName(tag))) return tag;
    return null;
}

/// One tier's effective candidate list with provenance (plan 899 D3/D5).
/// `candidates[0]` is always the tier default.
const TierCandidates = struct {
    vendor: []const u8,
    tier: []const u8,
    candidates: []const []const u8,
    source: []const u8,
};

/// One resolved `routing.<vendor>.<tier>.<work_type>` entry with provenance
/// (plan 899 D4/D9/D10/D11). Only work types actually present in the
/// effective map (file override or embedded default) are reported — a
/// work type with no routing entry falls back to the tier default at
/// resolve time and is simply absent here.
const RoutingEntry = struct {
    vendor: []const u8,
    tier: []const u8,
    work_type: []const u8,
    model: []const u8,
    source: []const u8,
};

fn handleRegistryList(args_ptr: *const anyopaque) anyerror!void {
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    // `export` intentionally shares the same additive versioned wire shape.
    const args = cli.castArgs(main.root, &.{ "models", "registry", "list" }, args_ptr);
    const candidates = engine.routing.store.listCandidates(d, ctx.allocator) catch |e|
        exit.die(ctx, e, "listing model registry: {s}", .{@errorName(e)});
    defer {
        for (candidates) |candidate| candidate.deinit(ctx.allocator);
        ctx.allocator.free(candidates);
    }
    if (args.json) {
        try ctx.stdout.print("{{\"registry_version\":1,\"candidates\":", .{});
        try std.json.Stringify.value(candidates, .{}, ctx.stdout);
        try ctx.stdout.print(",\"migration_warning\":\"legacy catalog compatibility is one-window and non-authoritative\"}}\n", .{});
        return;
    }
    for (candidates) |candidate| {
        try ctx.stdout.print(
            "{d} {s} {s} enabled={} order={d} bindings={d} observation={s}\n",
            .{
                candidate.registration.id,
                candidate.registration.vendor,
                candidate.registration.candidate_id,
                candidate.registration.enabled,
                candidate.registration.fallback_order,
                candidate.bindings.len,
                if (candidate.latest_observation == null) "none" else "present",
            },
        );
    }
}

fn handleRegistryExport(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "export" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    const candidates = engine.routing.store.listCandidates(d, ctx.allocator) catch |e|
        exit.die(ctx, e, "exporting model registry: {s}", .{@errorName(e)});
    defer {
        for (candidates) |candidate| candidate.deinit(ctx.allocator);
        ctx.allocator.free(candidates);
    }
    if (!args.json) try ctx.stderr.print("warning: legacy catalog compatibility is one-window and non-authoritative\n", .{});
    try ctx.stdout.print("{{\"registry_version\":1,\"candidates\":", .{});
    try std.json.Stringify.value(candidates, .{}, ctx.stdout);
    try ctx.stdout.print(",\"migration_warning\":\"legacy catalog compatibility is one-window and non-authoritative\"}}\n", .{});
}

fn handleRegistryAdd(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    const id = engine.routing.store.createCandidate(d, .{
        .vendor = args.vendor,
        .candidate_id = args.id,
        .enabled = !args.disabled,
        .fallback_order = args.order,
    }) catch |e| exit.die(ctx, e, "registering opaque candidate: {s}", .{@errorName(e)});
    try ctx.stdout.print("{d}\n", .{id});
}

fn handleRegistryUpdate(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "update" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    engine.routing.store.updateCandidate(d, args.candidate, !args.disabled, args.order) catch |e|
        exit.die(ctx, e, "updating candidate: {s}", .{@errorName(e)});
}

fn handleRegistryRemove(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "remove" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    engine.routing.store.deleteCandidate(d, args.candidate) catch |e|
        exit.die(ctx, e, "removing candidate: {s}", .{@errorName(e)});
}

fn handleRegistryBind(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "bind" }, args_ptr);
    const ctx = runtime.current();
    const tier = parseTier(args.tier) orelse exit.die(ctx, error.InvalidInput, "invalid tier: {s}", .{args.tier});
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    engine.routing.store.bindCandidate(d, args.candidate, args.role, tier) catch |e|
        exit.die(ctx, e, "binding candidate: {s}", .{@errorName(e)});
}

fn handleRegistryUnbind(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "unbind" }, args_ptr);
    const ctx = runtime.current();
    const tier = parseTier(args.tier) orelse exit.die(ctx, error.InvalidInput, "invalid tier: {s}", .{args.tier});
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    engine.routing.store.unbindCandidate(d, args.candidate, args.role, tier) catch |e|
        exit.die(ctx, e, "unbinding candidate: {s}", .{@errorName(e)});
}

fn handleRegistryObserve(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "observe" }, args_ptr);
    const ctx = runtime.current();
    const availability = parseAvailability(args.availability) orelse
        exit.die(ctx, error.InvalidInput, "invalid availability: {s}", .{args.availability});
    const verification = parseVerification(args.spawn_verification) orelse
        exit.die(ctx, error.InvalidInput, "invalid spawn verification: {s}", .{args.spawn_verification});
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    const id = engine.routing.store.observeCandidate(d, .{
        .candidate_id = args.candidate,
        .host_id = args.host,
        .observation_version = args.version,
        .availability = availability,
        .spawn_verification = verification,
        .evidence_ref = args.evidence_ref,
        .captured_at = args.captured_at,
        .expires_at = args.expires_at,
    }) catch |e| exit.die(ctx, e, "recording host observation: {s}", .{@errorName(e)});
    try ctx.stdout.print("{d}\n", .{id});
}

fn handleRegistryEligibility(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "eligibility" }, args_ptr);
    const ctx = runtime.current();
    const tier = parseTier(args.tier) orelse exit.die(ctx, error.InvalidInput, "invalid tier: {s}", .{args.tier});
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    const candidate = engine.routing.store.getCandidateForHost(d, ctx.allocator, args.candidate, args.host) catch |e|
        exit.die(ctx, e, "reading candidate: {s}", .{@errorName(e)});
    defer candidate.deinit(ctx.allocator);
    var bound = false;
    for (candidate.bindings) |binding| {
        if (std.mem.eql(u8, binding.role, args.role) and binding.tier == tier) {
            bound = true;
            break;
        }
    }
    const eligibility = engine.routing.store.evaluateEligibility(.{
        .enabled = candidate.registration.enabled,
        .binding_present = bound,
        .role_surface_override_supported = args.override_supported,
        .host_policy_permits = args.policy_permits,
        .observation = candidate.latest_observation,
        .now = args.now,
    });
    var reason_buffer: [6]engine.routing.store.EligibilityReason = undefined;
    const reasons = eligibility.reasons(&reason_buffer);
    try ctx.stdout.print("{{\"candidate\":{d},\"host\":", .{args.candidate});
    try std.json.Stringify.value(args.host, .{}, ctx.stdout);
    try ctx.stdout.print(",\"eligible\":{},\"gates\":", .{eligibility.eligible()});
    try std.json.Stringify.value(eligibility, .{}, ctx.stdout);
    try ctx.stdout.print(",\"reasons\":", .{});
    try std.json.Stringify.value(reasons, .{}, ctx.stdout);
    try ctx.stdout.print("}}\n", .{});
}

fn handleRegistryVerifyIdentity(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "registry", "verify-identity" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e| exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});
    const candidates = engine.routing.store.listCandidates(d, ctx.allocator) catch |e|
        exit.die(ctx, e, "reading candidate: {s}", .{@errorName(e)});
    defer {
        for (candidates) |candidate| candidate.deinit(ctx.allocator);
        ctx.allocator.free(candidates);
    }
    var requested: ?engine.routing.store.CandidateRegistration = null;
    for (candidates) |candidate| if (candidate.registration.id == args.candidate) {
        requested = candidate.registration;
        break;
    };
    const registration = requested orelse exit.die(ctx, error.NotFound, "candidate {d} not found", .{args.candidate});
    const result = engine.routing.store.verifyActualIdentity(
        registration.vendor,
        registration.candidate_id,
        args.actual_vendor,
        args.actual_id,
    );
    try ctx.stdout.print("{{\"candidate\":{d},\"identity\":\"{s}\"}}\n", .{ args.candidate, @tagName(result) });
}

fn optFlag(v: ?[]const u8) ?[]const u8 {
    const t = v orelse return null;
    return if (t.len == 0) null else t;
}

fn requireFlag(ctx: anytype, name: []const u8, v: ?[]const u8) []const u8 {
    return optFlag(v) orelse
        exit.die(ctx, error.InvalidInput, "{s} is required when ranking a cohort", .{name});
}

fn parseCohortEnum(comptime T: type, ctx: anytype, flag: []const u8, raw: []const u8) T {
    // `high-risk` on the wire, `high_risk` in the enum.
    const normalized = if (std.mem.eql(u8, raw, "high-risk")) "high_risk" else raw;
    return std.meta.stringToEnum(T, normalized) orelse
        exit.die(ctx, error.InvalidInput, "invalid {s} '{s}'", .{ flag, raw });
}

/// Rank one exact cohort from declared-experiment evidence.
fn rankCohort(ctx: anytype, d: *db.sqlite.Db, args: anytype, vendor: []const u8) !void {
    const ranking = engine.routing.ranking;
    const store = engine.routing.store;

    const project_raw = requireFlag(ctx, "--project", args.project);
    const project_id = std.fmt.parseInt(i64, project_raw, 10) catch
        exit.die(ctx, error.InvalidInput, "invalid --project '{s}': expected integer", .{project_raw});

    var gates: ranking.Gates = .{};
    if (optFlag(args.min_samples)) |raw| {
        gates.minimum_samples = std.fmt.parseInt(u64, raw, 10) catch
            exit.die(ctx, error.InvalidInput, "invalid --min-samples '{s}'", .{raw});
    }
    if (optFlag(args.quality_floor)) |raw| {
        gates.quality_floor = std.fmt.parseFloat(f64, raw) catch
            exit.die(ctx, error.InvalidInput, "invalid --quality-floor '{s}'", .{raw});
    }

    const cohort: ranking.Cohort = .{
        .project_id = project_id,
        .validation_policy_version = requireFlag(ctx, "--validation-policy", args.validation_policy),
        .routing_policy_version = requireFlag(ctx, "--routing-policy", args.routing_policy),
        .vendor = vendor,
        .role = requireFlag(ctx, "--role", args.role),
        .tier = parseCohortEnum(store.Tier, ctx, "--tier", requireFlag(ctx, "--tier", args.tier)),
        .work_type = parseCohortEnum(store.WorkType, ctx, "--work-type", requireFlag(ctx, "--work-type", args.work_type)),
        .complexity = parseCohortEnum(store.Complexity, ctx, "--complexity", requireFlag(ctx, "--complexity", args.complexity)),
    };

    var result = ranking.rank(d, ctx.allocator, cohort, gates) catch |e|
        exit.die(ctx, e, "ranking cohort: {s}", .{@errorName(e)});
    defer result.deinit(ctx.allocator);

    if (args.json) {
        try std.json.Stringify.value(.{
            .version = result.version,
            .evidence = "declared_experiment",
            .gates = .{
                .minimum_samples = gates.minimum_samples,
                .quality_floor = gates.quality_floor,
            },
            .rows = result.rows,
            .recommended = if (result.recommended) |i| result.rows[i].candidate else null,
            .no_recommendation_reason = result.no_recommendation_reason,
        }, .{}, ctx.stdout);
        try ctx.stdout.writeAll("\n");
        return;
    }

    try ctx.stdout.print(
        "routing evidence ranking ({s}) — declared-experiment samples only, read-only:\n",
        .{result.version},
    );
    try ctx.stdout.print(
        "  gates: minimum_samples={d} quality_floor={d:.2}\n\n",
        .{ gates.minimum_samples, gates.quality_floor },
    );
    if (result.rows.len == 0) {
        try ctx.stdout.writeAll("  (no cohort-eligible declared-experiment samples)\n");
        return;
    }
    try ctx.stdout.print(
        "  {s: <4} {s: <28} {s: >7} {s: >8} {s: >8} {s: >8} {s: >9} {s: >7}\n",
        .{ "rank", "candidate", "samples", "success", "raw", "wilson", "gatefail", "excess" },
    );
    for (result.rows) |row| {
        var rank_buf: [8]u8 = undefined;
        const rank_text: []const u8 = if (row.rank) |r|
            std.fmt.bufPrint(&rank_buf, "{d}", .{r}) catch "?"
        else if (row.insufficient_data)
            "n/a"
        else
            "--";
        try ctx.stdout.print(
            "  {s: <4} {s: <28} {d: >7} {d: >8} {d: >8.3} {d: >8.3} {d: >9.3} {d: >7.2}",
            .{
                rank_text,             row.candidate,                  row.samples,
                row.successes,         row.raw_rate,                   row.wilson_lower,
                row.gate_failure_rate, row.expected_excess_iterations,
            },
        );
        if (row.insufficient_data) {
            try ctx.stdout.writeAll("  insufficient_data");
        } else if (row.below_quality_floor) {
            try ctx.stdout.writeAll("  below_quality_floor");
        }
        try ctx.stdout.writeAll("\n");
    }
    if (result.recommended) |i| {
        try ctx.stdout.print("\nrecommended: {s} (preview only; writes nothing)\n", .{result.rows[i].candidate});
    } else {
        try ctx.stdout.print("\nno recommendation: {s}\n", .{result.no_recommendation_reason orelse "gated"});
    }
}

fn handleEvals(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "evals" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e|
        exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});

    // Cohort flags select the evidence-backed path. Evidence is never pooled
    // across cohorts, so ranking requires the caller to name one exactly
    // rather than defaulting to "everything" — an aggregate over mixed
    // cohorts would be a number with no meaning.
    if (optFlag(args.vendor)) |vendor| {
        try rankCohort(ctx, d, args, vendor);
        return;
    }

    const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    const file_content: ?[]u8 = std.Io.Dir.cwd().readFileAlloc(ctx.io, path, ctx.allocator, .unlimited) catch |e| switch (e) {
        error.FileNotFound => null,
        else => exit.die(ctx, e, "reading config file: {s}", .{@errorName(e)}),
    };
    defer if (file_content) |fc| ctx.allocator.free(fc);

    var resolved = engine.config.resolve(ctx.allocator, file_content, ctx.environ, null) catch |e|
        exit.die(ctx, e, "resolving configuration: {s}", .{@errorName(e)});
    defer resolved.deinit(ctx.allocator);

    var result = engine.evals.aggregate(d, ctx.allocator) catch |e|
        exit.die(ctx, e, "aggregating routing evals: {s}", .{@errorName(e)});
    defer result.deinit(ctx.allocator);

    if (args.json) {
        try ctx.stdout.print("{{\"scorecard\":", .{});
        try std.json.Stringify.value(result.scorecard, .{}, ctx.stdout);
        try ctx.stdout.print(",\"recommendations\":", .{});
        try std.json.Stringify.value(result.recommendations, .{}, ctx.stdout);
        try ctx.stdout.print(",\"signals_sourced\":", .{});
        try std.json.Stringify.value(result.signals_sourced, .{}, ctx.stdout);
        try ctx.stdout.print(",\"legacy_dispatch_notes_skipped\":{d}}}\n", .{result.legacy_dispatch_notes_skipped});
        return;
    }

    try ctx.stdout.print("routing evals scorecard (per work-type, candidate) — read-only, writes nothing:\n", .{});
    if (result.scorecard.len == 0) {
        try ctx.stdout.print("  (no completed dispatch history recorded yet)\n", .{});
    }
    for (result.scorecard) |row| {
        if (row.insufficient_data) {
            try ctx.stdout.print(
                "  {s: <14} {s: <24} insufficient-data  [{s}/{s}]\n",
                .{ row.work_type, row.candidate, row.vendor orelse "?", row.tier },
            );
        } else {
            try ctx.stdout.print(
                "  {s: <14} {s: <24} rank {d: <2} {d}/{d} approved  avg {d:.2} iter  [{s}/{s}]\n",
                .{ row.work_type, row.candidate, row.rank.?, row.approved_count, row.dispatch_count, row.avg_iterations, row.vendor orelse "?", row.tier },
            );
        }
    }

    try ctx.stdout.print("\nrecommendations (preview only — writes nothing; apply is a separate operator-gated step):\n", .{});
    if (result.recommendations.len == 0) {
        try ctx.stdout.print("  (none — no work type has scored dispatch history yet)\n", .{});
    }
    for (result.recommendations) |rec| {
        try ctx.stdout.print(
            "  {s} → {s} [{s}/{s}]: {s}\n",
            .{ rec.work_type, rec.candidate, rec.vendor orelse "?", rec.tier, rec.rationale },
        );
    }

    try ctx.stdout.print(
        "\nsignals sourced: reviewer_disposition={s} iteration_count={s} quality_gate_pass_fail={s} test_coder_expansion={s}\n",
        .{
            if (result.signals_sourced.reviewer_disposition) "yes" else "no",
            if (result.signals_sourced.iteration_count) "yes" else "no",
            if (result.signals_sourced.quality_gate_pass_fail) "yes" else "no",
            if (result.signals_sourced.test_coder_expansion) "yes" else "no",
        },
    );
    if (result.legacy_dispatch_notes_skipped > 0) {
        try ctx.stdout.print(
            "note: {d} dispatch note(s) skipped — missing/malformed model_choice work_type\n",
            .{result.legacy_dispatch_notes_skipped},
        );
    }
}
