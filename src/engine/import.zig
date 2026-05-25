//! engine/import — M18 pl-import parity: staged request, validated cache, apply.

const std = @import("std");
const db = @import("db");
const llm = @import("llm.zig");
const planning = @import("planning.zig");

pub const schema_version: i64 = 1;
pub const min_forward_specs: usize = 3;
pub const max_forward_specs: usize = 5;
pub const deferred_priority_floor: i64 = 150;

pub const Opts = struct {
    repo_root: []const u8,
    interpret: bool = false,
    no_interpret: bool = false,
    apply: bool = false,
    apply_removals: bool = false,
    provider_override: ?[]const u8 = null,
    scope: ?[]const u8 = null,
};

pub const Mode = enum { skipped, pending, cache_hit };

pub const ApplyReport = struct {
    anchor_plan_id: i64,
    plans_created: usize,
    plans_updated: usize,
    plans_abandoned: usize,
    tasks_created: usize,
    tasks_updated: usize,
    tasks_cancelled: usize,
    artifacts_created: usize,
    artifacts_retired: usize,
    decisions_created: usize,
    decisions_superseded: usize,
};

pub const Outcome = struct {
    mode: Mode,
    provider: llm.provider.Kind,
    repo_slug: []const u8,
    fingerprint: []const u8,
    cache_path: ?[]const u8 = null,
    pending_path: ?[]const u8 = null,
    docs_count: usize = 0,
    guide_files_count: usize = 0,
    tree_entry_count: usize = 0,
    message: []const u8,
    applied: ?ApplyReport = null,
};

pub fn deinitOutcome(out: Outcome, allocator: std.mem.Allocator) void {
    allocator.free(out.repo_slug);
    allocator.free(out.fingerprint);
    if (out.cache_path) |v| allocator.free(v);
    if (out.pending_path) |v| allocator.free(v);
    allocator.free(out.message);
}

pub fn run(
    d_opt: ?*db.sqlite.Db,
    allocator: std.mem.Allocator,
    environ: std.process.Environ,
    planar_home: []const u8,
    opts: Opts,
) !Outcome {
    if (opts.apply_removals and !opts.apply) return error.InvalidInput;
    if (opts.interpret and opts.no_interpret) return error.InvalidInput;
    if (opts.repo_root.len == 0) return error.InvalidInput;
    if (!isDir(opts.repo_root)) return error.NotFound;

    const abs_root = try std.Io.Dir.realPathFileAlloc(.cwd(), fsIo(), opts.repo_root, allocator);
    defer allocator.free(abs_root);
    const provider_kind = try llm.provider.resolve(opts.provider_override, environ);
    const repo_slug = try deriveRepoSlug(allocator, abs_root);
    errdefer allocator.free(repo_slug);

    if (!opts.interpret or opts.no_interpret) {
        const req = try buildRequest(allocator, abs_root, repo_slug, provider_kind);
        defer deinitRequest(req, allocator);
        var out: Outcome = .{
            .mode = .skipped,
            .provider = provider_kind,
            .repo_slug = try allocator.dupe(u8, req.repo_slug),
            .fingerprint = try allocator.dupe(u8, req.fingerprint),
            .docs_count = req.docs.len,
            .guide_files_count = req.guide_files.len,
            .tree_entry_count = req.tree_summary.len,
            .message = try allocator.dupe(u8, "pl-import: interpretation disabled; run with --interpret to stage/consume LLM artifacts."),
        };
        if (opts.apply) {
            const d = d_opt orelse return error.InvalidInput;
            const transcribed = try makeDeterministicTranscription(allocator, req);
            out.applied = try applyInterpretation(d, allocator, req, transcribed, opts.scope, opts.apply_removals);
            allocator.free(out.message);
            out.message = try std.fmt.allocPrint(
                allocator,
                "applied: anchor {d}; {d} plans created/{d} updated, {d} tasks created/{d} updated/{d} cancelled",
                .{
                    out.applied.?.anchor_plan_id,
                    out.applied.?.plans_created,
                    out.applied.?.plans_updated,
                    out.applied.?.tasks_created,
                    out.applied.?.tasks_updated,
                    out.applied.?.tasks_cancelled,
                },
            );
        }
        return out;
    }

    const req = try buildRequest(allocator, abs_root, repo_slug, provider_kind);
    defer deinitRequest(req, allocator);

    const cache_path = try llm.client.cachePath(allocator, planar_home, .import_interpretation, req.repo_slug, req.fingerprint);
    errdefer allocator.free(cache_path);
    const pending_path = try llm.client.pendingPath(allocator, planar_home, .import_interpretation, req.repo_slug);
    errdefer allocator.free(pending_path);

    const cached_opt = try llm.client.readIfExists(cache_path, allocator, 16 * 1024 * 1024);
    if (cached_opt) |cached| {
        defer allocator.free(cached);
        var arena = std.heap.ArenaAllocator.init(allocator);
        defer arena.deinit();
        const result = try parseInterpretationResult(arena.allocator(), cached);
        try validateInterpretationResult(result, req);
        var out: Outcome = .{
            .mode = .cache_hit,
            .provider = provider_kind,
            .repo_slug = try allocator.dupe(u8, req.repo_slug),
            .fingerprint = try allocator.dupe(u8, req.fingerprint),
            .cache_path = cache_path,
            .pending_path = pending_path,
            .docs_count = req.docs.len,
            .guide_files_count = req.guide_files.len,
            .tree_entry_count = req.tree_summary.len,
            .message = try std.fmt.allocPrint(allocator, "Loaded cached interpretation result: {s}", .{cache_path}),
        };
        if (opts.apply) {
            const d = d_opt orelse return error.InvalidInput;
            out.applied = try applyInterpretation(d, allocator, req, result, opts.scope, opts.apply_removals);
            allocator.free(out.message);
            out.message = try std.fmt.allocPrint(
                allocator,
                "applied: anchor {d}; {d} plans created/{d} updated, {d} tasks created/{d} updated/{d} cancelled",
                .{
                    out.applied.?.anchor_plan_id,
                    out.applied.?.plans_created,
                    out.applied.?.plans_updated,
                    out.applied.?.tasks_created,
                    out.applied.?.tasks_updated,
                    out.applied.?.tasks_cancelled,
                },
            );
        }
        return out;
    }

    if (opts.apply) return error.NotFound;
    const request_json = try encodeRequestJSON(allocator, req);
    defer allocator.free(request_json);
    try llm.client.writeJsonAtomic(pending_path, request_json, allocator);
    return .{
        .mode = .pending,
        .provider = provider_kind,
        .repo_slug = try allocator.dupe(u8, req.repo_slug),
        .fingerprint = try allocator.dupe(u8, req.fingerprint),
        .cache_path = cache_path,
        .pending_path = pending_path,
        .docs_count = req.docs.len,
        .guide_files_count = req.guide_files.len,
        .tree_entry_count = req.tree_summary.len,
        .message = try awaitingMessage(allocator, pending_path, cache_path),
    };
}

