//! handlers/spec/ingest — `planar spec ingest <plan> [--apply] [--apply-removals] [--strict] [--json] [--format text|json] [--scope <slug>]`
//!
//! Mirrors Go's `runSpecIngestOne` (`src/cmd/planar/internal/planning/spec_ingest.go`).
//! Default mode is PREVIEW: read tech-spec + roadmap + (optional) test-spec
//! from the anchor plan's workbench feature directory, parse them, compute
//! the diff against the current DB, and render it. `--apply` commits the
//! Diff; `--strict` refuses (exit 1) when the diff has uncovered task slugs
//! or orphan scenarios so the test-coder gate has bite.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const db = @import("db");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");

const ingestor = engine.ingestor;
const wb_feature = engine.workbench.feature;
const wb_parse = engine.workbench.parse;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "spec", "ingest" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // --apply-removals without --apply is a user error.
    if (args.apply_removals and !args.apply) {
        exit.die(ctx, error.InvalidInput, "--apply-removals requires --apply; use both flags together to commit removals", .{});
    }

    // Format selection: --json overrides --format=text for parity with
    // the Go CLI's convenience flag.
    const json_out = args.json or std.mem.eql(u8, args.format, "json");

    // Batch: first positional + any extras. Multi-arg JSON mode emits an
    // array; single-arg JSON mode emits a single object (Go parity).
    const multi = args.extra_plans.len > 0;
    const total: usize = 1 + args.extra_plans.len;

    // For multi-arg JSON we collect rendered diff bodies and emit a
    // bracketed array at the end. Each entry is the per-plan JSON the
    // renderer would have emitted standalone.
    var json_bodies: std.ArrayList([]const u8) = .empty;
    defer {
        for (json_bodies.items) |b| ctx.allocator.free(b);
        json_bodies.deinit(ctx.allocator);
    }

    var any_err = false;
    var idx: usize = 0;
    while (idx < total) : (idx += 1) {
        const plan_arg = if (idx == 0) args.plan else args.extra_plans[idx - 1];
        runOnePlan(
            ctx,
            d,
            plan_arg,
            args.apply,
            args.apply_removals,
            args.scope,
            args.strict,
            json_out,
            multi,
            &json_bodies,
        ) catch {
            any_err = true;
            continue;
        };
    }

    // Multi-arg JSON: emit one wrapping array. Single-arg JSON already
    // emitted in runOnePlan.
    if (json_out and multi) {
        try ctx.stdout.print("[", .{});
        for (json_bodies.items, 0..) |b, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print("{s}", .{b});
        }
        try ctx.stdout.print("]\n", .{});
    }

    if (any_err) exit.die(ctx, error.InvalidInput, "one or more plans failed to ingest", .{});
}

