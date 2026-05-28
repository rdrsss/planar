//! engine/synthesize — M18 synthesize parity: staged request, validated cache, apply.

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
    apply: bool = false,
    apply_removals: bool = false,
    provider_override: ?[]const u8 = null,
    code_layout: ?[]const u8 = null,
    treat_as_greenfield: bool = false,
    treat_as_nongreenfield: bool = false,
    scope: ?[]const u8 = null,
};

pub const Mode = enum { pending, cache_hit };

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
    cache_path: []const u8,
    pending_path: []const u8,
    docs_count: usize,
    guide_files_count: usize,
    tree_entry_count: usize,
    greenfield: bool,
    message: []const u8,
    applied: ?ApplyReport = null,
};

pub fn deinitOutcome(out: Outcome, allocator: std.mem.Allocator) void {
    allocator.free(out.repo_slug);
    allocator.free(out.fingerprint);
    allocator.free(out.cache_path);
    allocator.free(out.pending_path);
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
    if (opts.treat_as_greenfield and opts.treat_as_nongreenfield) return error.InvalidInput;
    if (opts.code_layout) |raw| if (!isValidCodeLayout(raw)) return error.InvalidInput;
    if (!isDir(opts.repo_root)) return error.NotFound;

    const abs_root = try std.Io.Dir.realPathFileAlloc(.cwd(), fsIo(), opts.repo_root, allocator);
    defer allocator.free(abs_root);
    const provider_kind = try llm.provider.resolve(opts.provider_override, environ);
    const repo_slug = try deriveRepoSlug(allocator, abs_root);
    errdefer allocator.free(repo_slug);

    const req = try buildRequest(allocator, abs_root, repo_slug, provider_kind, opts);
    defer deinitRequest(req, allocator);

    const cache_path = try llm.client.cachePath(allocator, planar_home, .bootstrap_synthesis, req.repo_slug, req.fingerprint);
    errdefer allocator.free(cache_path);
    const pending_path = try llm.client.pendingPath(allocator, planar_home, .bootstrap_synthesis, req.repo_slug);
    errdefer allocator.free(pending_path);

    const cached_opt = try llm.client.readIfExists(cache_path, allocator, 16 * 1024 * 1024);
    if (cached_opt) |cached| {
        defer allocator.free(cached);
        var arena = std.heap.ArenaAllocator.init(allocator);
        defer arena.deinit();
        const result = try parseSynthesisResult(arena.allocator(), cached);
        try validateSynthesisResult(result, req);
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
            .greenfield = req.greenfield,
            .message = try std.fmt.allocPrint(allocator, "Loaded cached synthesis result: {s}", .{cache_path}),
        };
        if (opts.apply) {
            const d = d_opt orelse return error.InvalidInput;
            out.applied = try applySynthesis(d, allocator, req, result, opts.scope, opts.apply_removals);
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
        .greenfield = req.greenfield,
        .message = try awaitingMessage(allocator, pending_path, cache_path),
    };
}

// -----------------------------------------------------------------------------
// Request / result
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
pub const Request = struct {
    schema_version: i64,
    repo_slug: []const u8,
    repo_root: []const u8,
    fingerprint: []const u8,
    readme: []const u8,
    docs: []const DocEntry,
    guide_files: []const DocEntry,
    tree_summary: []const []const u8,
    git_log: []const GitCommit,
    code_evidence: llm.evidence.EvidenceMap,
    greenfield: bool,
    code_layout: []const u8,
};