// -----------------------------------------------------------------------------
// Request/result models
// -----------------------------------------------------------------------------

pub const DocEntry = struct { path: []const u8, body: []const u8 };

pub const GitCommit = struct {
    sha: []const u8 = "",
    title: []const u8 = "",
    body: []const u8 = "",
    date: []const u8 = "",
    author: []const u8 = "",
    tags: []const []const u8 = &.{},
};

pub const DetectedArtifact = struct { path: []const u8, kind: []const u8, source: []const u8 };

pub const Request = struct {
    schema_version: i64,
    repo_slug: []const u8,
    repo_root: []const u8,
    fingerprint: []const u8,
    provider: []const u8,
    readme: []const u8,
    docs: []const DocEntry,
    guide_files: []const DocEntry,
    tree_summary: []const []const u8,
    git_log: []const GitCommit,
    detected_artifacts: []const DetectedArtifact,
    code_evidence: llm.evidence.EvidenceMap,
};

pub const Citation = struct { path: []const u8 = "", section: []const u8 = "", line: i64 = 0 };
pub const LlmTask = struct {
    slug: []const u8 = "",
    title: []const u8 = "",
    body: []const u8 = "",
    status: []const u8 = "",
    priority: i64 = 0,
    next_action: []const u8 = "",
    citations: []const Citation = &.{},
};
pub const LlmPhase = struct {
    slug: []const u8 = "",
    title: []const u8 = "",
    summary: []const u8 = "",
    status: []const u8 = "",
    tasks: []const LlmTask = &.{},
};
pub const LlmDecision = struct {
    slug: []const u8 = "",
    title: []const u8 = "",
    body: []const u8 = "",
    rationale: []const u8 = "",
    source: []const u8 = "",
    citation: Citation = .{},
};
pub const DeferredItem = struct {
    slug: []const u8 = "",
    title: []const u8 = "",
    body: []const u8 = "",
    phase_slug: []const u8 = "",
    priority: i64 = 0,
    origin_pattern: []const u8 = "",
};
pub const ForwardSpec = struct { slug: []const u8 = "", title: []const u8 = "", goal: []const u8 = "", summary: []const u8 = "" };
pub const InterpretationResult = struct {
    schema_version: i64 = 0,
    fingerprint: []const u8 = "",
    anchor_title: []const u8 = "",
    phases: []const LlmPhase = &.{},
    decisions: []const LlmDecision = &.{},
    deferred_items: []const DeferredItem = &.{},
    forward_specs: []const ForwardSpec = &.{},
    provenance: []const u8 = "",
    generated_at: []const u8 = "",
};

fn parseInterpretationResult(arena: std.mem.Allocator, raw: []const u8) !InterpretationResult {
    const parsed = try std.json.parseFromSlice(InterpretationResult, arena, raw, .{
        .ignore_unknown_fields = true,
    });
    return parsed.value;
}