/// runOnePlan handles a single plan argument end-to-end: anchor lookup,
/// cross-scope guard, artifact read, parse, diff, render, strict gate,
/// apply. JSON output mode behavior depends on `multi`:
///   - !multi: render the JSON body directly to stdout.
///   - multi: append the JSON body to `json_bodies` so the caller can wrap
///     it in an array.
fn runOnePlan(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    plan_arg: []const u8,
    apply_flag: bool,
    apply_removals: bool,
    scope_flag: ?[]const u8,
    strict: bool,
    json_out: bool,
    multi: bool,
    json_bodies: *std.ArrayList([]const u8),
) !void {
    // ---- resolve anchor plan ----------------------------------------
    const anchor = fetchAnchor(d, ctx.allocator, plan_arg) catch |e| {
        try ctx.stderr.print("plan '{s}' not found: {s}\n", .{ plan_arg, @errorName(e) });
        return e;
    };
    defer freeAnchor(ctx.allocator, anchor);

    // ---- cross-scope guard ------------------------------------------
    // Operator's write scope: explicit --scope wins; otherwise default to
    // the anchor's own assoc_slug (or null for a global anchor). This
    // matches Go's runSpecIngestOne, which loads the plan's stored scope
    // and refuses when the operator's resolved scope disagrees.
    const op_scope: ?[]const u8 = blk: {
        if (scope_flag) |s| break :blk if (s.len == 0) null else s;
        break :blk if (anchor.assoc_slug.len == 0) null else anchor.assoc_slug;
    };
    const entity_scope: ?[]const u8 = if (anchor.assoc_slug.len == 0) null else anchor.assoc_slug;
    scope_mod.guard(entity_scope, op_scope) catch |e| switch (e) {
        error.ScopeMismatch => {
            try ctx.stderr.print(
                "plan {d} belongs to {s} but the resolved write scope is {s}. " ++
                    "Refusing cross-scope write; pass --scope {s} or cd into the right repo.\n",
                .{
                    anchor.id,
                    if (entity_scope) |s| s else "global",
                    if (op_scope) |s| s else "global",
                    if (entity_scope) |s| s else "global",
                },
            );
            return e;
        },
    };

    // ---- locate workbench feature dir -------------------------------
    const wb_root = resolveWorkbenchRoot(ctx.allocator, ctx.environ) catch |e| {
        try ctx.stderr.print("resolving workbench root: {s}\n", .{@errorName(e)});
        return e;
    };
    defer ctx.allocator.free(wb_root);

    const plan_key = try planKey(d, ctx.allocator, anchor.id);
    defer ctx.allocator.free(plan_key);

    const feature_dir = try wb_feature.featureDir(ctx.allocator, wb_root, anchor.assoc_slug, plan_key, anchor.slug);
    defer ctx.allocator.free(feature_dir);

    // ---- read artifacts ---------------------------------------------
    const tech_body = readArtifactBody(d, ctx.allocator, feature_dir, anchor.id, "tech_spec") catch |e| {
        try ctx.stderr.print("plan {d} ({s}): reading tech_spec artifact: {s}\n", .{ anchor.id, anchor.slug, @errorName(e) });
        return e;
    };
    defer ctx.allocator.free(tech_body);
    const roadmap_body = readArtifactBody(d, ctx.allocator, feature_dir, anchor.id, "roadmap") catch |e| {
        try ctx.stderr.print("plan {d} ({s}): reading roadmap artifact: {s}\n", .{ anchor.id, anchor.slug, @errorName(e) });
        return e;
    };
    defer ctx.allocator.free(roadmap_body);

    // test-spec is optional.
    const test_body_opt: ?[]const u8 = readArtifactBody(d, ctx.allocator, feature_dir, anchor.id, "test_spec") catch null;
    defer if (test_body_opt) |b| ctx.allocator.free(b);

    // ---- parse + diff ------------------------------------------------
    const decisions = try ingestor.parse.parseTechSpecDecisions(ctx.allocator, tech_body);
    defer ingestor.parse.deinitDecisions(decisions, ctx.allocator);
    const questions = try ingestor.parse.parseTechSpecOpenQuestions(ctx.allocator, tech_body);
    defer ingestor.parse.deinitQuestions(questions, ctx.allocator);
    const milestones = try ingestor.parse.parseRoadmap(ctx.allocator, roadmap_body);
    defer ingestor.parse.deinitMilestones(milestones, ctx.allocator);

    const scenarios: []const ingestor.parse.Scenario = if (test_body_opt) |tb|
        try ingestor.parse.parseTestSpec(ctx.allocator, tb)
    else
        &.{};
    defer if (test_body_opt != null) ingestor.parse.deinitScenarios(scenarios, ctx.allocator);

    var diff = ingestor.diff.compute(d, ctx.allocator, anchor.id, milestones, decisions, questions, scenarios) catch |e| {
        try ctx.stderr.print("plan {d} ({s}): computing diff: {s}\n", .{ anchor.id, anchor.slug, @errorName(e) });
        return e;
    };
    defer ingestor.diff.deinitDiff(diff, ctx.allocator);

    // ---- register design references (P3a) ---------------------------
    // Glob `<feature_dir>/design/*.html`; each becomes a `design_note`
    // artifact linked to the plan's UI tasks at apply time. A missing
    // `design/` dir (the common case) yields no design notes.
    diff.design_notes = collectDesignNotes(ctx.allocator, ctx.io, feature_dir) catch |e| {
        try ctx.stderr.print("plan {d} ({s}): scanning design dir: {s}\n", .{ anchor.id, anchor.slug, @errorName(e) });
        return e;
    };

    // ---- render preview ---------------------------------------------
    if (json_out) {
        if (multi) {
            // Buffer the per-plan JSON so the caller can wrap it in an
            // array. Capture stdout-equivalent bytes via Allocating writer
            // and dupe the written slice (Allocating owns its buffer until
            // deinit; we hand the caller a separately-owned copy).
            var w: std.Io.Writer.Allocating = .init(ctx.allocator);
            defer w.deinit();
            try ingestor.render.renderJson(ctx.allocator, &w.writer, diff);
            const body = try ctx.allocator.dupe(u8, w.written());
            try json_bodies.append(ctx.allocator, body);
        } else {
            try ingestor.render.renderJson(ctx.allocator, ctx.stdout, diff);
        }
    } else {
        if (multi) {
            try ctx.stdout.print("=== plan {d}: {s} ===\n", .{ anchor.id, anchor.slug });
        }
        try ingestor.render.renderText(ctx.allocator, ctx.stdout, diff, apply_flag);
    }

    // ---- strict gate (after rendering so operator sees gaps) --------
    if (strict) {
        const cov = try ingestor.coverage.compute(ctx.allocator, diff);
        defer ingestor.coverage.deinitCoverage(cov, ctx.allocator);
        if (cov.hasGaps()) {
            try ctx.stderr.print("plan {d}: --strict refused: ", .{anchor.id});
            if (cov.uncovered_task_slugs.len > 0) {
                try ctx.stderr.print("{d} uncovered task slug(s): ", .{cov.uncovered_task_slugs.len});
                for (cov.uncovered_task_slugs, 0..) |s, i| {
                    if (i > 0) try ctx.stderr.print(", ", .{});
                    try ctx.stderr.print("{s}", .{s});
                }
            }
            if (cov.orphan_scenarios.len > 0) {
                if (cov.uncovered_task_slugs.len > 0) try ctx.stderr.print("; ", .{});
                try ctx.stderr.print("{d} orphan scenario(s): ", .{cov.orphan_scenarios.len});
                for (cov.orphan_scenarios, 0..) |s, i| {
                    if (i > 0) try ctx.stderr.print("; ", .{});
                    try ctx.stderr.print("{s}", .{s});
                }
            }
            try ctx.stderr.print("\n", .{});
            return error.InvalidInput;
        }
    }

    // ---- apply ------------------------------------------------------
    const apply_opts: ingestor.apply.Options = .{
        .apply = apply_flag,
        .apply_removals = apply_removals,
        .scope = op_scope,
    };
    const result = ingestor.apply.apply(d, ctx.allocator, diff, apply_opts) catch |e| {
        try ctx.stderr.print("plan {d} ({s}): apply failed: {s}\n", .{ anchor.id, anchor.slug, @errorName(e) });
        return e;
    };

    if (apply_flag and !json_out) {
        try ctx.stderr.print(
            "plan {d} ({s}) applied: {d} plans created, {d} tasks created, {d} tasks updated, {d} decisions added, {d} questions added, {d} questions answered",
            .{
                anchor.id,
                anchor.slug,
                result.plans_created,
                result.tasks_created,
                result.tasks_updated,
                result.decisions_added,
                result.questions_added,
                result.questions_answered,
            },
        );
        if (result.tasks_cancelled > 0) try ctx.stderr.print(", {d} tasks cancelled", .{result.tasks_cancelled});
        if (result.anchor_activated) try ctx.stderr.print(" (anchor plan activated)", .{});
        try ctx.stderr.print("\n", .{});
    }
}