pub const CodeCitation = struct { path: []const u8 = "", lines: []const u8 = "" };
pub const DocCitation = struct { path: []const u8 = "", section: []const u8 = "", line: i64 = 0 };
pub const SynthTask = struct {
    slug: []const u8 = "",
    title: []const u8 = "",
    body: []const u8 = "",
    status: []const u8 = "",
    priority: i64 = 0,
    next_action: []const u8 = "",
    code_evidence: []const CodeCitation = &.{},
    citations: []const DocCitation = &.{},
};
pub const Phase = struct {
    slug: []const u8 = "",
    title: []const u8 = "",
    summary: []const u8 = "",
    status: []const u8 = "",
    tasks: []const SynthTask = &.{},
};
pub const Decision = struct {
    slug: []const u8 = "",
    title: []const u8 = "",
    body: []const u8 = "",
    rationale: []const u8 = "",
    source: []const u8 = "",
    citation: DocCitation = .{},
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
pub const ReferenceArtifact = struct { path: []const u8 = "", kind: []const u8 = "", title: []const u8 = "" };
pub const SynthesisResult = struct {
    schema_version: i64 = 0,
    fingerprint: []const u8 = "",
    synthesized: bool = false,
    anchor_title: []const u8 = "",
    phases: []const Phase = &.{},
    decisions: []const Decision = &.{},
    deferred_items: []const DeferredItem = &.{},
    forward_specs: []const ForwardSpec = &.{},
    reference_artifacts: []const ReferenceArtifact = &.{},
    code_evidence_summary: []const u8 = "",
    provenance: []const u8 = "",
    generated_at: []const u8 = "",
};

fn parseSynthesisResult(arena: std.mem.Allocator, raw: []const u8) !SynthesisResult {
    const parsed = try std.json.parseFromSlice(SynthesisResult, arena, raw, .{
        .ignore_unknown_fields = true,
    });
    return parsed.value;
}

fn validateSynthesisResult(r: SynthesisResult, req: Request) !void {
    if (r.schema_version != schema_version) return error.InvalidInput;
    if (!std.mem.eql(u8, r.fingerprint, req.fingerprint)) return error.InvalidInput;
    if (!r.synthesized) return error.InvalidInput;
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
            if (!std.mem.eql(u8, t.status, "todo")) {
                if (req.greenfield) return error.InvalidInput;
                if (t.code_evidence.len == 0) return error.InvalidInput;
                for (t.code_evidence) |cc| if (!codeEvidencePathAllowed(req.code_evidence, cc.path)) return error.InvalidInput;
            }
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
    for (r.reference_artifacts) |ra| {
        if (ra.path.len == 0) return error.InvalidInput;
        try validateDocPath(req.repo_root, ra.path);
    }
}

fn codeEvidencePathAllowed(e: llm.evidence.EvidenceMap, path: []const u8) bool {
    for (e.areas) |a| {
        if (std.mem.eql(u8, path, a.path)) return true;
        const pref = std.mem.concat(std.heap.page_allocator, u8, &.{ a.path, "/" }) catch return false;
        defer std.heap.page_allocator.free(pref);
        if (std.mem.startsWith(u8, path, pref)) return true;
    }
    return false;
}

fn validateDocPath(repo_root: []const u8, rel_path: []const u8) !void {
    if (rel_path.len == 0) return;
    const full = try std.fs.path.join(std.heap.page_allocator, &.{ repo_root, rel_path });
    defer std.heap.page_allocator.free(full);
    std.Io.Dir.cwd().access(fsIo(), full, .{}) catch return error.InvalidInput;
}

// -----------------------------------------------------------------------------
// Apply
// -----------------------------------------------------------------------------

fn applySynthesis(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    req: Request,
    result: SynthesisResult,
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
        if (apply_removals) report.tasks_cancelled += try cancelMissingTasks(d, pid, keep_task_ids.items);
    }
    if (apply_removals) report.plans_abandoned += try abandonMissingChildPlans(d, anchor_id, keep_plan_ids.items);

    // synthesized artifacts
    report.artifacts_created += @as(usize, @intCast(try ensureSynthArtifacts(d, allocator, result, req.repo_slug, anchor_id, scope)));
    // reference artifacts
    for (result.reference_artifacts) |ra| {
        const title = if (ra.title.len > 0) ra.title else std.fs.path.basename(ra.path);
        const kind = if (ra.kind.len > 0) ra.kind else "research";
        const body = try std.fmt.allocPrint(allocator, "Preserved from {s} as input material to pl-synthesize.\n", .{ra.path});
        defer allocator.free(body);
        if (try createArtifactIfMissing(d, allocator, title, kind, body, ra.path, anchor_id, scope) > 0) report.artifacts_created += 1;
    }
    if (apply_removals) report.artifacts_retired += try retireArtifactsNotInResult(d, allocator, anchor_id, result.reference_artifacts);

    var keep_decision_ids = std.ArrayList(i64).empty;
    defer keep_decision_ids.deinit(allocator);
    for (result.decisions) |dec| {
        const upserted = try createDecisionIfMissing(d, allocator, dec, anchor_id, scope);
        if (upserted.created) report.decisions_created += 1;
        try keep_decision_ids.append(allocator, upserted.id);
    }
    if (apply_removals) report.decisions_superseded += try supersedeMissingDecisions(d, anchor_id, keep_decision_ids.items);
    return report;
}