fn validateInterpretationResult(r: InterpretationResult, req: Request) !void {
    if (r.schema_version != schema_version) return error.InvalidInput;
    if (!std.mem.eql(u8, r.fingerprint, req.fingerprint)) return error.InvalidInput;
    if (r.anchor_title.len == 0 or r.provenance.len == 0) return error.InvalidInput;
    if (r.forward_specs.len < min_forward_specs or r.forward_specs.len > max_forward_specs) return error.InvalidInput;

    var phase_seen = std.StringHashMap(void).init(std.heap.page_allocator);
    defer phase_seen.deinit();
    var phase_set = std.StringHashMap(void).init(std.heap.page_allocator);
    defer phase_set.deinit();
    for (r.phases) |p| {
        if (!isValidPlanStatus(p.status)) return error.InvalidInput;
        if (phase_seen.contains(p.slug)) return error.InvalidInput;
        try phase_seen.put(p.slug, {});
        try phase_set.put(p.slug, {});
        var task_seen = std.StringHashMap(void).init(std.heap.page_allocator);
        defer task_seen.deinit();
        var doing_count: usize = 0;
        for (p.tasks) |t| {
            if (!isValidTaskStatus(t.status)) return error.InvalidInput;
            if (task_seen.contains(t.slug)) return error.InvalidInput;
            try task_seen.put(t.slug, {});
            if (std.mem.eql(u8, t.status, "doing")) doing_count += 1;
            if (t.priority < 0 or t.priority > 1000) return error.InvalidInput;
            for (t.citations) |c| try validateDocPath(req.repo_root, c.path);
        }
        if (doing_count > 1) return error.InvalidInput;
    }
    for (r.decisions) |d| {
        if (!std.mem.eql(u8, d.source, "tech-spec") and !std.mem.eql(u8, d.source, "llm-inferred")) return error.InvalidInput;
        if (std.mem.eql(u8, d.source, "llm-inferred") and d.citation.path.len == 0) return error.InvalidInput;
        if (d.citation.path.len > 0) try validateDocPath(req.repo_root, d.citation.path);
    }
    for (r.deferred_items) |di| {
        if (di.priority < deferred_priority_floor) return error.InvalidInput;
        if (!phase_set.contains(di.phase_slug)) return error.InvalidInput;
    }
    var fs_seen = std.StringHashMap(void).init(std.heap.page_allocator);
    defer fs_seen.deinit();
    for (r.forward_specs) |fs| {
        if (fs.slug.len == 0) return error.InvalidInput;
        if (fs_seen.contains(fs.slug)) return error.InvalidInput;
        try fs_seen.put(fs.slug, {});
    }
}

fn validateDocPath(repo_root: []const u8, rel_path: []const u8) !void {
    if (rel_path.len == 0) return;
    const full = try std.fs.path.join(std.heap.page_allocator, &.{ repo_root, rel_path });
    defer std.heap.page_allocator.free(full);
    std.Io.Dir.cwd().access(fsIo(), full, .{}) catch return error.InvalidInput;
}

// -----------------------------------------------------------------------------
// Apply (M18 import parity slice)
// -----------------------------------------------------------------------------

fn applyInterpretation(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    req: Request,
    result: InterpretationResult,
    scope: ?[]const u8,
    apply_removals: bool,
) !ApplyReport {
    var report: ApplyReport = .{
        .anchor_plan_id = 0,
        .plans_created = 0,
        .plans_updated = 0,
        .plans_abandoned = 0,
        .tasks_created = 0,
        .tasks_updated = 0,
        .tasks_cancelled = 0,
        .artifacts_created = 0,
        .artifacts_retired = 0,
        .decisions_created = 0,
        .decisions_superseded = 0,
    };

    const anchor_slug = try slugify(allocator, result.anchor_title);
    defer allocator.free(anchor_slug);
    const anchor_id = try upsertPlan(d, allocator, result.anchor_title, anchor_slug, "draft", null, scope, &report);
    report.anchor_plan_id = anchor_id;

    var keep_plan_ids = std.ArrayList(i64).empty;
    defer keep_plan_ids.deinit(allocator);
    try keep_plan_ids.append(allocator, anchor_id);

    for (result.phases) |p| {
        const pid = try upsertPlan(d, allocator, p.title, p.slug, p.status, anchor_id, scope, &report);
        try keep_plan_ids.append(allocator, pid);
        var keep_task_ids = std.ArrayList(i64).empty;
        defer keep_task_ids.deinit(allocator);
        for (p.tasks) |t| {
            const tid = try upsertTask(d, allocator, t, pid, scope, &report);
            try keep_task_ids.append(allocator, tid);
        }
        if (apply_removals) {
            report.tasks_cancelled += try cancelMissingTasks(d, allocator, pid, keep_task_ids.items);
        }
    }

    if (apply_removals) {
        report.plans_abandoned += try abandonMissingChildPlans(d, allocator, anchor_id, keep_plan_ids.items);
    }

    for (req.docs) |doc| {
        const kind = classifyArtifactKind(doc.path);
        const title = std.fs.path.basename(doc.path);
        const aid = try createArtifactIfMissing(d, allocator, title, kind, doc.body, doc.path, anchor_id, scope);
        if (aid > 0) report.artifacts_created += 1;
    }
    if (apply_removals) {
        report.artifacts_retired += try retireMissingArtifacts(d, allocator, anchor_id, req.docs);
    }

    var keep_decision_ids = std.ArrayList(i64).empty;
    defer keep_decision_ids.deinit(allocator);
    for (result.decisions) |dec| {
        const upserted = try createDecisionIfMissing(d, allocator, dec, anchor_id, scope);
        if (upserted.created) report.decisions_created += 1;
        try keep_decision_ids.append(allocator, upserted.id);
    }
    if (apply_removals) {
        report.decisions_superseded += try supersedeMissingDecisions(d, allocator, anchor_id, keep_decision_ids.items);
    }
    return report;
}

