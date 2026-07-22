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
const main = @import("../main.zig");
const runtime = @import("runtime");
const output = @import("../output.zig");
const exit = @import("../exit.zig");
const config_path = @import("config/path.zig");

pub const verb: cli.Cmd = .{
    .name = "models",
    .desc = "Discover installed provider CLIs and their model catalogs.",
    .long_desc = "Probe the supported provider CLIs (claude, codex) for\n  installed-state + version, and report their curated model\n  catalogs and the default role→tier→model routing.\n\n  The provider CLIs do not expose a machine-readable model list,\n  so the per-vendor model catalog is curated in-repo; discovery\n  confirms which CLIs are callable on this machine.\n\n  Subcommands:\n    list       Probe + print (read-only).\n    refresh    Probe + print, and write the cache to\n               ${PLANAR_HOME:-~/.planar}/models/catalog.json.\n    sync-doc   Regenerate agents/models.md's ## Tier Table from config;\n               --check reports drift without writing (plan 918 D4).",
    .cmds = &.{
        .{
            .name = "list",
            .desc = "Probe providers and print catalogs + default routing.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleList),
        },
        .{
            .name = "refresh",
            .desc = "Probe providers and write the catalog cache under ~/.planar/models/.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleRefresh),
        },
        .{
            .name = "routing",
            .desc = "Print the effective role→vendor/tier/model routing (resolved via config).",
            .long_desc = "Resolve each role through the shared model resolver against the\n  effective config ([models.<vendor>] tiers, [roles] role→tier,\n  [role_vendors] role→vendor, [defaults].vendor) and print the\n  result with provenance. This is the routing an external workflow\n  harness consumes to pick a worker model per role.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleRouting),
        },
        .{
            .name = "apply",
            .desc = "Scaffold the [models]/[roles] config block into the config file.",
            .long_desc = "Write the curated model tier maps + role routing into the\n  resolved config file as an editable starting point. Skips when a\n  [models] section is already present unless --force appends anyway.",
            .flags = &.{
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleApply),
        },
        .{
            .name = "candidates",
            .desc = "Show each tier's candidate list and the work-type routing map with provenance.",
            .long_desc = "Resolve the effective config ([models.<vendor>.<tier>] candidate\n  lists — scalar or ordered list, plan 899 D3 — and the\n  [routing.<vendor>.<tier>] work-type routing map, plan 899 D4/D9/D10/D11)\n  and print both with provenance. list[0] in a tier's candidate list is\n  always the tier default; the routing map names, per work type\n  (schema/engine/architectural/cli/feature/mechanical), which candidate\n  in that list `resolve(role, work_type)` selects. Read-only.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleCandidates),
        },
        .{
            .name = "sync-doc",
            .desc = "Regenerate agents/models.md's ## Tier Table from the resolved config.",
            .long_desc = "Regenerate ONLY the `## Tier Table` section of `agents/models.md`\n  from `[models.<vendor>.<tier>]` (resolved via the same shared\n  resolver `models routing`/`models candidates` use), preserving every\n  other line byte-for-byte. Idempotent: running twice produces no\n  diff. `--check` is read-only: it exits non-zero naming the\n  divergence when the committed table disagrees with a fresh\n  regeneration, and exits 0 (writing nothing) when already in sync.\n  This is the drift gate that replaces skillrender's old render-time\n  patch (plan 918 D4).",
            .flags = &.{
                .{ .long = "--out", .kind = .string, .default = .{ .string = "." } },
                .{ .long = "--check", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleSyncDoc),
        },
        .{
            .name = "evals",
            .desc = "Aggregate completed dispatch outcomes into a per-(work-type, candidate) scorecard and preview-only recommendation.",
            .long_desc = "Read-only aggregation (plan 898/904, tech-spec 520 D8) over the\n  `dispatch_shape` / `model_choice` note convention in `session_entries`\n  (agents/orchestrator.md step 8a), joined with `agent_work_claims`\n  (terminal disposition) and `agent_actions` (test-coder expansion\n  outcome). Emits a per-(work-type, candidate) scorecard and a\n  recommended routing-map change. A pair with no completed-dispatch\n  history reports insufficient-data rather than a fabricated score.\n  Writes nothing: no routing-map mutation, no database write. Applying\n  a recommendation is a separate, explicit operator-gated action.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleEvals),
        },
    },
};

fn handleList(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "list" }, args_ptr);
    const ctx = runtime.current();
    const report = engine.models.discover(ctx.allocator, ctx.io) catch |e| exit.die(
        ctx,
        e,
        "model discovery failed: {s}",
        .{@errorName(e)},
    );
    try output.emit(ctx, engine.models, report, .{ .json = args.json });
}