// =========================================================================
// Anchor lookup + workbench artifact helpers
// =========================================================================

const Anchor = struct {
    id: i64,
    slug: []const u8,
    assoc_slug: []const u8,
    status: []const u8,
};

fn freeAnchor(allocator: std.mem.Allocator, a: Anchor) void {
    allocator.free(a.slug);
    allocator.free(a.assoc_slug);
    allocator.free(a.status);
}

/// fetchAnchor resolves an anchor plan from either a numeric id or a
/// top-level plan slug. Mirrors Go's `workbench.FetchAnchorBySlugOrID`.
fn fetchAnchor(d: *db.sqlite.Db, allocator: std.mem.Allocator, arg: []const u8) !Anchor {
    // Try numeric first.
    if (std.fmt.parseInt(i64, arg, 10)) |id| {
        return try fetchAnchorById(d, allocator, id);
    } else |_| {}
    // Slug lookup.
    var stmt = try d.prepare(
        \\select p.id, p.slug, coalesce(a.slug, ''), p.status
        \\from plans p
        \\left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id)
        \\where p.parent_plan_id is null and p.slug = ?
        \\order by p.id limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .text = arg }});
    switch (try stmt.step()) {
        .done => return error.NotFound,
        .row => return .{
            .id = stmt.columnInt(0),
            .slug = try stmt.columnTextAlloc(1, allocator),
            .assoc_slug = try stmt.columnTextAlloc(2, allocator),
            .status = try stmt.columnTextAlloc(3, allocator),
        },
    }
}