fn upsertPlan(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    title: []const u8,
    slug: []const u8,
    status: []const u8,
    parent_id: ?i64,
    scope: ?[]const u8,
    report: *ApplyReport,
) !i64 {
    if (try findPlanBySlugAndParent(d, slug, parent_id)) |id| {
        _ = d.execParams(
            \\update plans set title=?, status=?, updated_at=datetime('now')
            \\where id=?
        , &.{ .{ .text = title }, .{ .text = status }, .{ .int = id } }) catch return error.QueryFailed;
        report.plans_updated += 1;
        return id;
    }
    const created = try planning.plan.create(d, allocator, .{
        .title = title,
        .slug = slug,
        .status = statusFromText(status),
        .parent_plan_id = parent_id,
        .scope = scope,
    });
    defer planning.plan.deinit(created, allocator);
    report.plans_created += 1;
    return created.id;
}

fn upsertTask(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    t: LlmTask,
    plan_id: i64,
    scope: ?[]const u8,
    report: *ApplyReport,
) !i64 {
    if (try findTaskBySlugAndPlan(d, t.slug, plan_id)) |id| {
        _ = d.execParams(
            \\update tasks set title=?, body=?, status=?, priority=?, next_action=?, updated_at=datetime('now')
            \\where id=?
        , &.{
            .{ .text = t.title },
            .{ .text = t.body },
            .{ .text = t.status },
            .{ .int = t.priority },
            if (t.next_action.len > 0) .{ .text = t.next_action } else .{ .null = {} },
            .{ .int = id },
        }) catch return error.QueryFailed;
        report.tasks_updated += 1;
        return id;
    }
    const created = try planning.task.create(d, allocator, .{
        .title = t.title,
        .body = if (t.body.len > 0) t.body else null,
        .status = taskStatusFromText(t.status),
        .priority = t.priority,
        .plan_id = plan_id,
        .next_action = if (t.next_action.len > 0) t.next_action else null,
        .slug = if (t.slug.len > 0) t.slug else null,
        .scope = scope,
    });
    defer planning.task.deinit(created, allocator);
    report.tasks_created += 1;
    return created.id;
}

fn createArtifactIfMissing(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    title: []const u8,
    kind: []const u8,
    body: []const u8,
    source_path: []const u8,
    plan_id: i64,
    scope: ?[]const u8,
) !i64 {
    if (try findArtifactBySourcePathAndPlan(d, source_path, plan_id)) |_| return 0;
    const created = try planning.artifact.create(d, allocator, .{
        .title = title,
        .kind = artifactKindFromText(kind),
        .body = body,
        .source_path = source_path,
        .plan_id = plan_id,
        .scope = scope,
    });
    defer planning.artifact.deinit(created, allocator);
    return created.id;
}

const DecisionUpsert = struct {
    id: i64,
    created: bool,
};

fn createDecisionIfMissing(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    dec: LlmDecision,
    plan_id: i64,
    scope: ?[]const u8,
) !DecisionUpsert {
    if (try findDecisionByTitleAndPlan(d, dec.title, plan_id)) |id| {
        return .{ .id = id, .created = false };
    }
    const created = try planning.decision.create(d, allocator, .{
        .title = dec.title,
        .body = if (dec.body.len > 0) dec.body else dec.title,
        .rationale = if (dec.rationale.len > 0) dec.rationale else null,
        .scope = scope,
    });
    defer planning.decision.deinit(created, allocator);
    try ensureLink(d, "decision", created.id, "plan", plan_id, "derives-from");
    return .{ .id = created.id, .created = true };
}

fn ensureLink(d: *db.sqlite.Db, from_kind: []const u8, from_id: i64, to_kind: []const u8, to_id: i64, rel: []const u8) !void {
    _ = d.execParams(
        \\insert or ignore into entity_links (from_kind, from_id, to_kind, to_id, relationship)
        \\values (?, ?, ?, ?, ?)
    , &.{ .{ .text = from_kind }, .{ .int = from_id }, .{ .text = to_kind }, .{ .int = to_id }, .{ .text = rel } }) catch return error.QueryFailed;
}

fn cancelMissingTasks(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64, keep_ids: []const i64) !usize {
    var count: usize = 0;
    var stmt = try d.prepare(
        \\select id from tasks where plan_id = ? and status != 'cancelled'
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = plan_id }});
    while (try stmt.step() == .row) {
        const id = stmt.columnInt(0);
        if (!containsI64(keep_ids, id)) {
            _ = d.execParams("update tasks set status='cancelled', updated_at=datetime('now') where id=?", &.{.{ .int = id }}) catch return error.QueryFailed;
            count += 1;
        }
    }
    _ = allocator;
    return count;
}