fn ensureSynthArtifacts(d: *db.sqlite.Db, allocator: std.mem.Allocator, r: SynthesisResult, repo_slug: []const u8, plan_id: i64, scope: ?[]const u8) !i64 {
    var count: i64 = 0;
    const title = r.anchor_title;
    const product = try std.fmt.allocPrint(allocator, "# {s} — Product Spec\n\n> Synthesized by pl-synthesize from {s}.\n", .{ title, repo_slug });
    defer allocator.free(product);
    if (try createArtifactIfMissing(d, allocator, try std.fmt.allocPrint(allocator, "{s} — Product Spec", .{title}), "product_spec", product, "pl-synthesize://product_spec", plan_id, scope) > 0) count += 1;
    const tech = try std.fmt.allocPrint(allocator, "# {s} — Tech Spec\n\n> Synthesized by pl-synthesize from {s}.\n", .{ title, repo_slug });
    defer allocator.free(tech);
    if (try createArtifactIfMissing(d, allocator, try std.fmt.allocPrint(allocator, "{s} — Tech Spec", .{title}), "tech_spec", tech, "pl-synthesize://tech_spec", plan_id, scope) > 0) count += 1;
    const roadmap = try std.fmt.allocPrint(allocator, "# {s} — Roadmap\n\n> Synthesized by pl-synthesize from {s}.\n", .{ title, repo_slug });
    defer allocator.free(roadmap);
    if (try createArtifactIfMissing(d, allocator, try std.fmt.allocPrint(allocator, "{s} — Roadmap", .{title}), "roadmap", roadmap, "pl-synthesize://roadmap", plan_id, scope) > 0) count += 1;
    return count;
}

fn retireArtifactsNotInResult(d: *db.sqlite.Db, allocator: std.mem.Allocator, anchor_id: i64, refs: []const ReferenceArtifact) !usize {
    var count: usize = 0;
    var stmt = try d.prepare(
        \\select a.id, coalesce(a.source_path,'') from artifacts a
        \\join entity_links el on el.from_kind='artifact' and el.from_id=a.id
        \\ and el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
        \\where a.status != 'retired'
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = anchor_id }});
    while (try stmt.step() == .row) {
        const aid = stmt.columnInt(0);
        const src = try stmt.columnTextAlloc(1, allocator);
        defer allocator.free(src);
        var keep = std.mem.startsWith(u8, src, "pl-synthesize://");
        if (!keep) for (refs) |ra| if (std.mem.eql(u8, src, ra.path)) {
            keep = true;
            break;
        };
        if (!keep) {
            _ = d.execParams("update artifacts set status='retired', updated_at=datetime('now') where id=?", &.{.{ .int = aid }}) catch return error.QueryFailed;
            count += 1;
        }
    }
    return count;
}

// upsert primitives reused
fn upsertPlan(d: *db.sqlite.Db, allocator: std.mem.Allocator, title: []const u8, slug: []const u8, status: []const u8, parent_id: ?i64, scope: ?[]const u8, report: *ApplyReport) !i64 {
    if (try findPlanBySlugAndParent(d, slug, parent_id)) |id| {
        _ = d.execParams("update plans set title=?, status=?, updated_at=datetime('now') where id=?", &.{ .{ .text = title }, .{ .text = status }, .{ .int = id } }) catch return error.QueryFailed;
        report.plans_updated += 1;
        return id;
    }
    const created = try planning.plan.create(d, allocator, .{ .title = title, .slug = slug, .status = statusFromText(status), .parent_plan_id = parent_id, .scope = scope });
    defer planning.plan.deinit(created, allocator);
    report.plans_created += 1;
    return created.id;
}

fn upsertTask(d: *db.sqlite.Db, allocator: std.mem.Allocator, t: SynthTask, plan_id: i64, scope: ?[]const u8, report: *ApplyReport) !i64 {
    if (try findTaskBySlugAndPlan(d, t.slug, plan_id)) |id| {
        _ = d.execParams(
            "update tasks set title=?, body=?, status=?, priority=?, next_action=?, updated_at=datetime('now') where id=?",
            &.{ .{ .text = t.title }, .{ .text = t.body }, .{ .text = t.status }, .{ .int = t.priority }, if (t.next_action.len > 0) .{ .text = t.next_action } else .{ .null = {} }, .{ .int = id } },
        ) catch return error.QueryFailed;
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

fn cancelMissingTasks(d: *db.sqlite.Db, plan_id: i64, keep_ids: []const i64) !usize {
    var count: usize = 0;
    var stmt = try d.prepare("select id from tasks where plan_id = ? and status != 'cancelled'");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = plan_id }});
    while (try stmt.step() == .row) {
        const id = stmt.columnInt(0);
        if (!containsI64(keep_ids, id)) {
            _ = d.execParams("update tasks set status='cancelled', updated_at=datetime('now') where id=?", &.{.{ .int = id }}) catch return error.QueryFailed;
            count += 1;
        }
    }
    return count;
}

