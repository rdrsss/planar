//! handlers/ext/propagate — `planar ext propagate <plan-id> [--system <slug>] [flags]`
//!
//! Implements the full Go parity surface for ext propagate, including:
//!
//!   - Happy-path tree walk + per-entity render + remote POST + link record
//!     (M10 baseline).
//!   - Strategy stickiness cache via `external_links.config_json` (M3.2 / Go
//!     engine.go::resolveStrategy).
//!   - `--restrategize [--yes]` — abandon prior counterparts and re-propagate
//!     under a fresh strategy. The old remote items are NOT deleted; Planar
//!     stops tracking them and records `sync_events(outcome='strategy-abandoned')`.
//!   - `--verify-counterparts` — probe each linked entity via the adapter's
//!     pull path; entities returning 404 (`Error.NotFound`) are reported as
//!     "missing" with a `sync_events(outcome='counterpart-missing')` row.
//!   - `--unlink` (requires --verify-counterparts) — delete the link row for
//!     each missing counterpart.
//!   - `--recreate` (requires --verify-counterparts) — delete the link row for
//!     each missing counterpart; a follow-up `propagate` call recreates fresh
//!     (matches Go's E-11 behaviour: --recreate is a "remove now, recreate on
//!     next pass" cycle).
//!   - `--github-strategy <name>` — override auto-detected GitHub strategy.
//!     Accepted values: parent-issue, projects-v2, tracking-issue.
//!     All three strategies run end-to-end in zig. `parent-issue` and
//!     `tracking-issue` use REST; `projects-v2` (Cycle B''', task 2349)
//!     uses the GraphQL ProjectsV2 surface to create one Project per anchor
//!     plan, add issues across the touched repos as Project items, and
//!     optionally set the configured `Parent` field on task items.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");
const adapter_factory = @import("adapter_factory.zig");
const remote = @import("remote.zig");
const tmpl_common = @import("../templates/common.zig");
const ext_strategy = @import("engine").extsync.strategy;

/// RunOpts captures the propagate options passed in from either the
/// `ext propagate` handler or the `link --propagate` convenience verb.
pub const RunOpts = struct {
    system_slug: ?[]const u8 = null,
    dry_run: bool = false,
    sync: ?[]const u8 = null,
    json: bool = false,

    // Advanced flags wired in Cycle B (task 2345, plan 314):
    restrategize: bool = false,
    auto_yes: bool = false,
    verify_counterparts: bool = false,
    unlink_missing: bool = false,
    recreate_missing: bool = false,
    /// `--github-strategy` CLI flag value, already validated. One of
    /// "parent-issue", "projects-v2", "tracking-issue", or null.
    github_strategy: ?[]const u8 = null,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "ext", "propagate" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    _ = args.scope;

    // Up-front flag validation (mirrors Go's RunE preflight).
    if (args.unlink and args.recreate) {
        exit.die(ctx, error.InvalidInput, "--unlink and --recreate are mutually exclusive", .{});
    }
    if ((args.unlink or args.recreate) and !args.verify_counterparts) {
        exit.die(ctx, error.InvalidInput, "--unlink and --recreate require --verify-counterparts", .{});
    }
    if (args.github_strategy != null and args.restrategize) {
        exit.die(ctx, error.InvalidInput, "--github-strategy and --restrategize are mutually exclusive", .{});
    }
    if (args.github_strategy) |strategy| {
        const ok = std.mem.eql(u8, strategy, "parent-issue") or
            std.mem.eql(u8, strategy, "projects-v2") or
            std.mem.eql(u8, strategy, "tracking-issue");
        if (!ok) {
            exit.die(ctx, error.InvalidInput, "invalid --github-strategy '{s}'; accepted: parent-issue, projects-v2, tracking-issue", .{strategy});
        }
    }
    if (args.sync) |sync_text| {
        _ = engine.external.link.SyncDirection.fromText(sync_text) orelse
            exit.die(ctx, error.InvalidInput, "invalid --sync '{s}'; accepted: read-only, write-back, two-way", .{sync_text});
    }

    // Resolve anchor plan.
    const plan_id = resolvePlanId(d, args.plan_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "plan '{s}' not found", .{args.plan_id}),
        else => exit.die(ctx, e, "ext propagate: lookup plan: {s}", .{@errorName(e)}),
    };

    try runForPlan(ctx, d, plan_id, .{
        .system_slug = args.system,
        .dry_run = args.dry_run,
        .sync = args.sync,
        .json = args.json,
        .restrategize = args.restrategize,
        .auto_yes = args.yes,
        .verify_counterparts = args.verify_counterparts,
        .unlink_missing = args.unlink,
        .recreate_missing = args.recreate,
        .github_strategy = args.github_strategy,
    });
}