fn abandonMissingChildPlans(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64, keep_ids: []const i64) !usize {
    var count: usize = 0;
    var stmt = try d.prepare(
        \\select id from plans where parent_plan_id = ? and status != 'abandoned'
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = anchor_id }});
    while (try stmt.step() == .row) {
        const id = stmt.columnInt(0);
        if (!containsI64(keep_ids, id)) {
            _ = d.execParams("update plans set status='abandoned', updated_at=datetime('now') where id=?", &.{.{ .int = id }}) catch return error.QueryFailed;
            count += 1;
        }
    }
    _ = allocator;
    return count;
}

fn retireMissingArtifacts(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64, docs: []const DocEntry) !usize {
    var count: usize = 0;
    var stmt = try d.prepare(
        \\select a.id, coalesce(a.source_path,'') from artifacts a
        \\join entity_links el on el.from_kind='artifact' and el.from_id=a.id
        \\  and el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
        \\where a.status != 'retired'
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = anchor_id }});
    while (try stmt.step() == .row) {
        const aid = stmt.columnInt(0);
        const src = try stmt.columnTextAlloc(1, allocator);
        defer allocator.free(src);
        if (!docPathExists(docs, src)) {
            _ = d.execParams("update artifacts set status='retired', updated_at=datetime('now') where id=?", &.{.{ .int = aid }}) catch return error.QueryFailed;
            count += 1;
        }
    }
    return count;
}

fn supersedeMissingDecisions(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64, keep_ids: []const i64) !usize {
    var count: usize = 0;
    var stmt = try d.prepare(
        \\select d.id from decisions d
        \\join entity_links el on el.from_kind='decision' and el.from_id=d.id
        \\  and el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
        \\where d.status in ('proposed','accepted')
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = anchor_id }});
    while (try stmt.step() == .row) {
        const id = stmt.columnInt(0);
        if (!containsI64(keep_ids, id)) {
            _ = d.execParams("update decisions set status='superseded', updated_at=datetime('now') where id=?", &.{.{ .int = id }}) catch return error.QueryFailed;
            count += 1;
        }
    }
    _ = allocator;
    return count;
}

fn findPlanBySlugAndParent(d: *db.sqlite.Db, slug: []const u8, parent_id: ?i64) !?i64 {
    var stmt = try d.prepare(
        \\select id from plans where slug = ?
        \\and ((? is null and parent_plan_id is null) or parent_plan_id = ?)
        \\limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .text = slug }, if (parent_id) |v| .{ .int = v } else .{ .null = {} }, if (parent_id) |v| .{ .int = v } else .{ .null = {} } });
    return switch (try stmt.step()) {
        .done => null,
        .row => stmt.columnInt(0),
    };
}

fn findTaskBySlugAndPlan(d: *db.sqlite.Db, slug: []const u8, plan_id: i64) !?i64 {
    if (slug.len == 0) return null;
    var stmt = try d.prepare("select id from tasks where slug = ? and plan_id = ? limit 1");
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .text = slug }, .{ .int = plan_id } });
    return switch (try stmt.step()) {
        .done => null,
        .row => stmt.columnInt(0),
    };
}

fn findArtifactBySourcePathAndPlan(d: *db.sqlite.Db, source_path: []const u8, plan_id: i64) !?i64 {
    var stmt = try d.prepare(
        \\select a.id from artifacts a
        \\join entity_links el on el.from_kind='artifact' and el.from_id=a.id
        \\ and el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
        \\where coalesce(a.source_path,'') = ? limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .int = plan_id }, .{ .text = source_path } });
    return switch (try stmt.step()) {
        .done => null,
        .row => stmt.columnInt(0),
    };
}

fn findDecisionByTitleAndPlan(d: *db.sqlite.Db, title: []const u8, plan_id: i64) !?i64 {
    var stmt = try d.prepare(
        \\select d.id from decisions d
        \\join entity_links el on el.from_kind='decision' and el.from_id=d.id
        \\ and el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
        \\where d.title = ? and d.status in ('proposed','accepted') limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .int = plan_id }, .{ .text = title } });
    return switch (try stmt.step()) {
        .done => null,
        .row => stmt.columnInt(0),
    };
}

// -----------------------------------------------------------------------------
// Request build + fingerprint + JSON encode
// -----------------------------------------------------------------------------