fn abandonMissingChildPlans(d: *db.sqlite.Db, anchor_id: i64, keep_ids: []const i64) !usize {
    var count: usize = 0;
    var stmt = try d.prepare("select id from plans where parent_plan_id = ? and status != 'abandoned'");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = anchor_id }});
    while (try stmt.step() == .row) {
        const id = stmt.columnInt(0);
        if (!containsI64(keep_ids, id)) {
            _ = d.execParams("update plans set status='abandoned', updated_at=datetime('now') where id=?", &.{.{ .int = id }}) catch return error.QueryFailed;
            count += 1;
        }
    }
    return count;
}

fn createArtifactIfMissing(d: *db.sqlite.Db, allocator: std.mem.Allocator, title: []const u8, kind: []const u8, body: []const u8, source_path: []const u8, plan_id: i64, scope: ?[]const u8) !i64 {
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

fn createDecisionIfMissing(d: *db.sqlite.Db, allocator: std.mem.Allocator, dec: Decision, plan_id: i64, scope: ?[]const u8) !DecisionUpsert {
    if (try findDecisionByTitleAndPlan(d, dec.title, plan_id)) |id| return .{ .id = id, .created = false };
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

fn supersedeMissingDecisions(d: *db.sqlite.Db, anchor_id: i64, keep_ids: []const i64) !usize {
    var count: usize = 0;
    var stmt = try d.prepare(
        \\select d.id from decisions d
        \\join entity_links el on el.from_kind='decision' and el.from_id=d.id
        \\ and el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
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
    return count;
}

fn ensureLink(d: *db.sqlite.Db, from_kind: []const u8, from_id: i64, to_kind: []const u8, to_id: i64, rel: []const u8) !void {
    _ = d.execParams(
        "insert or ignore into entity_links (from_kind, from_id, to_kind, to_id, relationship) values (?, ?, ?, ?, ?)",
        &.{ .{ .text = from_kind }, .{ .int = from_id }, .{ .text = to_kind }, .{ .int = to_id }, .{ .text = rel } },
    ) catch return error.QueryFailed;
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
// request building
// -----------------------------------------------------------------------------

fn buildRequest(allocator: std.mem.Allocator, abs_root: []const u8, repo_slug: []const u8, _: llm.provider.Kind, opts: Opts) !Request {
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
    const evidence = try llm.evidence.probe(abs_root, allocator);
    errdefer llm.evidence.deinitEvidence(evidence, allocator);

    const greenfield = if (opts.treat_as_greenfield) true else if (opts.treat_as_nongreenfield) false else isAutoGreenfield(evidence);
    const fp = try fingerprintRequest(allocator, repo_slug, findReadme(docs.items) orelse "", docs.items, guides.items, tree, evidence, greenfield, opts.code_layout orelse "");
    errdefer allocator.free(fp);
    return .{
        .schema_version = schema_version,
        .repo_slug = try allocator.dupe(u8, repo_slug),
        .repo_root = try allocator.dupe(u8, abs_root),
        .fingerprint = fp,
        .readme = try allocator.dupe(u8, findReadme(docs.items) orelse ""),
        .docs = try docs.toOwnedSlice(allocator),
        .guide_files = try guides.toOwnedSlice(allocator),
        .tree_summary = tree,
        .git_log = &.{},
        .code_evidence = evidence,
        .greenfield = greenfield,
        .code_layout = try allocator.dupe(u8, opts.code_layout orelse ""),
    };
}

fn deinitRequest(req: Request, allocator: std.mem.Allocator) void {
    allocator.free(req.repo_slug);
    allocator.free(req.repo_root);
    allocator.free(req.fingerprint);
    allocator.free(req.readme);
    deinitDocs(req.docs, allocator);
    deinitDocs(req.guide_files, allocator);
    for (req.tree_summary) |entry| allocator.free(entry);
    allocator.free(req.tree_summary);
    allocator.free(req.git_log);
    llm.evidence.deinitEvidence(req.code_evidence, allocator);
    allocator.free(req.code_layout);
}

fn fingerprintRequest(
    allocator: std.mem.Allocator,
    repo_slug: []const u8,
    readme: []const u8,
    docs: []const DocEntry,
    guides: []const DocEntry,
    tree: []const []const u8,
    evidence: llm.evidence.EvidenceMap,
    greenfield: bool,
    code_layout: []const u8,
) ![]const u8 {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);
    try appendRec(&buf, allocator, &.{ "repo_slug", repo_slug });
    try appendRec(&buf, allocator, &.{ "readme", readme });
    try appendRecInt(&buf, allocator, "docs", docs.len);
    for (docs) |d| try appendRec(&buf, allocator, &.{ d.path, d.body });
    try appendRecInt(&buf, allocator, "guide_files", guides.len);
    for (guides) |g| try appendRec(&buf, allocator, &.{ g.path, g.body });
    try appendRecInt(&buf, allocator, "tree_summary", tree.len);
    for (tree) |t| try appendRec(&buf, allocator, &.{t});
    try appendRecInt(&buf, allocator, "git_log", 0);
    try appendRec(&buf, allocator, &.{ "layout", evidence.layout, code_layout });
    try appendRecInt(&buf, allocator, "areas", evidence.areas.len);
    for (evidence.areas) |a| {
        try appendRec(&buf, allocator, &.{ a.path, a.name });
        try appendRecIntI64(&buf, allocator, "source_files", a.source_files);
        try appendRecIntI64(&buf, allocator, "test_files", a.test_files);
    }
    try appendRec(&buf, allocator, &.{ "greenfield", if (greenfield) "1" else "0" });
    return llm.client.sha256HexAlloc(allocator, buf.items);
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
    try out.writer.print(",\"code_evidence\":", .{});
    try std.json.Stringify.value(req.code_evidence, .{}, &out.writer);
    try out.writer.print(",\"greenfield\":{},\"code_layout\":", .{req.greenfield});
    try std.json.Stringify.value(req.code_layout, .{}, &out.writer);
    try out.writer.print("}}\n", .{});
    return allocator.dupe(u8, out.written());
}

fn awaitingMessage(allocator: std.mem.Allocator, pending_path: []const u8, cache_path: []const u8) ![]const u8 {
    return std.fmt.allocPrint(
        allocator,
        "Awaiting LLM synthesis. The vendor skill should:\n  1. read  {s}\n  2. run the LLM at temperature 0\n  3. write the Result to {s}\n  4. re-invoke `planar synthesize <repo-root>`\nSee `commands/claude/pl-synthesize.md` for the full contract.",
        .{ pending_path, cache_path },
    );
}

// docs walkers/helpers
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

// shared primitives
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
fn appendRecIntI64(buf: *std.ArrayList(u8), allocator: std.mem.Allocator, key: []const u8, n: i64) !void {
    const s = try std.fmt.allocPrint(allocator, "{d}", .{n});
    defer allocator.free(s);
    try appendRec(buf, allocator, &.{ key, s });
}

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

fn isValidCodeLayout(raw: []const u8) bool {
    return raw.len == 0 or
        std.mem.eql(u8, raw, "swift") or
        std.mem.eql(u8, raw, "go") or
        std.mem.eql(u8, raw, "node") or
        std.mem.eql(u8, raw, "python") or
        std.mem.eql(u8, raw, "mixed");
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
    if (out.items.len == 0) try out.appendSlice(allocator, "synthesized-plan");
    return out.toOwnedSlice(allocator);
}
fn byDocPath(_: void, a: DocEntry, b: DocEntry) bool {
    return std.mem.lessThan(u8, a.path, b.path);
}

fn isAutoGreenfield(evidence: llm.evidence.EvidenceMap) bool {
    if (evidence.total_files == 0 or evidence.total_lines == 0) return true;
    for (evidence.areas) |area| {
        if (area.signal_strength != 0.0) return false;
    }
    return true;
}

test "fingerprint stable across checkout roots for same content" {
    const gpa = std.testing.allocator;
    const docs = [_]DocEntry{.{ .path = "README.md", .body = "x" }};
    const guides = [_]DocEntry{};
    const tree = [_][]const u8{"README.md"};
    const evidence = llm.evidence.EvidenceMap{
        .layout = "go",
        .areas = &.{},
        .total_files = 0,
        .total_lines = 0,
        .has_tests = false,
        .has_ci = false,
        .recent_commits = &.{},
    };
    const a = try fingerprintRequest(gpa, "repo", "x", &docs, &guides, &tree, evidence, true, "go");
    defer gpa.free(a);
    const b = try fingerprintRequest(gpa, "repo", "x", &docs, &guides, &tree, evidence, true, "go");
    defer gpa.free(b);
    try std.testing.expectEqualStrings(a, b);
}