/// runForPlan executes the full propagation flow for `plan_id` and the
/// resolved options. Public so the `link --propagate` convenience verb can
/// reuse it.
pub fn runForPlan(
    ctx: *const runtime.Ctx,
    d: anytype,
    plan_id: i64,
    opts: RunOpts,
) anyerror!void {

    // Resolve target system.
    const sys = resolveSystem(d, ctx.allocator, opts.system_slug) catch |e| switch (e) {
        error.NotFound => {
            const slug = opts.system_slug orelse "";
            exit.die(ctx, error.NotFound, "external system '{s}' not found", .{slug});
        },
        error.NoSystemsRegistered => exit.die(ctx, error.NotFound, "no external systems registered; run 'planar ext register' first", .{}),
        else => exit.die(ctx, e, "ext propagate: lookup system: {s}", .{@errorName(e)}),
    };
    defer engine.external.system.deinit(sys, ctx.allocator);

    // --github-strategy validation against system kind + cached strategy.
    var override_strategy_kind: ?[]const u8 = null;
    if (opts.github_strategy) |gs| {
        if (sys.kind != .@"github-issues") {
            exit.die(
                ctx,
                error.InvalidInput,
                "--github-strategy is only valid for github-issues systems; system '{s}' has kind '{s}'",
                .{ sys.slug, sys.kind.toText() },
            );
        }
        const kind_full = githubStrategyFlagToKind(gs);
        const cached_opt = ext_strategy.readCachedStrategy(ctx.allocator, d, plan_id, sys.id) catch |e|
            exit.die(ctx, e, "ext propagate: read cached strategy: {s}", .{@errorName(e)});
        if (cached_opt) |cached| {
            defer ctx.allocator.free(cached);
            if (!std.mem.eql(u8, cached, kind_full)) {
                exit.die(
                    ctx,
                    error.InvalidInput,
                    "feature already has cached strategy '{s}' which differs from --github-strategy '{s}'; use --restrategize to change the cached strategy",
                    .{ cached, gs },
                );
            }
        }
        override_strategy_kind = kind_full;
    }

    // Determine strategy: cache → override → fresh → restrategize.
    var report_strategy_kind: []const u8 = undefined;
    var abandoned_count: usize = 0;

    if (override_strategy_kind) |k| {
        report_strategy_kind = k;
    } else {
        const cached_opt = ext_strategy.readCachedStrategy(ctx.allocator, d, plan_id, sys.id) catch |e|
            exit.die(ctx, e, "ext propagate: read cached strategy: {s}", .{@errorName(e)});

        if (opts.restrategize) {
            // Fresh selection.
            const fresh = engine.extsync.propagate.selectStrategy(d, plan_id, sys.kind.toText()) catch |e|
                exit.die(ctx, e, "ext propagate: pick strategy: {s}", .{@errorName(e)});
            if (cached_opt) |cached| {
                defer ctx.allocator.free(cached);
                if (!std.mem.eql(u8, cached, fresh.kind)) {
                    // Strategy differs: prompt unless --yes.
                    if (!opts.auto_yes) {
                        const confirmed = confirmRestrategize(ctx, cached, fresh.kind) catch |e|
                            exit.die(ctx, e, "ext propagate: reading confirmation: {s}", .{@errorName(e)});
                        if (!confirmed) {
                            exit.die(ctx, error.InvalidInput, "restrategize cancelled by user", .{});
                        }
                    }
                    abandoned_count = ext_strategy.abandonCounterparts(ctx.allocator, d, plan_id, sys.id, cached, fresh.kind) catch |e|
                        exit.die(ctx, e, "ext propagate: abandon counterparts: {s}", .{@errorName(e)});
                }
                // If equal: no-op restrategize.
            }
            report_strategy_kind = fresh.kind;
        } else if (cached_opt) |cached| {
            // Sticky cache hit.
            report_strategy_kind = cached; // ownership transferred; freed at function end.
        } else {
            const fresh = engine.extsync.propagate.selectStrategy(d, plan_id, sys.kind.toText()) catch |e|
                exit.die(ctx, e, "ext propagate: pick strategy: {s}", .{@errorName(e)});
            report_strategy_kind = fresh.kind;
        }
    }
    defer {
        // report_strategy_kind is allocator-owned only when it came from the
        // cached path; the strategyForSystem and override paths return static
        // strings. The cached path's `cached` slice is freed inside its branch
        // when it doesn't propagate up. When the cache value DOES become
        // report_strategy_kind we hold ownership; release it here.
        // Detection: every non-cache path returns a static string literal from
        // strategyForSystem or githubStrategyFlagToKind. We can recognise
        // static-versus-owned by attempting to free unconditionally — but
        // freeing static slices crashes in debug builds. Instead, gate the
        // free on whether the slice equals one of the known static values.
        if (!isStaticStrategy(report_strategy_kind)) ctx.allocator.free(report_strategy_kind);
    }

    // Resolve the per-entity template kinds for the (possibly cache-supplied)
    // strategy. The walkTree below dispatches identically to the M10 happy
    // path; only the cache-write decision uses the strategy kind.
    const strategy_kinds = strategyTemplateKinds(sys.kind.toText(), report_strategy_kind) catch |e|
        exit.die(ctx, e, "ext propagate: map strategy '{s}' to templates: {s}", .{ report_strategy_kind, @errorName(e) });

    // Templates root + sync direction default.
    const root = tmpl_common.resolveTemplatesRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving templates root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const sync_text: []const u8 = opts.sync orelse "read-only";
    const sync_dir = engine.external.link.SyncDirection.fromText(sync_text).?;

    // Walk the feature tree.
    const tree = engine.extsync.propagate.walkTree(ctx.allocator, d, plan_id) catch |e|
        exit.die(ctx, e, "ext propagate: walk tree: {s}", .{@errorName(e)});
    defer engine.extsync.propagate.freeTree(tree, ctx.allocator);

    // Build adapter handle for actual POSTs (skipped under --dry-run unless
    // --verify-counterparts is set — verify needs the adapter to probe).
    var adp: ?*adapter_factory.Handle = null;
    defer if (adp) |h| {
        h.deinit();
        ctx.allocator.destroy(h);
    };
    const need_adapter = !opts.dry_run or opts.verify_counterparts;
    if (need_adapter) {
        adp = adapter_factory.build(ctx.allocator, ctx.io, &ctx.environ, sys) catch |e| switch (e) {
            error.TokenEnvVarMissing => exit.die(ctx, error.InvalidInput, "token env var '{s}' is not set", .{sys.auth_ref}),
            error.UnsupportedAuthMethod => exit.die(ctx, error.InvalidInput, "oauth-stored auth not yet supported (matches Go: deferred)", .{}),
            error.GhCliNotFound => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh` binary not on PATH", .{}),
            error.GhCliFailed => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh auth token` returned non-zero (run `gh auth login`)", .{}),
            error.GhCliEmptyToken => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh auth token` returned empty output", .{}),
            error.UnsupportedSystemKind => exit.die(ctx, error.InvalidInput, "system kind '{s}' is not supported", .{sys.kind.toText()}),
            else => exit.die(ctx, e, "ext propagate: build adapter: {s}", .{@errorName(e)}),
        };
    }

    var created_count: usize = 0;
    var skipped_count: usize = 0;
    var failed_count: usize = 0;
    var verified_count: usize = 0;
    var missing_count: usize = 0;
    var results_buf: std.ArrayList(LineResult) = .empty;
    defer {
        for (results_buf.items) |r| {
            ctx.allocator.free(r.entity_kind);
            ctx.allocator.free(r.title);
            ctx.allocator.free(r.external_id);
            if (r.error_message.len > 0) ctx.allocator.free(r.error_message);
        }
        results_buf.deinit(ctx.allocator);
    }

    // --- parent-issue strategy short-circuit -------------------------------
    //
    // When the strategy resolves to `github-parent-issue` (either auto-detected
    // or `--github-strategy parent-issue`), bypass the generic template/render
    // walk and run the dedicated engine path that creates a parent issue,
    // sub-issues for child plans, and sub-issues for tasks, recording links and
    // sync_events per Go parity. The output buffer is filled with LineResult
    // rows so the existing rendering code below works unchanged.
    if (std.mem.eql(u8, report_strategy_kind, "github-parent-issue")) {
        runParentIssueStrategy(
            ctx,
            d,
            plan_id,
            sys,
            opts,
            adp,
            sync_dir,
            &results_buf,
            &created_count,
            &skipped_count,
            &failed_count,
        ) catch |e| {
            exit.die(ctx, e, "ext propagate: parent-issue: {s}", .{@errorName(e)});
        };
    } else if (std.mem.eql(u8, report_strategy_kind, "github-projects-v2")) {
        runProjectsV2Strategy(
            ctx,
            d,
            plan_id,
            sys,
            opts,
            adp,
            sync_dir,
            &results_buf,
            &created_count,
            &skipped_count,
            &failed_count,
        ) catch |e| {
            exit.die(ctx, e, "ext propagate: projects-v2: {s}", .{@errorName(e)});
        };
    } else for (tree) |entry| {
        const entity_kind_str: []const u8 = if (entry.kind == .task) "task" else "plan";
        const template_kind: []const u8 = switch (entry.kind) {
            .plan_anchor => strategy_kinds.plan_anchor_kind,
            .plan_child => strategy_kinds.plan_child_kind,
            .task => strategy_kinds.task_kind,
        };

        // Idempotent skip.
        const existing = engine.extsync.propagate.loadExistingMirror(ctx.allocator, d, entity_kind_str, entry.id, sys.id) catch |e|
            exit.die(ctx, e, "ext propagate: read mirror link: {s}", .{@errorName(e)});
        if (existing.len > 0) {
            try results_buf.append(ctx.allocator, .{
                .entity_kind = try ctx.allocator.dupe(u8, entity_kind_str),
                .entity_id = entry.id,
                .title = try ctx.allocator.dupe(u8, entry.title),
                .op = "skipped",
                .external_id = existing,
            });
            skipped_count += 1;
            continue;
        }
        ctx.allocator.free(existing);

        // Build context + load template + render.
        var owned = switch (entry.kind) {
            .plan_anchor, .plan_child => engine.templates.builder.buildPlanContext(ctx.allocator, d, entry.id) catch |e| {
                try recordFailure(ctx.allocator, &results_buf, entity_kind_str, entry, e);
                failed_count += 1;
                continue;
            },
            .task => engine.templates.builder.buildTaskContext(ctx.allocator, d, entry.id) catch |e| {
                try recordFailure(ctx.allocator, &results_buf, entity_kind_str, entry, e);
                failed_count += 1;
                continue;
            },
        };
        defer owned.deinit();

        const tmpl = engine.templates.load(ctx.allocator, "default", sys.kind.toText(), template_kind, root) catch |e| {
            try recordFailure(ctx.allocator, &results_buf, entity_kind_str, entry, e);
            failed_count += 1;
            continue;
        };
        defer engine.templates.deinitTemplate(tmpl, ctx.allocator);

        var rendered = engine.templates.renderTemplate(ctx.allocator, tmpl.fields, owned.ctx) catch |e| {
            try recordFailure(ctx.allocator, &results_buf, entity_kind_str, entry, e);
            failed_count += 1;
            continue;
        };
        defer rendered.deinit();

        const payload = rendered.toJson(ctx.allocator) catch |e| {
            try recordFailure(ctx.allocator, &results_buf, entity_kind_str, entry, e);
            failed_count += 1;
            continue;
        };
        defer ctx.allocator.free(payload);

        if (opts.dry_run) {
            try results_buf.append(ctx.allocator, .{
                .entity_kind = try ctx.allocator.dupe(u8, entity_kind_str),
                .entity_id = entry.id,
                .title = try ctx.allocator.dupe(u8, entry.title),
                .op = "planned",
                .external_id = try std.fmt.allocPrint(ctx.allocator, "<{s}>", .{template_kind}),
            });
            created_count += 1;
            continue;
        }

        const created = remote.createRemote(adp.?, ctx.allocator, sys, payload) catch |e| {
            try recordFailure(ctx.allocator, &results_buf, entity_kind_str, entry, e);
            failed_count += 1;
            continue;
        };
        defer ctx.allocator.free(created.external_url);

        // Record external_links row. Anchor plan also carries the strategy
        // cache in config_json so a future propagate hits the stickiness path.
        const entity_kind_enum = engine.external.link.ExternalEntityKind.fromText(entity_kind_str) orelse {
            ctx.allocator.free(created.external_id);
            try recordFailure(ctx.allocator, &results_buf, entity_kind_str, entry, error.InvalidInput);
            failed_count += 1;
            continue;
        };
        const is_anchor = entry.kind == .plan_anchor;
        const anchor_config_json = if (is_anchor) try std.fmt.allocPrint(
            ctx.allocator,
            "{{\"strategy\":\"{s}\"}}",
            .{report_strategy_kind},
        ) else null;
        defer if (anchor_config_json) |s| ctx.allocator.free(s);

        const link = engine.external.link.create(d, ctx.allocator, .{
            .entity_kind = entity_kind_enum,
            .entity_id = entry.id,
            .system_id = sys.id,
            .external_id = created.external_id,
            .external_url = if (created.external_url.len > 0) created.external_url else null,
            .link_role = .mirror,
            .sync_direction = sync_dir,
            .initial_status = .ok,
            .config_json = anchor_config_json,
        }) catch |e| {
            ctx.allocator.free(created.external_id);
            try recordFailure(ctx.allocator, &results_buf, entity_kind_str, entry, e);
            failed_count += 1;
            continue;
        };
        defer engine.external.link.deinit(link, ctx.allocator);

        try results_buf.append(ctx.allocator, .{
            .entity_kind = try ctx.allocator.dupe(u8, entity_kind_str),
            .entity_id = entry.id,
            .title = try ctx.allocator.dupe(u8, entry.title),
            .op = "created",
            .external_id = created.external_id, // ownership transferred to results
        });
        created_count += 1;
    }

    // --- verify-counterparts pass -----------------------------------------

    if (opts.verify_counterparts and !opts.dry_run) {
        const links = ext_strategy.listMirrorLinksInTree(ctx.allocator, d, plan_id, sys.id) catch |e|
            exit.die(ctx, e, "ext propagate: list links for verify: {s}", .{@errorName(e)});
        defer ext_strategy.deinitMirrorLinks(links, ctx.allocator);

        for (links) |link_row| {
            const probe_result = probeCounterpart(adp.?, ctx.allocator, link_row.external_id);
            switch (probe_result) {
                .present => {
                    try results_buf.append(ctx.allocator, .{
                        .entity_kind = try ctx.allocator.dupe(u8, link_row.entity_kind),
                        .entity_id = link_row.entity_id,
                        .title = try ctx.allocator.dupe(u8, ""),
                        .op = "verified",
                        .external_id = try ctx.allocator.dupe(u8, link_row.external_id),
                    });
                    verified_count += 1;
                },
                .missing => {
                    ext_strategy.recordCounterpartMissing(
                        ctx.allocator,
                        d,
                        link_row.id,
                        link_row.entity_kind,
                        link_row.entity_id,
                        link_row.external_id,
                        opts.unlink_missing or opts.recreate_missing,
                    ) catch |e|
                        exit.die(ctx, e, "ext propagate: record counterpart-missing: {s}", .{@errorName(e)});

                    try results_buf.append(ctx.allocator, .{
                        .entity_kind = try ctx.allocator.dupe(u8, link_row.entity_kind),
                        .entity_id = link_row.entity_id,
                        .title = try ctx.allocator.dupe(u8, ""),
                        .op = "missing",
                        .external_id = try ctx.allocator.dupe(u8, link_row.external_id),
                    });
                    missing_count += 1;
                },
                .probe_error => |e| {
                    // Non-fatal: log via stderr, skip the link.
                    try ctx.stderr.print("warning: counterpart probe failed for {s} ({s}); skipping\n", .{ link_row.external_id, @errorName(e) });
                },
            }
        }
    }

    // --- output -----------------------------------------------------------

    const ok = failed_count == 0 and missing_count == 0;

    if (opts.json) {
        try ctx.stdout.print("{{\"ok\":{s},\"plan_id\":{d},\"system\":", .{ if (ok) "true" else "false", plan_id });
        try output.writeJsonString(ctx.stdout, sys.slug);
        try ctx.stdout.print(",\"strategy\":", .{});
        try output.writeJsonString(ctx.stdout, report_strategy_kind);
        try ctx.stdout.print(",\"created\":{d},\"skipped\":{d},\"failed\":{d}", .{ created_count, skipped_count, failed_count });
        if (verified_count > 0) try ctx.stdout.print(",\"verified\":{d}", .{verified_count});
        if (abandoned_count > 0) try ctx.stdout.print(",\"abandoned\":{d}", .{abandoned_count});
        if (missing_count > 0) try ctx.stdout.print(",\"missing\":{d}", .{missing_count});
        try ctx.stdout.print(",\"results\":[", .{});
        for (results_buf.items, 0..) |r, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{{\"entity_kind\":", .{});
            try output.writeJsonString(ctx.stdout, r.entity_kind);
            try ctx.stdout.print(",\"entity_id\":{d},\"title\":", .{r.entity_id});
            try output.writeJsonString(ctx.stdout, r.title);
            try ctx.stdout.print(",\"op\":", .{});
            try output.writeJsonString(ctx.stdout, r.op);
            try ctx.stdout.print(",\"external_id\":", .{});
            try output.writeJsonString(ctx.stdout, r.external_id);
            if (r.error_message.len > 0) {
                try ctx.stdout.print(",\"error\":", .{});
                try output.writeJsonString(ctx.stdout, r.error_message);
            }
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]}}\n", .{});
    } else {
        const prefix: []const u8 = if (opts.dry_run) "(dry-run) " else "";
        try ctx.stdout.print("{s}propagated plan {d} to {s} via strategy {s} (created {d}, skipped {d}, failed {d}", .{
            prefix, plan_id, sys.slug, report_strategy_kind, created_count, skipped_count, failed_count,
        });
        if (verified_count > 0) try ctx.stdout.print(", verified {d}", .{verified_count});
        if (abandoned_count > 0) try ctx.stdout.print(", abandoned {d}", .{abandoned_count});
        if (missing_count > 0) try ctx.stdout.print(", missing {d}", .{missing_count});
        try ctx.stdout.print(")\n", .{});
        for (results_buf.items) |r| {
            if (std.mem.eql(u8, r.op, "created") or std.mem.eql(u8, r.op, "planned")) {
                try ctx.stdout.print("  {s}    {s}:{d} {s} -> {s}\n", .{ r.op, r.entity_kind, r.entity_id, r.title, r.external_id });
            } else if (std.mem.eql(u8, r.op, "skipped")) {
                try ctx.stdout.print("  skipped   {s}:{d} {s} (already linked: {s})\n", .{ r.entity_kind, r.entity_id, r.title, r.external_id });
            } else if (std.mem.eql(u8, r.op, "verified")) {
                try ctx.stdout.print("  verified  {s}:{d}  ({s} still present)\n", .{ r.entity_kind, r.entity_id, r.external_id });
            } else if (std.mem.eql(u8, r.op, "missing")) {
                try ctx.stdout.print("  MISSING   {s}:{d}  (counterpart {s} not found on remote)\n", .{ r.entity_kind, r.entity_id, r.external_id });
            } else {
                try ctx.stdout.print("  FAILED    {s}:{d} {s} ({s})\n", .{ r.entity_kind, r.entity_id, r.title, r.error_message });
            }
        }
        if (missing_count > 0 and !opts.unlink_missing and !opts.recreate_missing) {
            try ctx.stdout.print("  {d} counterpart(s) missing; use --unlink or --recreate to remediate\n", .{missing_count});
        }
    }

    if (missing_count > 0 and !opts.unlink_missing and !opts.recreate_missing) {
        exit.die(ctx, error.InvalidInput, "{d} counterpart(s) missing during --verify-counterparts", .{missing_count});
    }
    if (failed_count > 0) {
        exit.die(ctx, error.InvalidInput, "{d} entity/entities failed during propagation", .{failed_count});
    }
}

// ---- per-strategy template kinds -------------------------------------------

/// strategyTemplateKinds maps a (system_kind, strategy_kind) pair to the
/// per-entity template kind triple. Mirrors `strategyForSystem` in
/// engine/extsync/propagate.zig but allows cached/override strategies to pick
/// the same template surface (currently every GitHub strategy variant uses the
/// same triple in the zig port — only the cache/audit value differs).
fn strategyTemplateKinds(system_kind: []const u8, strategy_kind: []const u8) !engine.extsync.propagate.Strategy {
    if (std.mem.eql(u8, system_kind, "jira")) {
        return .{
            .kind = "jira-epic",
            .plan_anchor_kind = "epic",
            .plan_child_kind = "story",
            .task_kind = "sub-task",
        };
    }
    if (std.mem.eql(u8, system_kind, "github-issues")) {
        // tracking-issue and (eventually) parent-issue/projects-v2 share the
        // same per-entity template kinds in the zig port. Cache the strategy
        // separately via writeAnchorConfigJSON.
        return .{
            .kind = strategy_kind,
            .plan_anchor_kind = "parent-issue",
            .plan_child_kind = "issue",
            .task_kind = "sub-task",
        };
    }
    return error.UnsupportedSystemKind;
}

// ---- restrategize prompt ---------------------------------------------------

/// confirmRestrategize prints the Go-compatible prompt and reads a single
/// line from stdin. Returns true for "y" / "Y" / "yes", false otherwise.
fn confirmRestrategize(ctx: *const runtime.Ctx, old: []const u8, new: []const u8) !bool {
    try ctx.stdout.print("Restrategize: cached strategy is \"{s}\", new strategy would be \"{s}\".\n", .{ old, new });
    try ctx.stdout.print("Abandoning old counterparts will not delete them on the remote; Planar will stop tracking them.\n", .{});
    try ctx.stdout.print("Confirm? [y/N] ", .{});
    try ctx.stdout.flush();

    var buf: [256]u8 = undefined;
    var stdin_reader = std.Io.File.Reader.init(.stdin(), ctx.io, &buf);
    var line_buf: [256]u8 = undefined;
    var fbs = std.Io.Writer.fixed(&line_buf);
    _ = stdin_reader.interface.streamDelimiter(&fbs, '\n') catch |e| {
        if (e != error.EndOfStream) return e;
    };
    const raw = fbs.buffered();
    const trimmed = std.mem.trim(u8, raw, " \t\r\n");
    // Lowercase compare.
    if (trimmed.len == 0) return false;
    var lc_buf: [16]u8 = undefined;
    const n = @min(trimmed.len, lc_buf.len);
    for (trimmed[0..n], 0..) |c, i| lc_buf[i] = std.ascii.toLower(c);
    const lc = lc_buf[0..n];
    return std.mem.eql(u8, lc, "y") or std.mem.eql(u8, lc, "yes");
}

// ---- counterpart probe -----------------------------------------------------

const ProbeResult = union(enum) {
    present,
    missing,
    probe_error: anyerror,
};

/// probeCounterpart dispatches a single GET against the remote for the given
/// external_id. A 404 (`Error.NotFound` from the adapter) maps to `.missing`;
/// success maps to `.present`; any other error maps to `.probe_error` so the
/// caller can warn and continue (matches Go's non-fatal probe behaviour).
fn probeCounterpart(h: *adapter_factory.Handle, allocator: std.mem.Allocator, external_id: []const u8) ProbeResult {
    switch (h.kind) {
        .jira => {
            const adp = &h.jira_adapter.?;
            const state = adp.pull(allocator, external_id) catch |e| {
                if (e == error.NotFound) return .missing;
                return .{ .probe_error = e };
            };
            engine.extsync.common.deinitRemoteState(state, allocator);
            return .present;
        },
        .github => {
            const adp = &h.github_adapter.?;
            const state = adp.pull(allocator, external_id) catch |e| {
                if (e == error.NotFound) return .missing;
                return .{ .probe_error = e };
            };
            engine.extsync.common.deinitRemoteState(state, allocator);
            return .present;
        },
    }
}

// ---- strategy-kind ownership helper ----------------------------------------

/// isStaticStrategy reports whether `s` is one of the strategyForSystem /
/// githubStrategyFlagToKind static string literals. Used by runForPlan's defer
/// to decide whether to free `report_strategy_kind`.
fn isStaticStrategy(s: []const u8) bool {
    const known = [_][]const u8{
        "jira-epic",
        "github-parent-issue",
        "github-projects-v2",
        "github-tracking-issue",
        "github-zero-repo",
    };
    for (known) |k| {
        if (std.mem.eql(u8, s, k)) return true;
    }
    return false;
}

/// githubStrategyFlagToKind converts the `--github-strategy` CLI flag value
/// to the canonical "github-<name>" cache key. Returns static string literals.
fn githubStrategyFlagToKind(flag: []const u8) []const u8 {
    if (std.mem.eql(u8, flag, "parent-issue")) return "github-parent-issue";
    if (std.mem.eql(u8, flag, "projects-v2")) return "github-projects-v2";
    if (std.mem.eql(u8, flag, "tracking-issue")) return "github-tracking-issue";
    unreachable; // Pre-validated by the handler.
}

// ---- shared types and helpers ----------------------------------------------

const LineResult = struct {
    entity_kind: []const u8,
    entity_id: i64,
    title: []const u8,
    op: []const u8,
    external_id: []const u8,
    error_message: []const u8 = "",
};

fn recordFailure(
    allocator: std.mem.Allocator,
    results: *std.ArrayList(LineResult),
    entity_kind: []const u8,
    entry: engine.extsync.propagate.TreeEntry,
    err: anyerror,
) !void {
    try results.append(allocator, .{
        .entity_kind = try allocator.dupe(u8, entity_kind),
        .entity_id = entry.id,
        .title = try allocator.dupe(u8, entry.title),
        .op = "failed",
        .external_id = try allocator.dupe(u8, ""),
        .error_message = try allocator.dupe(u8, @errorName(err)),
    });
}

/// resolvePlanId accepts either an integer id or a plan slug. Public so the
/// `link --propagate` helper can reuse it.
pub fn resolvePlanId(d: anytype, raw: []const u8) !i64 {
    if (std.fmt.parseInt(i64, raw, 10)) |id| {
        var stmt = try d.prepare("select count(*) from plans where id = ?");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = id }});
        const step = try stmt.step();
        if (step == .done) return error.NotFound;
        if (stmt.columnInt(0) == 0) return error.NotFound;
        return id;
    } else |_| {}

    var stmt = try d.prepare("select id from plans where slug = ? limit 1");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .text = raw }});
    const step = try stmt.step();
    if (step == .done) return error.NotFound;
    return stmt.columnInt(0);
}

// ---- parent-issue strategy dispatch ----------------------------------------

const parent_issue_engine = @import("engine").extsync.parent_issue;
const github_adapter = @import("engine").extsync.github;

/// runParentIssueStrategy bridges the CLI surface and the engine's
/// `parent-issue` flow. It builds a `GhClient` over the resolved GithubAdapter
/// (or a dry-run client when `opts.dry_run` is true) and translates the
/// engine's per-entity results into `results_buf` rows so the existing output
/// rendering code in `runForPlan` works unchanged.
fn runParentIssueStrategy(
    ctx: *const runtime.Ctx,
    d: anytype,
    plan_id: i64,
    sys: engine.external.system.ExternalSystem,
    opts: RunOpts,
    adp_opt: ?*adapter_factory.Handle,
    sync_dir: engine.external.link.SyncDirection,
    results_buf: *std.ArrayList(LineResult),
    created_count: *usize,
    skipped_count: *usize,
    failed_count: *usize,
) !void {
    // Build a GhClient bound to either the real GithubAdapter (when
    // `need_adapter` was true upstream) or a dry-run stub. Under dry-run the
    // engine never calls the client's network functions, so the stub is safe.
    var bridge = ParentIssueBridge{ .handle = adp_opt };
    const client = bridge.client();

    var report = parent_issue_engine.propagateParentIssue(ctx.allocator, d, client, plan_id, .{
        .sys_id = sys.id,
        .sys_slug = sys.slug,
        .dry_run = opts.dry_run,
        .sync_direction = sync_dir,
    }) catch |e| switch (e) {
        parent_issue_engine.Error.NoTouchedRepos => {
            exit.die(ctx, error.InvalidInput, "parent-issue strategy needs a touched repo (add a `tasks.scope_kind='repo'` row or a `task touches add` link)", .{});
        },
        parent_issue_engine.Error.CannotResolveRepo => {
            exit.die(ctx, error.InvalidInput, "parent-issue strategy could not resolve a GitHub owner/repo from the touched project (set `projects.git_remote` or use an `owner/repo` slug)", .{});
        },
        parent_issue_engine.Error.SubIssueUnsupported => {
            exit.die(ctx, error.InvalidInput, "parent-issue strategy: GitHub account does not expose the sub-issue REST endpoint for this repo; use `--github-strategy tracking-issue` instead", .{});
        },
        else => return e,
    };
    defer report.deinit(ctx.allocator);

    created_count.* += report.created;
    skipped_count.* += report.skipped;
    failed_count.* += report.failed;

    for (report.results) |r| {
        const op_str: []const u8 = switch (r.op) {
            .created => "created",
            .skipped => "skipped",
            .failed => "failed",
        };
        try results_buf.append(ctx.allocator, .{
            .entity_kind = try ctx.allocator.dupe(u8, r.entity_kind),
            .entity_id = r.entity_id,
            .title = try ctx.allocator.dupe(u8, r.title),
            .op = op_str,
            .external_id = try ctx.allocator.dupe(u8, r.external_id),
            .error_message = if (r.op == .failed) try ctx.allocator.dupe(u8, r.error_name) else "",
        });
    }
}

/// ParentIssueBridge wraps an `adapter_factory.Handle` so the engine's
/// `GhClient` interface can dispatch REST calls without depending on the CLI
/// runtime. When `handle` is null (dry-run with no adapter built), every
/// callback returns `error.NotPermittedDuringDryRun`. The engine never invokes
/// those callbacks under `dry_run = true`, so the unreachable case is fenced
/// off by an assert message.
const ParentIssueBridge = struct {
    handle: ?*adapter_factory.Handle,

    fn client(self: *ParentIssueBridge) parent_issue_engine.GhClient {
        return .{
            .ctx = self,
            .probeFn = probe,
            .createIssueFn = createIssue,
            .linkSubIssueFn = linkSub,
            .postCommentFn = postComment,
        };
    }

    fn probe(ctx_ptr: *anyopaque, allocator: std.mem.Allocator, owner: []const u8, repo: []const u8) anyerror!void {
        const self: *ParentIssueBridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            adp.linkSubIssueProbe(allocator, owner, repo) catch |e| {
                if (e == github_adapter.Error.NotFound) return error.NotFound;
                return e;
            };
            return;
        }
        return error.NotAGithubAdapter;
    }

    fn createIssue(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
        title: []const u8,
        body: []const u8,
        labels: []const []const u8,
    ) anyerror!parent_issue_engine.CreatedIssue {
        const self: *ParentIssueBridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            const created = try adp.createIssue(allocator, owner, repo, title, body, labels);
            return .{ .number = created.number, .node_id = created.node_id };
        }
        return error.NotAGithubAdapter;
    }

    fn linkSub(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
        parent_number: i64,
        child_number: i64,
    ) anyerror!void {
        const self: *ParentIssueBridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            adp.linkSubIssue(allocator, owner, repo, parent_number, child_number) catch |e| {
                if (e == github_adapter.Error.NotFound) return error.NotFound;
                return e;
            };
            return;
        }
        return error.NotAGithubAdapter;
    }

    fn postComment(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
        external_id: []const u8,
        body: []const u8,
    ) anyerror!void {
        const self: *ParentIssueBridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            return adp.postComment(allocator, external_id, body);
        }
        return error.NotAGithubAdapter;
    }
};

// ---- projects-v2 strategy dispatch -----------------------------------------

const projects_v2_engine = @import("engine").extsync.projects_v2;
const config_path = @import("../config/path.zig");

/// runProjectsV2Strategy bridges the CLI surface and the engine's
/// `projects-v2` flow. It builds a `GhProjectsClient` over the resolved
/// GithubAdapter (or fails fast in dry-run with no adapter — the engine's
/// dry-run still walks the DB but never calls the network functions), resolves
/// `parent_field_names` from the config plane, runs the engine, and translates
/// per-entity results into `results_buf` rows so the existing output rendering
/// works unchanged.
fn runProjectsV2Strategy(
    ctx: *const runtime.Ctx,
    d: anytype,
    plan_id: i64,
    sys: engine.external.system.ExternalSystem,
    opts: RunOpts,
    adp_opt: ?*adapter_factory.Handle,
    sync_dir: engine.external.link.SyncDirection,
    results_buf: *std.ArrayList(LineResult),
    created_count: *usize,
    skipped_count: *usize,
    failed_count: *usize,
) !void {
    // Resolve parent_field_names from the config plane (env > assoc > file >
    // defaults). Empty list is fine — the engine falls back to its own
    // default candidate list.
    var resolved_owned: ?engine.config.Resolved = null;
    defer if (resolved_owned) |*r| r.deinit(ctx.allocator);
    var parent_field_names: []const []const u8 = &.{};
    {
        const cfg_path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch null;
        defer if (cfg_path) |p| ctx.allocator.free(p);
        const file_content: ?[]u8 = if (cfg_path) |p|
            std.Io.Dir.cwd().readFileAlloc(ctx.io, p, ctx.allocator, .unlimited) catch null
        else
            null;
        defer if (file_content) |fc| ctx.allocator.free(fc);
        if (engine.config.resolve(ctx.allocator, file_content, ctx.environ, null)) |r| {
            resolved_owned = r;
            parent_field_names = r.config.external.github_projects.parent_field_names;
        } else |_| {}
    }

    var bridge = ProjectsV2Bridge{ .handle = adp_opt };
    const client = bridge.client();

    var report = projects_v2_engine.propagateProjectsV2(ctx.allocator, d, client, plan_id, .{
        .sys_id = sys.id,
        .sys_slug = sys.slug,
        .dry_run = opts.dry_run,
        .sync_direction = sync_dir,
        .parent_field_names = parent_field_names,
    }) catch |e| switch (e) {
        projects_v2_engine.Error.NoTouchedRepos => {
            exit.die(ctx, error.InvalidInput, "projects-v2 strategy needs touched repos (multi-repo features); use --github-strategy parent-issue for single-repo features", .{});
        },
        projects_v2_engine.Error.ProjectCreateFailed => {
            exit.die(ctx, error.InvalidInput, "projects-v2 strategy: could not create the GitHub Project (token may lack the project scope, or both org and user creation failed)", .{});
        },
        else => return e,
    };
    defer report.deinit(ctx.allocator);

    created_count.* += report.created;
    skipped_count.* += report.skipped;
    failed_count.* += report.failed;

    for (report.results) |r| {
        const op_str: []const u8 = switch (r.op) {
            .created => "created",
            .skipped => "skipped",
            .failed => "failed",
        };
        try results_buf.append(ctx.allocator, .{
            .entity_kind = try ctx.allocator.dupe(u8, r.entity_kind),
            .entity_id = r.entity_id,
            .title = try ctx.allocator.dupe(u8, r.title),
            .op = op_str,
            .external_id = try ctx.allocator.dupe(u8, r.external_id),
            .error_message = if (r.op == .failed) try ctx.allocator.dupe(u8, r.error_name) else "",
        });
    }
}

/// ProjectsV2Bridge wraps an `adapter_factory.Handle` so the engine's
/// `GhProjectsClient` interface can dispatch GraphQL + REST calls without
/// depending on the CLI runtime. When `handle` is null (dry-run with no
/// adapter built), every callback returns `error.NotPermittedDuringDryRun`.
/// The engine never invokes them under `dry_run = true`, so the unreachable
/// case is fenced off in practice.
const ProjectsV2Bridge = struct {
    handle: ?*adapter_factory.Handle,

    fn client(self: *ProjectsV2Bridge) projects_v2_engine.GhProjectsClient {
        return .{
            .ctx = self,
            .getAuthenticatedOwnerFn = getAuthenticatedOwner,
            .createProjectV2Fn = createProjectV2,
            .getProjectV2FieldsFn = getProjectV2Fields,
            .addProjectV2ItemFn = addProjectV2Item,
            .setProjectV2ItemFieldValueFn = setProjectV2ItemFieldValue,
            .createIssueFn = createIssue,
        };
    }

    fn getAuthenticatedOwner(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
    ) anyerror!projects_v2_engine.AuthenticatedOwner {
        const self: *ProjectsV2Bridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            const owner = try adp.getAuthenticatedOwner(allocator);
            // Repackage from adapter shape to engine shape (the engine uses
            // []const []const u8 whereas the adapter returns [][]u8).
            const orgs_engine = try allocator.alloc([]const u8, owner.org_ids.len);
            for (owner.org_ids, 0..) |id, i| orgs_engine[i] = id;
            const user_engine = owner.user_node_id;
            // Free only the outer adapter slice (we hand the inner pointers
            // to the engine, which owns them now).
            allocator.free(owner.org_ids);
            return .{ .user_node_id = user_engine, .org_ids = orgs_engine };
        }
        return error.NotAGithubAdapter;
    }

    fn createProjectV2(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
        owner_node_id: []const u8,
        title: []const u8,
    ) anyerror!projects_v2_engine.CreatedProject {
        const self: *ProjectsV2Bridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            const created = try adp.createProjectV2(allocator, owner_node_id, title);
            return .{ .node_id = created.node_id, .url = created.url };
        }
        return error.NotAGithubAdapter;
    }

    fn getProjectV2Fields(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
    ) anyerror![]projects_v2_engine.ProjectField {
        const self: *ProjectsV2Bridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            const fields = try adp.getProjectV2Fields(allocator, project_node_id);
            // Repackage to engine shape: adapter ProjectField has []u8;
            // engine ProjectField has []const u8. We can reinterpret in place
            // by allocating a new outer slice.
            const out = try allocator.alloc(projects_v2_engine.ProjectField, fields.len);
            for (fields, 0..) |f, i| out[i] = .{ .id = f.id, .name = f.name, .data_type = f.data_type };
            allocator.free(fields);
            return out;
        }
        return error.NotAGithubAdapter;
    }

    fn addProjectV2Item(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
        content_node_id: []const u8,
    ) anyerror![]const u8 {
        const self: *ProjectsV2Bridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            return try adp.addProjectV2Item(allocator, project_node_id, content_node_id);
        }
        return error.NotAGithubAdapter;
    }

    fn setProjectV2ItemFieldValue(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
        item_id: []const u8,
        field_id: []const u8,
        text_value: []const u8,
    ) anyerror!void {
        const self: *ProjectsV2Bridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            return adp.setProjectV2ItemFieldValue(allocator, project_node_id, item_id, field_id, text_value);
        }
        return error.NotAGithubAdapter;
    }

    fn createIssue(
        ctx_ptr: *anyopaque,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
        title: []const u8,
        body: []const u8,
        labels: []const []const u8,
    ) anyerror!projects_v2_engine.CreatedIssue {
        const self: *ProjectsV2Bridge = @ptrCast(@alignCast(ctx_ptr));
        const h = self.handle orelse return error.NotPermittedDuringDryRun;
        if (h.github_adapter) |adp| {
            const created = try adp.createIssue(allocator, owner, repo, title, body, labels);
            return .{ .number = created.number, .node_id = created.node_id };
        }
        return error.NotAGithubAdapter;
    }
};

fn resolveSystem(
    d: anytype,
    allocator: std.mem.Allocator,
    slug_opt: ?[]const u8,
) !engine.external.system.ExternalSystem {
    if (slug_opt) |slug| if (slug.len > 0) {
        return engine.external.system.showBySlug(d, allocator, slug);
    };
    const systems = try engine.external.system.list(d, allocator);
    defer engine.external.system.deinitMany(systems, allocator);
    if (systems.len == 0) return error.NoSystemsRegistered;
    // Dupe the first system, since the slice deinit will free the originals.
    return engine.external.system.showBySlug(d, allocator, systems[0].slug);
}