fn fetchAnchorById(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) !Anchor {
    var stmt = try d.prepare(
        \\select p.id, p.slug, coalesce(a.slug, ''), p.status
        \\from plans p
        \\left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id)
        \\where p.id = ? and p.parent_plan_id is null
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = id }});
    switch (try stmt.step()) {
        .done => return error.NotFound,
        .row => return .{
            .id = stmt.columnInt(0),
            .slug = try stmt.columnTextAlloc(1, allocator),
            .assoc_slug = try stmt.columnTextAlloc(2, allocator),
            .status = try stmt.columnTextAlloc(3, allocator),
        },
    }
}

/// planKey returns the workbench dir's "<plan-key>" component:
/// `<external-id>` when an external_link exists, otherwise `p<id>`.
fn planKey(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64) ![]const u8 {
    var stmt = try d.prepare(
        \\select external_id from external_links
        \\where entity_kind = 'plan' and entity_id = ?
        \\limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = anchor_id }});
    switch (try stmt.step()) {
        .done => return try std.fmt.allocPrint(allocator, "p{d}", .{anchor_id}),
        .row => {
            const ext = try stmt.columnTextAlloc(0, allocator);
            if (ext.len == 0) {
                allocator.free(ext);
                return try std.fmt.allocPrint(allocator, "p{d}", .{anchor_id});
            }
            return ext;
        },
    }
}

/// readArtifactBody reads the body of the lowest-id artifact of the given
/// kind linked to anchor_id, strips its front matter, and extracts the
/// `## Content` section. Mirrors Go's `workbench.ReadArtifactBodyByKind`.
fn readArtifactBody(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    feature_dir: []const u8,
    anchor_id: i64,
    kind: []const u8,
) ![]const u8 {
    // Resolve artifact row.
    var stmt = try d.prepare(
        \\select a.id, a.title from artifacts a
        \\join entity_links el
        \\  on el.from_kind = 'artifact' and el.from_id = a.id
        \\ and el.to_kind   = 'plan'     and el.to_id   = ?
        \\ and el.relationship = 'derives-from'
        \\where a.kind = ?
        \\order by a.id limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .int = anchor_id }, .{ .text = kind } });
    var id: i64 = 0;
    var title_buf: []const u8 = &.{};
    switch (try stmt.step()) {
        .done => return error.ArtifactNotFound,
        .row => {
            id = stmt.columnInt(0);
            title_buf = try stmt.columnTextAlloc(1, allocator);
        },
    }
    defer allocator.free(title_buf);

    const filename = try wb_feature.artifactFilename(allocator, id, title_buf, kind);
    defer allocator.free(filename);

    // Plan 314 task 2319: zig and Go now agree — artifacts live at the
    // feature-dir root, never under `artifacts/`. The previous
    // subdir-then-root fallback collapsed once `engine.workbench.sync`
    // was updated to write at root.
    const path = try std.fs.path.join(allocator, &.{ feature_dir, filename });
    defer allocator.free(path);

    // Read the file. We use posix c.open/c.read here because the
    // M9 cycle predates a high-level wrapper for file IO in the zig
    // engine — the workbench's other readers do the same (see
    // engine/workbench/sync.zig). When a unified file-IO helper lands
    // this block collapses to a one-liner.
    const path_z = try allocator.dupeZ(u8, path);
    defer allocator.free(path_z);
    const fd = std.c.open(path_z.ptr, .{ .ACCMODE = .RDONLY });
    if (fd < 0) return error.ArtifactReadFailed;
    defer _ = std.c.close(fd);
    var buf: std.ArrayList(u8) = .empty;
    errdefer buf.deinit(allocator);
    var chunk: [4096]u8 = undefined;
    while (true) {
        const n = std.c.read(fd, &chunk, chunk.len);
        if (n <= 0) break;
        try buf.appendSlice(allocator, chunk[0..@intCast(n)]);
    }
    const raw = try buf.toOwnedSlice(allocator);
    errdefer allocator.free(raw);

    // Strip front matter; fall back to raw on parse failure. Use errdefer
    // so an OOM inside extractContentSection also frees raw and the
    // ParseResult — the previous explicit `wb_parse.deinit(pr, ...)`
    // immediately before `extractContentSection` could leak `pr` if the
    // extraction OOMed before we reached the deinit line.
    const pr = wb_parse.parse(allocator, raw) catch {
        return raw; // raw is owned by caller on the fallback path.
    };
    defer wb_parse.deinit(pr, allocator);
    defer allocator.free(raw);
    return try extractContentSection(allocator, pr.body);
}