fn handleRefresh(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "refresh" }, args_ptr);
    const ctx = runtime.current();

    const report = engine.models.discover(ctx.allocator, ctx.io) catch |e| exit.die(
        ctx,
        e,
        "model discovery failed: {s}",
        .{@errorName(e)},
    );

    const home = engine.identity.workspace.planarHome(ctx.allocator, ctx.environ) catch |e| exit.die(
        ctx,
        e,
        "resolving PLANAR_HOME: {s}",
        .{@errorName(e)},
    );
    const path = engine.models.writeCache(ctx.allocator, ctx.io, home, report) catch |e| exit.die(
        ctx,
        e,
        "writing model cache: {s}",
        .{@errorName(e)},
    );

    try output.emit(ctx, engine.models, report, .{ .json = args.json });
    // Provenance note on stderr so JSON stdout stays clean for scripts.
    try ctx.stderr.print("wrote model cache: {s}\n", .{path});
}

fn handleRouting(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "routing" }, args_ptr);
    const ctx = runtime.current();

    // Resolve the effective config (file-over-default), then build the routing.
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

    const rows = engine.models.buildRouting(ctx.allocator, &resolved.effective) catch |e|
        exit.die(ctx, e, "building model routing: {s}", .{@errorName(e)});
    defer ctx.allocator.free(rows);

    if (args.json) {
        try std.json.Stringify.value(rows, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        for (rows) |r| {
            try ctx.stdout.print(
                "  {s: <10} → {s: <6} {s: <22} ({s}) [{s}]\n",
                .{ r.role, r.vendor, r.model, r.tier, r.source.label() },
            );
        }
    }
}

fn handleApply(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "apply" }, args_ptr);
    const ctx = runtime.current();

    const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    const existing: []u8 = std.Io.Dir.cwd().readFileAlloc(ctx.io, path, ctx.allocator, .unlimited) catch |e| switch (e) {
        error.FileNotFound => try ctx.allocator.dupe(u8, ""),
        else => exit.die(ctx, e, "reading config file: {s}", .{@errorName(e)}),
    };
    defer ctx.allocator.free(existing);

    const already = std.mem.indexOf(u8, existing, "[models.") != null or std.mem.indexOf(u8, existing, "[models]") != null;
    if (already and !args.force) {
        try ctx.stdout.print("models config already present in {s} (use --force to append)\n", .{path});
        return;
    }

    const block = engine.models.renderConfigBlock(ctx.allocator) catch |e|
        exit.die(ctx, e, "rendering model config: {s}", .{@errorName(e)});
    defer ctx.allocator.free(block);

    // Append the block to existing content (with a separating newline).
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(ctx.allocator);
    out.appendSlice(ctx.allocator, existing) catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});
    if (existing.len > 0 and existing[existing.len - 1] != '\n') out.append(ctx.allocator, '\n') catch {};
    if (existing.len > 0) out.append(ctx.allocator, '\n') catch {};
    out.appendSlice(ctx.allocator, block) catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    std.Io.Dir.cwd().writeFile(ctx.io, .{ .sub_path = path, .data = out.items }) catch |e|
        exit.die(ctx, e, "writing config file: {s}", .{@errorName(e)});
    try ctx.stdout.print("wrote model routing config to {s}\n", .{path});
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

fn handleCandidates(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "candidates" }, args_ptr);
    const ctx = runtime.current();

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

    var tiers_out: std.ArrayList(TierCandidates) = .empty;
    defer tiers_out.deinit(ctx.allocator);
    for (engine.config.vendors) |v| {
        for (engine.config.tiers) |t| {
            var buf: [64]u8 = undefined;
            const key = std.fmt.bufPrint(&buf, "models.{s}.{s}", .{ v, t }) catch continue;
            const vws = resolved.effective.get(key) orelse continue;
            tiers_out.append(ctx.allocator, .{
                .vendor = v,
                .tier = t,
                .candidates = vws.candidates,
                .source = vws.source.label(),
            }) catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});
        }
    }

    var routing_out: std.ArrayList(RoutingEntry) = .empty;
    defer routing_out.deinit(ctx.allocator);
    for (engine.config.vendors) |v| {
        for (engine.config.tiers) |t| {
            for (engine.config.work_types) |wt| {
                var buf: [96]u8 = undefined;
                const key = std.fmt.bufPrint(&buf, "routing.{s}.{s}.{s}", .{ v, t, wt }) catch continue;
                const vws = resolved.effective.get(key) orelse continue;
                if (vws.value.len == 0) continue;
                routing_out.append(ctx.allocator, .{
                    .vendor = v,
                    .tier = t,
                    .work_type = wt,
                    .model = vws.value,
                    .source = vws.source.label(),
                }) catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});
            }
        }
    }

    if (args.json) {
        try ctx.stdout.print("{{\"candidates\":", .{});
        try std.json.Stringify.value(tiers_out.items, .{}, ctx.stdout);
        try ctx.stdout.print(",\"routing\":", .{});
        try std.json.Stringify.value(routing_out.items, .{}, ctx.stdout);
        try ctx.stdout.print("}}\n", .{});
        return;
    }

    try ctx.stdout.print("tier candidate lists (models.<vendor>.<tier>):\n", .{});
    for (tiers_out.items) |tc| {
        try ctx.stdout.print("  {s: <8} {s: <6} → ", .{ tc.vendor, tc.tier });
        for (tc.candidates, 0..) |c, i| {
            if (i > 0) try ctx.stdout.print(", ", .{});
            try ctx.stdout.print("{s}", .{c});
        }
        try ctx.stdout.print("  [{s}]\n", .{tc.source});
    }

    try ctx.stdout.print("\nwork-type routing map (routing.<vendor>.<tier>.<work-type>):\n", .{});
    for (routing_out.items) |re| {
        try ctx.stdout.print(
            "  {s: <8} {s: <6} {s: <14} → {s: <22} [{s}]\n",
            .{ re.vendor, re.tier, re.work_type, re.model, re.source },
        );
    }
}