fn buildRequest(allocator: std.mem.Allocator, abs_root: []const u8, repo_slug: []const u8, provider_kind: llm.provider.Kind) !Request {
    var docs: std.ArrayList(DocEntry) = .empty;
    errdefer deinitDocList(&docs, allocator);
    var guides: std.ArrayList(DocEntry) = .empty;
    errdefer deinitDocList(&guides, allocator);
    try collectDocs(abs_root, "", allocator, &docs, &guides);
    std.mem.sort(DocEntry, docs.items, {}, byDocPath);
    std.mem.sort(DocEntry, guides.items, {}, byDocPath);

    const tree = try listTreeSummary(allocator, abs_root);
    errdefer {
        for (tree) |entry| allocator.free(entry);
        allocator.free(tree);
    }

    const artifacts = try classifyArtifacts(allocator, docs.items);
    errdefer {
        for (artifacts) |a| {
            allocator.free(a.path);
            allocator.free(a.kind);
            allocator.free(a.source);
        }
        allocator.free(artifacts);
    }

    const evidence = try llm.evidence.probe(abs_root, allocator);
    errdefer llm.evidence.deinitEvidence(evidence, allocator);
    const fp = try fingerprintRequest(allocator, repo_slug, findReadme(docs.items) orelse "", docs.items, guides.items, tree, artifacts);
    errdefer allocator.free(fp);

    return .{
        .schema_version = schema_version,
        .repo_slug = try allocator.dupe(u8, repo_slug),
        .repo_root = try allocator.dupe(u8, abs_root),
        .fingerprint = fp,
        .provider = try allocator.dupe(u8, provider_kind.text()),
        .readme = try allocator.dupe(u8, findReadme(docs.items) orelse ""),
        .docs = try docs.toOwnedSlice(allocator),
        .guide_files = try guides.toOwnedSlice(allocator),
        .tree_summary = tree,
        .git_log = &.{},
        .detected_artifacts = artifacts,
        .code_evidence = evidence,
    };
}

fn deinitRequest(req: Request, allocator: std.mem.Allocator) void {
    allocator.free(req.repo_slug);
    allocator.free(req.repo_root);
    allocator.free(req.fingerprint);
    allocator.free(req.provider);
    allocator.free(req.readme);
    deinitDocs(req.docs, allocator);
    deinitDocs(req.guide_files, allocator);
    for (req.tree_summary) |entry| allocator.free(entry);
    allocator.free(req.tree_summary);
    allocator.free(req.git_log);
    for (req.detected_artifacts) |a| {
        allocator.free(a.path);
        allocator.free(a.kind);
        allocator.free(a.source);
    }
    allocator.free(req.detected_artifacts);
    llm.evidence.deinitEvidence(req.code_evidence, allocator);
}

fn classifyArtifacts(allocator: std.mem.Allocator, docs: []const DocEntry) ![]const DetectedArtifact {
    var out: std.ArrayList(DetectedArtifact) = .empty;
    errdefer {
        for (out.items) |a| {
            allocator.free(a.path);
            allocator.free(a.kind);
            allocator.free(a.source);
        }
        out.deinit(allocator);
    }
    for (docs) |d| {
        try out.append(allocator, .{
            .path = try allocator.dupe(u8, d.path),
            .kind = try allocator.dupe(u8, classifyArtifactKind(d.path)),
            .source = try allocator.dupe(u8, "filename"),
        });
    }
    return try out.toOwnedSlice(allocator);
}

fn makeDeterministicTranscription(allocator: std.mem.Allocator, req: Request) !InterpretationResult {
    _ = allocator;
    const anchor_title = deterministicAnchorTitle(req.repo_slug, req.readme);
    return .{
        .schema_version = schema_version,
        .fingerprint = req.fingerprint,
        .anchor_title = anchor_title,
        .phases = &.{},
        .decisions = &.{},
        .deferred_items = &.{},
        .forward_specs = &.{},
        .provenance = "deterministic-transcription",
        .generated_at = "",
    };
}

fn deterministicAnchorTitle(repo_slug: []const u8, readme: []const u8) []const u8 {
    var lines = std.mem.splitScalar(u8, readme, '\n');
    while (lines.next()) |line_raw| {
        const line = std.mem.trim(u8, line_raw, " \t\r");
        if (line.len == 0) continue;
        if (std.mem.startsWith(u8, line, "#")) {
            var i: usize = 0;
            while (i < line.len and (line[i] == '#' or line[i] == ' ' or line[i] == '\t')) : (i += 1) {}
            const title = line[i..];
            if (title.len > 0) return title;
        }
    }
    return repo_slug;
}

fn fingerprintRequest(
    allocator: std.mem.Allocator,
    repo_slug: []const u8,
    readme: []const u8,
    docs: []const DocEntry,
    guides: []const DocEntry,
    tree: []const []const u8,
    artifacts: []const DetectedArtifact,
) ![]const u8 {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    try appendRec(&buf, allocator, &.{ "repo_slug", repo_slug });
    try appendRec(&buf, allocator, &.{ "readme", readme });
    try appendRecInt(&buf, allocator, "docs", docs.len);
    for (docs) |d| try appendRec(&buf, allocator, &.{ d.path, d.body });
    try appendRecInt(&buf, allocator, "guide_files", guides.len);
    for (guides) |g| try appendRec(&buf, allocator, &.{ g.path, g.body });
    try appendRecInt(&buf, allocator, "git_log", 0);
    try appendRecInt(&buf, allocator, "tree_summary", tree.len);
    for (tree) |t| try appendRec(&buf, allocator, &.{t});
    try appendRecInt(&buf, allocator, "detected_artifacts", artifacts.len);
    for (artifacts) |a| try appendRec(&buf, allocator, &.{ a.path, a.kind, a.source });
    return llm.client.sha256HexAlloc(allocator, buf.items);
}