fn extractContentSection(allocator: std.mem.Allocator, body: []const u8) ![]const u8 {
    const marker = "\n## Content\n";
    if (std.mem.indexOf(u8, body, marker)) |idx| {
        var section = body[idx + marker.len ..];
        if (section.len > 0 and section[0] == '\n') section = section[1..];
        return try allocator.dupe(u8, section);
    }
    return try allocator.dupe(u8, body);
}

/// resolveWorkbenchRoot honors `$PLANAR_WORKBENCH_ROOT`, falling back to
/// `$HOME/.planar/workbench` and ultimately CWD. Mirrors Go's
/// `workbench.Root` resolution.
fn resolveWorkbenchRoot(allocator: std.mem.Allocator, environ: std.process.Environ) ![]const u8 {
    if (environ.getPosix("PLANAR_WORKBENCH_ROOT")) |raw| return try allocator.dupe(u8, raw);
    if (environ.getPosix("HOME")) |home| return try std.fs.path.join(allocator, &.{ home, ".planar", "workbench" });
    return try allocator.dupe(u8, ".");
}

/// collectDesignNotes globs `<feature_dir>/design/*.html` and returns one
/// `DesignNoteEntry` per file with a workbench-relative `source_path`
/// (`design/<file>`) — the reconcile key apply.zig registers on. A missing
/// or unreadable `design/` dir (the common, non-UI case) yields an empty
/// slice, never an error. The returned slice + its strings are owned by the
/// caller (freed via `deinitDiff`).
fn collectDesignNotes(
    allocator: std.mem.Allocator,
    io: std.Io,
    feature_dir: []const u8,
) ![]const ingestor.diff.DesignNoteEntry {
    var out: std.ArrayList(ingestor.diff.DesignNoteEntry) = .empty;
    errdefer {
        for (out.items) |dn| {
            allocator.free(dn.source_path);
            allocator.free(dn.title);
        }
        out.deinit(allocator);
    }

    const design_dir = try std.fs.path.join(allocator, &.{ feature_dir, "design" });
    defer allocator.free(design_dir);

    var dir = std.Io.Dir.cwd().openDir(io, design_dir, .{ .iterate = true }) catch {
        return try out.toOwnedSlice(allocator);
    };
    defer dir.close(io);

    var it = dir.iterate();
    while (try it.next(io)) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".html")) continue;
        const source_path = try std.fmt.allocPrint(allocator, "design/{s}", .{entry.name});
        errdefer allocator.free(source_path);
        const title = try allocator.dupe(u8, entry.name);
        try out.append(allocator, .{ .source_path = source_path, .title = title });
    }
    return try out.toOwnedSlice(allocator);
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "extractContentSection: returns body unchanged when marker absent" {
    const a = testing.allocator;
    const out = try extractContentSection(a, "# Hello\n\nworld\n");
    defer a.free(out);
    try testing.expectEqualStrings("# Hello\n\nworld\n", out);
}

test "extractContentSection: extracts section after marker" {
    const a = testing.allocator;
    const out = try extractContentSection(a, "# Title\n\n## Content\n\nthe body\n");
    defer a.free(out);
    try testing.expectEqualStrings("the body\n", out);
}