/// `planar models sync-doc [--check]` (plan 918 D4, milestone M3). Reads
/// `agents/models.md` under `--out` (default cwd), regenerates its
/// `## Tier Table` section from the resolved config via
/// `engine.models.syncTierTable`, and either writes the result back
/// (atomically: write-temp then rename) or, with `--check`, only reports
/// whether the committed table already matches — the replacement for
/// skillrender's retired render-time patch.
fn handleSyncDoc(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "sync-doc" }, args_ptr);
    const ctx = runtime.current();

    const cfg_path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(cfg_path);

    const file_content: ?[]u8 = std.Io.Dir.cwd().readFileAlloc(ctx.io, cfg_path, ctx.allocator, .unlimited) catch |e| switch (e) {
        error.FileNotFound => null,
        else => exit.die(ctx, e, "reading config file: {s}", .{@errorName(e)}),
    };
    defer if (file_content) |fc| ctx.allocator.free(fc);

    var resolved = engine.config.resolve(ctx.allocator, file_content, ctx.environ, null) catch |e|
        exit.die(ctx, e, "resolving configuration: {s}", .{@errorName(e)});
    defer resolved.deinit(ctx.allocator);

    const models_path = std.fs.path.join(ctx.allocator, &.{ args.out, "agents/models.md" }) catch |e|
        exit.die(ctx, e, "{s}", .{@errorName(e)});
    defer ctx.allocator.free(models_path);

    const current = std.Io.Dir.cwd().readFileAlloc(ctx.io, models_path, ctx.allocator, .unlimited) catch |e|
        exit.die(ctx, e, "reading {s}: {s}", .{ models_path, @errorName(e) });
    defer ctx.allocator.free(current);

    const rewritten = engine.models.syncTierTable(ctx.allocator, current, &resolved.effective) catch |e| switch (e) {
        error.MissingTierTableHeading => exit.die(
            ctx,
            e,
            "{s}: no '## Tier Table' heading found — refusing to append or corrupt the file",
            .{models_path},
        ),
        else => exit.die(ctx, e, "regenerating tier table: {s}", .{@errorName(e)}),
    };
    defer ctx.allocator.free(rewritten);

    const in_sync = std.mem.eql(u8, current, rewritten);

    if (args.check) {
        if (in_sync) {
            try ctx.stdout.print("{s}: tier table is in sync\n", .{models_path});
            return;
        }
        exit.die(
            ctx,
            error.ModelsDocDrift,
            "{s}: '## Tier Table' is out of sync with the resolved config (run `planar models sync-doc` to regenerate)",
            .{models_path},
        );
    }

    if (in_sync) {
        try ctx.stdout.print("{s}: already in sync\n", .{models_path});
        return;
    }

    writeAtomicFile(ctx.io, models_path, rewritten) catch |e|
        exit.die(ctx, e, "writing {s}: {s}", .{ models_path, @errorName(e) });
    try ctx.stdout.print("wrote {s}\n", .{models_path});
}

/// Write `data` to `path` via a sibling `<path>.planar-sync-tmp` temp file
/// followed by a rename (write-temp-then-rename) so a crash mid-write never
/// leaves `agents/models.md` truncated or partially rewritten.
fn writeAtomicFile(io: std.Io, path: []const u8, data: []const u8) !void {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp = try std.fmt.bufPrint(&buf, "{s}.planar-sync-tmp", .{path});
    std.Io.Dir.cwd().deleteFile(io, tmp) catch {};
    errdefer std.Io.Dir.cwd().deleteFile(io, tmp) catch {};
    try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = tmp, .data = data });
    try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), path, io);
}

fn handleEvals(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "evals" }, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e|
        exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});

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

    var result = engine.evals.aggregate(d, ctx.allocator, &resolved.effective) catch |e|
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