fn appendRec(buf: *std.ArrayList(u8), allocator: std.mem.Allocator, parts: []const []const u8) !void {
    for (parts) |p| {
        try buf.appendSlice(allocator, p);
        try buf.append(allocator, 0);
    }
}

fn appendRecInt(buf: *std.ArrayList(u8), allocator: std.mem.Allocator, key: []const u8, n: usize) !void {
    const s = try std.fmt.allocPrint(allocator, "{d}", .{n});
    defer allocator.free(s);
    try appendRec(buf, allocator, &.{ key, s });
}

fn encodeRequestJSON(allocator: std.mem.Allocator, req: Request) ![]u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    try out.writer.print("{{\"schema_version\":{},\"repo_slug\":", .{req.schema_version});
    try std.json.Stringify.value(req.repo_slug, .{}, &out.writer);
    try out.writer.print(",\"repo_root\":", .{});
    try std.json.Stringify.value(req.repo_root, .{}, &out.writer);
    try out.writer.print(",\"fingerprint\":", .{});
    try std.json.Stringify.value(req.fingerprint, .{}, &out.writer);
    try out.writer.print(",\"readme\":", .{});
    try std.json.Stringify.value(req.readme, .{}, &out.writer);
    try out.writer.print(",\"docs\":{{", .{});
    for (req.docs, 0..) |d, i| {
        if (i > 0) try out.writer.print(",", .{});
        try std.json.Stringify.value(d.path, .{}, &out.writer);
        try out.writer.print(":", .{});
        try std.json.Stringify.value(d.body, .{}, &out.writer);
    }
    try out.writer.print("}},\"guide_files\":{{", .{});
    for (req.guide_files, 0..) |g, i| {
        if (i > 0) try out.writer.print(",", .{});
        try std.json.Stringify.value(g.path, .{}, &out.writer);
        try out.writer.print(":", .{});
        try std.json.Stringify.value(g.body, .{}, &out.writer);
    }
    try out.writer.print("}},\"git_log\":[],\"tree_summary\":", .{});
    try std.json.Stringify.value(req.tree_summary, .{}, &out.writer);
    try out.writer.print(",\"detected_artifacts\":", .{});
    try std.json.Stringify.value(req.detected_artifacts, .{}, &out.writer);
    try out.writer.print(",\"code_evidence\":", .{});
    try std.json.Stringify.value(req.code_evidence, .{}, &out.writer);
    try out.writer.print("}}\n", .{});
    return allocator.dupe(u8, out.written());
}

fn awaitingMessage(allocator: std.mem.Allocator, pending_path: []const u8, cache_path: []const u8) ![]const u8 {
    return std.fmt.allocPrint(
        allocator,
        "Awaiting LLM interpretation. The vendor skill should:\n  1. read  {s}\n  2. run the LLM at temperature 0\n  3. write the Result to {s}\n  4. re-invoke `planar pl-import <repo> --interpret`\nSee `commands/claude/pl-import.md` for the full contract.",
        .{ pending_path, cache_path },
    );
}

fn collectDocs(root: []const u8, rel: []const u8, allocator: std.mem.Allocator, docs: *std.ArrayList(DocEntry), guides: *std.ArrayList(DocEntry)) !void {
    const dir_path = if (rel.len == 0) try allocator.dupe(u8, root) else try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(dir_path);
    var dir = try std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true });
    defer dir.close(fsIo());
    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        if (std.mem.eql(u8, entry.name, ".git")) continue;
        if (entry.name.len > 0 and entry.name[0] == '.') continue;
        const child_rel = if (rel.len == 0) try allocator.dupe(u8, entry.name) else try std.fs.path.join(allocator, &.{ rel, entry.name });
        defer allocator.free(child_rel);
        switch (entry.kind) {
            .directory => try collectDocs(root, child_rel, allocator, docs, guides),
            .file => {
                const lower = try std.ascii.allocLowerString(allocator, entry.name);
                defer allocator.free(lower);
                const is_guide = std.mem.eql(u8, lower, "agents.md") or std.mem.eql(u8, lower, "claude.md");
                if (!is_guide and !std.mem.endsWith(u8, lower, ".md")) continue;
                const file_path = try std.fs.path.join(allocator, &.{ root, child_rel });
                defer allocator.free(file_path);
                const body = try std.Io.Dir.cwd().readFileAlloc(fsIo(), file_path, allocator, std.Io.Limit.limited(1024 * 1024));
                const dst = if (is_guide) guides else docs;
                try dst.append(allocator, .{ .path = try allocator.dupe(u8, child_rel), .body = body });
            },
            else => {},
        }
    }
}

fn listTreeSummary(allocator: std.mem.Allocator, root: []const u8) ![]const []const u8 {
    var dir = try std.Io.Dir.cwd().openDir(fsIo(), root, .{ .iterate = true });
    defer dir.close(fsIo());
    var out: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (out.items) |s| allocator.free(s);
        out.deinit(allocator);
    }
    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        if (std.mem.eql(u8, entry.name, ".git")) continue;
        try out.append(allocator, if (entry.kind == .directory) try std.fmt.allocPrint(allocator, "{s}/", .{entry.name}) else try allocator.dupe(u8, entry.name));
    }
    std.mem.sort([]const u8, out.items, {}, struct {
        fn less(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.lessThan(u8, a, b);
        }
    }.less);
    return out.toOwnedSlice(allocator);
}

fn deinitDocs(docs: []const DocEntry, allocator: std.mem.Allocator) void {
    for (docs) |d| {
        allocator.free(d.path);
        allocator.free(d.body);
    }
    allocator.free(docs);
}

fn deinitDocList(list: *std.ArrayList(DocEntry), allocator: std.mem.Allocator) void {
    for (list.items) |d| {
        allocator.free(d.path);
        allocator.free(d.body);
    }
    list.deinit(allocator);
}

fn findReadme(docs: []const DocEntry) ?[]const u8 {
    for (docs) |d| {
        const base = std.fs.path.basename(d.path);
        if (std.ascii.eqlIgnoreCase(base, "readme") or std.ascii.eqlIgnoreCase(base, "readme.md")) return d.body;
    }
    return null;
}

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

fn deriveRepoSlug(allocator: std.mem.Allocator, abs_root: []const u8) ![]const u8 {
    const base = std.fs.path.basename(abs_root);
    return std.ascii.allocLowerString(allocator, if (base.len == 0) "repo" else base);
}

fn isDir(path: []const u8) bool {
    if (std.Io.Dir.cwd().openDir(fsIo(), path, .{})) |d| {
        d.close(fsIo());
        return true;
    } else |_| return false;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

fn containsI64(hay: []const i64, needle: i64) bool {
    for (hay) |v| if (v == needle) return true;
    return false;
}

fn docPathExists(docs: []const DocEntry, source_path: []const u8) bool {
    for (docs) |d| if (std.mem.eql(u8, d.path, source_path)) return true;
    return false;
}

fn classifyArtifactKind(path: []const u8) []const u8 {
    const base = std.fs.path.basename(path);
    const lower = std.ascii.allocLowerString(std.heap.page_allocator, base) catch return "research";
    defer std.heap.page_allocator.free(lower);
    if (std.mem.indexOf(u8, lower, "roadmap") != null) return "roadmap";
    if (std.mem.indexOf(u8, lower, "product") != null) return "product_spec";
    if (std.mem.indexOf(u8, lower, "adr") != null) return "adr";
    if (std.mem.indexOf(u8, lower, "tech") != null) return "tech_spec";
    if (std.mem.indexOf(u8, lower, "readme") != null) return "readme";
    return "research";
}

fn isValidPlanStatus(s: []const u8) bool {
    return std.mem.eql(u8, s, "draft") or std.mem.eql(u8, s, "active") or std.mem.eql(u8, s, "paused") or std.mem.eql(u8, s, "done") or std.mem.eql(u8, s, "abandoned");
}

fn isValidTaskStatus(s: []const u8) bool {
    return std.mem.eql(u8, s, "todo") or std.mem.eql(u8, s, "doing") or std.mem.eql(u8, s, "blocked") or std.mem.eql(u8, s, "done") or std.mem.eql(u8, s, "cancelled");
}

fn statusFromText(s: []const u8) planning.plan.Status {
    return planning.plan.Status.fromText(s) orelse .draft;
}

fn taskStatusFromText(s: []const u8) planning.task.Status {
    return planning.task.Status.fromText(s) orelse .todo;
}

fn artifactKindFromText(s: []const u8) planning.artifact.Kind {
    return planning.artifact.Kind.fromText(s) orelse .other;
}

fn slugify(allocator: std.mem.Allocator, title: []const u8) ![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var dash = false;
    for (title) |c| {
        const lc = std.ascii.toLower(c);
        if ((lc >= 'a' and lc <= 'z') or (lc >= '0' and lc <= '9')) {
            try out.append(allocator, lc);
            dash = false;
        } else if (!dash and out.items.len > 0) {
            try out.append(allocator, '-');
            dash = true;
        }
    }
    while (out.items.len > 0 and out.items[out.items.len - 1] == '-') _ = out.pop();
    if (out.items.len == 0) try out.appendSlice(allocator, "imported-plan");
    return out.toOwnedSlice(allocator);
}

fn byDocPath(_: void, a: DocEntry, b: DocEntry) bool {
    return std.mem.lessThan(u8, a.path, b.path);
}

test "request fingerprint excludes absolute repo path" {
    const gpa = std.testing.allocator;
    const docs = [_]DocEntry{.{ .path = "README.md", .body = "x" }};
    const guides = [_]DocEntry{};
    const tree = [_][]const u8{"README.md"};
    const arts = [_]DetectedArtifact{.{ .path = "README.md", .kind = "readme", .source = "filename" }};
    const a = try fingerprintRequest(gpa, "repo", "x", &docs, &guides, &tree, &arts);
    defer gpa.free(a);
    const b = try fingerprintRequest(gpa, "repo", "x", &docs, &guides, &tree, &arts);
    defer gpa.free(b);
    try std.testing.expectEqualStrings(a, b);
}
