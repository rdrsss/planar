//! engine/extsync/parent_issue — GitHub `parent-issue` propagation strategy.
//!
//! Mirrors Go `internal/extsync/github.go` for the single-repo parent/sub-issue
//! flow. The Go entry points this module ports are:
//!
//!   - `propagateParentIssue` — single-repo strategy: anchor → parent issue,
//!     each child plan → sub-issue of parent, each task → sub-issue of its
//!     child plan, anchor-direct tasks → sub-issues of parent.
//!   - `propagateParentIssueWithRepo` — same as above but with the owner/repo
//!     pair injected by the caller (used by zero-repo strategy).
//!   - `propagateZeroRepo` — reads `github_lead_repo` from the config plane
//!     and delegates to `propagateParentIssueWithRepo`.
//!   - `detectParentIssueSupport` — probes `LinkSubIssueProbe` once per anchor
//!     and caches the result on the anchor's external_links row.
//!
//! Helpers ported as small free functions:
//!   `parseGitHubRepo`, `projectGitHubCoords`, `resolveTargetRepo`,
//!   `firstTouchedRepo`, `localEntityTitle`, `localEntityBody`,
//!   `tasksUnderPlan`, `directTasksOf`, `childPlansOf`, `recordLink`,
//!   `postDecisionComments`.
//!
//! This module is adapter-agnostic at the call surface: the caller hands in
//! a `GhClient` with function pointers for each REST endpoint, so unit tests
//! can drive the engine without an HTTP client. The CLI handler wraps the real
//! `engine.extsync.github.GithubAdapter` in a `GhClient`.

const std = @import("std");
const db = @import("db");
const link_mod = @import("../external/link.zig");

/// Op is the per-entity outcome the engine reports.
pub const Op = enum { created, skipped, failed };

/// EntityResult is one row in the report's results slice. All slices are
/// allocator-owned and must be freed via the consumer's deinit (the handler
/// reuses its own LineResult shape, so this engine returns owned slices).
pub const EntityResult = struct {
    entity_kind: []const u8,
    entity_id: i64,
    title: []const u8,
    op: Op,
    external_id: []const u8,
    external_url: []const u8,
    error_name: []const u8,
};

/// Report carries the propagate summary plus per-entity rows.
pub const Report = struct {
    created: usize = 0,
    skipped: usize = 0,
    failed: usize = 0,
    /// Effective strategy after fallback resolution. The engine writes
    /// "github-parent-issue" on success and may emit warnings when the
    /// probe fails — the handler decides whether to fall back to
    /// tracking-issue. For now we surface ProbeUnsupported up and the
    /// caller chooses how to react.
    strategy: []const u8 = "github-parent-issue",
    /// Owned result rows.
    results: []EntityResult = &.{},

    pub fn deinit(self: *Report, allocator: std.mem.Allocator) void {
        for (self.results) |r| {
            allocator.free(r.entity_kind);
            allocator.free(r.title);
            allocator.free(r.external_id);
            allocator.free(r.external_url);
            allocator.free(r.error_name);
        }
        allocator.free(self.results);
    }
};

/// Opts captures the subset of Go PropagateOpts the parent-issue path needs.
pub const Opts = struct {
    sys_id: i64,
    sys_slug: []const u8,
    dry_run: bool = false,
    sync_direction: link_mod.SyncDirection = .@"two-way",
};

/// GhClient is the adapter surface this engine needs. Function pointers let
/// unit tests inject fakes; the real CLI handler wraps a `GithubAdapter`.
pub const GhClient = struct {
    ctx: *anyopaque,

    /// probeFn returns null on supported, error.SubIssueUnsupported on 404, or
    /// any other adapter error.
    probeFn: *const fn (ctx: *anyopaque, allocator: std.mem.Allocator, owner: []const u8, repo: []const u8) anyerror!void,

    /// createIssueFn POSTs a regular issue. Caller owns `node_id` (may be "").
    createIssueFn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
        title: []const u8,
        body: []const u8,
        labels: []const []const u8,
    ) anyerror!CreatedIssue,

    /// linkSubIssueFn POSTs the sub-issue parent relation. Returns
    /// error.SubIssueLinkFailed if the link fails for any reason after the
    /// issue was created; the caller decides whether to keep the orphaned issue.
    linkSubIssueFn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
        parent_number: i64,
        child_number: i64,
    ) anyerror!void,

    /// postCommentFn posts a comment on an issue identified by `external_id`.
    /// Errors are non-fatal; the engine logs and continues.
    postCommentFn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        external_id: []const u8,
        body: []const u8,
    ) anyerror!void,
};

/// CreatedIssue is the wire result of a single REST create. `node_id` is
/// allocator-owned (may be empty).
pub const CreatedIssue = struct {
    number: i64,
    node_id: []const u8,
};

pub const Error = error{
    NoTouchedRepos,
    CannotResolveRepo,
    SubIssueUnsupported,
    QueryFailed,
    BadConfig,
    WriteFailed,
} || std.mem.Allocator.Error;

// -----------------------------------------------------------------------------
// Public entry points
// -----------------------------------------------------------------------------

/// propagateParentIssue runs the single-repo parent-issue flow for `anchor_plan_id`.
/// Resolves the target repo from the feature's touched-repo set; returns
/// `NoTouchedRepos` when the anchor has none (caller should use zero-repo
/// strategy first).
pub fn propagateParentIssue(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    client: GhClient,
    anchor_plan_id: i64,
    opts: Opts,
) Error!Report {
    const target = resolveTargetRepo(allocator, d, anchor_plan_id) catch |e| switch (e) {
        Error.NoTouchedRepos => return e,
        Error.CannotResolveRepo => return e,
        else => return e,
    };
    defer allocator.free(target.owner);
    defer allocator.free(target.repo);

    return try propagateParentIssueWithRepo(allocator, d, client, anchor_plan_id, target.owner, target.repo, opts);
}

/// propagateParentIssueWithRepo runs the parent-issue flow against an
/// explicitly-supplied owner/repo. Used by `propagateZeroRepo` (which reads
/// `github_lead_repo`) and by tests.
pub fn propagateParentIssueWithRepo(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    client: GhClient,
    anchor_plan_id: i64,
    owner: []const u8,
    repo: []const u8,
    opts: Opts,
) Error!Report {
    var report: Report = .{};
    var results: std.ArrayList(EntityResult) = .empty;
    errdefer {
        for (results.items) |r| {
            allocator.free(r.entity_kind);
            allocator.free(r.title);
            allocator.free(r.external_id);
            allocator.free(r.external_url);
            allocator.free(r.error_name);
        }
        results.deinit(allocator);
    }

    // Probe for sub-issue support unless dry-run.
    if (!opts.dry_run) {
        const supported = try detectParentIssueSupport(allocator, d, client, owner, repo, anchor_plan_id, opts.sys_id);
        if (!supported) return Error.SubIssueUnsupported;
    }

    // ---- step 1: create or skip the anchor parent issue ----
    const parent = try createOrSkipGithubIssue(
        allocator,
        d,
        client,
        opts,
        "plan",
        anchor_plan_id,
        0, // no parent for the anchor itself
        owner,
        repo,
        &.{},
        &results,
    );

    // Cache strategy + parent coords on the anchor's mirror link.
    if (!opts.dry_run and parent.external_id.len > 0) {
        const cfg = try std.fmt.allocPrint(
            allocator,
            "{{\"strategy\":\"github-parent-issue\",\"parent_issue_repo\":\"{s}/{s}\",\"parent_issue_num\":{d}}}",
            .{ owner, repo, parent.number },
        );
        defer allocator.free(cfg);
        _ = d.execParams(
            \\update external_links set config_json = ?
            \\where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror'
        , &.{ .{ .text = cfg }, .{ .int = anchor_plan_id }, .{ .int = opts.sys_id } }) catch return Error.QueryFailed;
    }

    // ---- step 2: child plans → sub-issues; tasks → sub-issues of child ----
    const child_plans = try childPlansOf(allocator, d, anchor_plan_id);
    defer freePlanRows(child_plans, allocator);

    for (child_plans) |cp| {
        const cp_res = createOrSkipGithubIssue(
            allocator,
            d,
            client,
            opts,
            "plan",
            cp.id,
            parent.number,
            owner,
            repo,
            &.{},
            &results,
        ) catch {
            // Failure already recorded in results; continue with siblings.
            continue;
        };
        const tasks = try tasksUnderPlan(allocator, d, cp.id);
        defer freeTaskRows(tasks, allocator);
        for (tasks) |t| {
            _ = createOrSkipGithubIssue(
                allocator,
                d,
                client,
                opts,
                "task",
                t.id,
                cp_res.number,
                owner,
                repo,
                &.{},
                &results,
            ) catch continue;
        }
    }

    // ---- step 3: direct anchor tasks → sub-issues of parent ----
    const direct = try directTasksOf(allocator, d, anchor_plan_id);
    defer freeTaskRows(direct, allocator);
    for (direct) |t| {
        _ = createOrSkipGithubIssue(
            allocator,
            d,
            client,
            opts,
            "task",
            t.id,
            parent.number,
            owner,
            repo,
            &.{},
            &results,
        ) catch continue;
    }

    // ---- step 4: decisions → comments on parent (best-effort) ----
    if (!opts.dry_run and parent.external_id.len > 0) {
        postDecisionComments(allocator, d, client, anchor_plan_id, parent.external_id) catch {};
    }

    // Tally counts.
    for (results.items) |r| switch (r.op) {
        .created => report.created += 1,
        .skipped => report.skipped += 1,
        .failed => report.failed += 1,
    };
    report.results = try results.toOwnedSlice(allocator);
    return report;
}

/// propagateZeroRepo resolves the `github_lead_repo` config value (from the
/// per-association block in `~/.planar/config.toml`) and delegates to
/// `propagateParentIssueWithRepo`. Returns `BadConfig` when the value is
/// missing or malformed.
pub fn propagateZeroRepo(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    client: GhClient,
    anchor_plan_id: i64,
    lead_repo: []const u8,
    opts: Opts,
) Error!Report {
    if (lead_repo.len == 0) return Error.BadConfig;
    const slash = std.mem.indexOfScalar(u8, lead_repo, '/') orelse return Error.BadConfig;
    if (slash == 0 or slash + 1 >= lead_repo.len) return Error.BadConfig;
    const owner = lead_repo[0..slash];
    const repo = lead_repo[slash + 1 ..];
    if (owner.len == 0 or repo.len == 0) return Error.BadConfig;
    return try propagateParentIssueWithRepo(allocator, d, client, anchor_plan_id, owner, repo, opts);
}

// -----------------------------------------------------------------------------
// Sub-issue probe + cache
// -----------------------------------------------------------------------------

/// detectParentIssueSupport probes the sub-issue REST endpoint once per anchor
/// and caches the result in the anchor's `config_json.sub_issue_supported`.
/// Returns the cached value when present; otherwise calls `client.probeFn`
/// and writes the result.
pub fn detectParentIssueSupport(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    client: GhClient,
    owner: []const u8,
    repo: []const u8,
    anchor_plan_id: i64,
    system_id: i64,
) Error!bool {
    // Read cache.
    if (try readSubIssueSupportCache(allocator, d, anchor_plan_id, system_id)) |v| {
        return v;
    }
    // Probe.
    const supported: bool = blk: {
        client.probeFn(client.ctx, allocator, owner, repo) catch |e| {
            if (e == error.NotFound) break :blk false;
            // Any other error → treat as endpoint available (mirrors Go).
            break :blk true;
        };
        break :blk true;
    };
    // Best-effort write (no-op when anchor has no mirror link yet — the cache
    // is rewritten alongside the very first link insert below by
    // propagateParentIssueWithRepo's anchor-config update path).
    writeSubIssueSupportCache(allocator, d, anchor_plan_id, system_id, supported) catch {};
    return supported;
}

fn readSubIssueSupportCache(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    system_id: i64,
) Error!?bool {
    var stmt = d.prepare(
        \\select coalesce(config_json, '') from external_links
        \\where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return null;
    const raw = try stmt.columnTextAlloc(0, allocator);
    defer allocator.free(raw);
    if (raw.len == 0) return null;

    var parsed = std.json.parseFromSlice(std.json.Value, allocator, raw, .{}) catch return null;
    defer parsed.deinit();
    if (parsed.value != .object) return null;
    const v = parsed.value.object.get("sub_issue_supported") orelse return null;
    return switch (v) {
        .bool => |b| b,
        else => null,
    };
}

fn writeSubIssueSupportCache(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    system_id: i64,
    supported: bool,
) Error!void {
    var stmt = d.prepare(
        \\select coalesce(config_json, '{}') from external_links
        \\where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return; // no row yet; first link insert will carry the cache via the strategy block.

    const existing = try stmt.columnTextAlloc(0, allocator);
    defer allocator.free(existing);

    var parsed = std.json.parseFromSlice(std.json.Value, allocator, existing, .{}) catch null;
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    const w = &out.writer;

    try w.writeAll("{");
    var first = true;
    var wrote_supported = false;

    if (parsed) |*pp| {
        defer pp.deinit();
        if (pp.value == .object) {
            var it = pp.value.object.iterator();
            while (it.next()) |kv| {
                if (std.mem.eql(u8, kv.key_ptr.*, "sub_issue_supported")) {
                    if (!first) try w.writeAll(",");
                    first = false;
                    try w.writeAll("\"sub_issue_supported\":");
                    try w.writeAll(if (supported) "true" else "false");
                    wrote_supported = true;
                    continue;
                }
                if (!first) try w.writeAll(",");
                first = false;
                try writeJsonString(w, kv.key_ptr.*);
                try w.writeAll(":");
                var jsw: std.json.Stringify = .{ .writer = w, .options = .{} };
                try jsw.write(kv.value_ptr.*);
            }
        }
    }
    if (!wrote_supported) {
        if (!first) try w.writeAll(",");
        try w.writeAll("\"sub_issue_supported\":");
        try w.writeAll(if (supported) "true" else "false");
    }
    try w.writeAll("}");
    const merged = out.written();

    _ = d.execParams(
        \\update external_links set config_json = ?
        \\where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror'
    , &.{ .{ .text = merged }, .{ .int = anchor_plan_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
}

// -----------------------------------------------------------------------------
// createOrSkipGithubIssue — the core per-entity primitive
// -----------------------------------------------------------------------------

const PerEntityResult = struct {
    number: i64,
    external_id: []const u8,
};

/// createOrSkipGithubIssue creates a regular or sub-issue for `entity_kind:id`,
/// records the external_links row + sync_events, and appends a result row.
/// Returns the issue number + external_id for follow-on linking (sub-issues
/// of this issue). Returns an empty PerEntityResult on dry-run.
fn createOrSkipGithubIssue(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    client: GhClient,
    opts: Opts,
    entity_kind: []const u8,
    entity_id: i64,
    parent_number: i64,
    owner: []const u8,
    repo: []const u8,
    labels: []const []const u8,
    results: *std.ArrayList(EntityResult),
) Error!PerEntityResult {
    // Skip if already linked.
    if (try loadExistingMirror(allocator, d, entity_kind, entity_id, opts.sys_id)) |existing| {
        defer allocator.free(existing.external_id);
        defer allocator.free(existing.external_url);
        const title = (try entityTitleOpt(allocator, d, entity_kind, entity_id)) orelse try allocator.dupe(u8, "");
        try results.append(allocator, .{
            .entity_kind = try allocator.dupe(u8, entity_kind),
            .entity_id = entity_id,
            .title = title,
            .op = .skipped,
            .external_id = try allocator.dupe(u8, existing.external_id),
            .external_url = try allocator.dupe(u8, existing.external_url),
            .error_name = try allocator.dupe(u8, ""),
        });
        // Parse the issue number from external_id "owner/repo#N".
        const num = parseIssueNumberFromExternalID(existing.external_id) orelse 0;
        return .{ .number = num, .external_id = "" };
    }

    // Load title + body for the create.
    const local = entityForCreate(allocator, d, entity_kind, entity_id) catch |e| {
        try appendFailure(allocator, results, entity_kind, entity_id, "", e);
        return Error.QueryFailed;
    };
    defer allocator.free(local.title);
    defer allocator.free(local.body);

    if (opts.dry_run) {
        try results.append(allocator, .{
            .entity_kind = try allocator.dupe(u8, entity_kind),
            .entity_id = entity_id,
            .title = try allocator.dupe(u8, local.title),
            .op = .created,
            .external_id = try allocator.dupe(u8, "(dry-run)"),
            .external_url = try allocator.dupe(u8, ""),
            .error_name = try allocator.dupe(u8, ""),
        });
        return .{ .number = 0, .external_id = "" };
    }

    // Create issue via REST.
    const body_or_default = if (local.body.len == 0) "_No description provided._" else local.body;
    const created = client.createIssueFn(client.ctx, allocator, owner, repo, local.title, body_or_default, labels) catch |e| {
        try appendFailure(allocator, results, entity_kind, entity_id, local.title, e);
        return Error.QueryFailed;
    };
    defer allocator.free(created.node_id);

    // Link as sub-issue when requested.
    if (parent_number > 0) {
        client.linkSubIssueFn(client.ctx, allocator, owner, repo, parent_number, created.number) catch |e| {
            // Non-fatal: issue created but not linked. Mirrors Go behaviour.
            std.log.warn("sub-issue link failed (issue {d} created; continuing): {s}", .{ created.number, @errorName(e) });
        };
    }

    // Build external_id "owner/repo#N" and external_url.
    const external_id = try std.fmt.allocPrint(allocator, "{s}/{s}#{d}", .{ owner, repo, created.number });
    errdefer allocator.free(external_id);
    const external_url = try std.fmt.allocPrint(allocator, "https://github.com/{s}/{s}/issues/{d}", .{ owner, repo, created.number });
    errdefer allocator.free(external_url);

    // Record the external_links row + paired sync_events 'ok' row atomically.
    try recordLink(d, entity_kind, entity_id, opts.sys_id, external_id, external_url, opts.sync_direction);

    try results.append(allocator, .{
        .entity_kind = try allocator.dupe(u8, entity_kind),
        .entity_id = entity_id,
        .title = try allocator.dupe(u8, local.title),
        .op = .created,
        .external_id = external_id,
        .external_url = external_url,
        .error_name = try allocator.dupe(u8, ""),
    });
    return .{ .number = created.number, .external_id = external_id };
}

fn appendFailure(
    allocator: std.mem.Allocator,
    results: *std.ArrayList(EntityResult),
    entity_kind: []const u8,
    entity_id: i64,
    title: []const u8,
    err: anyerror,
) Error!void {
    try results.append(allocator, .{
        .entity_kind = try allocator.dupe(u8, entity_kind),
        .entity_id = entity_id,
        .title = try allocator.dupe(u8, title),
        .op = .failed,
        .external_id = try allocator.dupe(u8, ""),
        .external_url = try allocator.dupe(u8, ""),
        .error_name = try allocator.dupe(u8, @errorName(err)),
    });
}

// -----------------------------------------------------------------------------
// Read-side helpers (entity title/body, mirror link, tree walks)
// -----------------------------------------------------------------------------

const ExistingMirror = struct {
    external_id: []const u8,
    external_url: []const u8,
};

/// loadExistingMirror returns the (external_id, external_url) for the
/// existing mirror link or null when none exists. Caller owns both slices.
pub fn loadExistingMirror(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    entity_kind: []const u8,
    entity_id: i64,
    system_id: i64,
) Error!?ExistingMirror {
    var stmt = d.prepare(
        \\select external_id, coalesce(external_url, '') from external_links
        \\where entity_kind = ? and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .text = entity_kind }, .{ .int = entity_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return null;
    return .{
        .external_id = try stmt.columnTextAlloc(0, allocator),
        .external_url = try stmt.columnTextAlloc(1, allocator),
    };
}

const LocalCreate = struct {
    title: []const u8,
    body: []const u8,
};

/// entityForCreate loads title + body for the given entity. plans table has
/// columns `(title, summary)`; tasks table has `(title, body)`. We pick the
/// best body column for each kind.
fn entityForCreate(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    entity_kind: []const u8,
    entity_id: i64,
) Error!LocalCreate {
    if (std.mem.eql(u8, entity_kind, "plan")) {
        var stmt = d.prepare("select coalesce(title,''), coalesce(summary,'') from plans where id = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = entity_id }}) catch return Error.QueryFailed;
        const step = stmt.step() catch return Error.QueryFailed;
        if (step == .done) return Error.QueryFailed;
        return .{
            .title = try stmt.columnTextAlloc(0, allocator),
            .body = try stmt.columnTextAlloc(1, allocator),
        };
    } else if (std.mem.eql(u8, entity_kind, "task")) {
        var stmt = d.prepare("select coalesce(title,''), coalesce(body,'') from tasks where id = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = entity_id }}) catch return Error.QueryFailed;
        const step = stmt.step() catch return Error.QueryFailed;
        if (step == .done) return Error.QueryFailed;
        return .{
            .title = try stmt.columnTextAlloc(0, allocator),
            .body = try stmt.columnTextAlloc(1, allocator),
        };
    }
    return Error.QueryFailed;
}

pub fn entityTitleOpt(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    entity_kind: []const u8,
    entity_id: i64,
) Error!?[]const u8 {
    const sql: [:0]const u8 = if (std.mem.eql(u8, entity_kind, "plan"))
        "select coalesce(title,'') from plans where id = ?"
    else if (std.mem.eql(u8, entity_kind, "task"))
        "select coalesce(title,'') from tasks where id = ?"
    else
        return null;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = entity_id }}) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return null;
    return try stmt.columnTextAlloc(0, allocator);
}

pub const PlanRow = struct { id: i64, title: []const u8 };
pub const TaskRow = struct { id: i64, title: []const u8 };

pub fn freePlanRows(rows: []const PlanRow, allocator: std.mem.Allocator) void {
    for (rows) |r| allocator.free(r.title);
    allocator.free(rows);
}

pub fn freeTaskRows(rows: []const TaskRow, allocator: std.mem.Allocator) void {
    for (rows) |r| allocator.free(r.title);
    allocator.free(rows);
}

/// childPlansOf returns the direct child plans (parent_plan_id = anchor) in id order.
pub fn childPlansOf(allocator: std.mem.Allocator, d: *db.sqlite.Db, anchor_plan_id: i64) Error![]PlanRow {
    var out: std.ArrayList(PlanRow) = .empty;
    errdefer {
        for (out.items) |r| allocator.free(r.title);
        out.deinit(allocator);
    }
    var stmt = d.prepare("select id, coalesce(title,'') from plans where parent_plan_id = ? order by id") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
    while (true) {
        const step = stmt.step() catch return Error.QueryFailed;
        if (step == .done) break;
        const id = stmt.columnInt(0);
        const title = try stmt.columnTextAlloc(1, allocator);
        try out.append(allocator, .{ .id = id, .title = title });
    }
    return try out.toOwnedSlice(allocator);
}

/// tasksUnderPlan returns tasks attached to `plan_id` via either tasks.plan_id
/// or entity_links(task→plan, derives-from). Mirrors Go `tasksUnderPlan`.
pub fn tasksUnderPlan(allocator: std.mem.Allocator, d: *db.sqlite.Db, plan_id: i64) Error![]TaskRow {
    var out: std.ArrayList(TaskRow) = .empty;
    errdefer {
        for (out.items) |r| allocator.free(r.title);
        out.deinit(allocator);
    }
    var stmt = d.prepare(
        \\select id, title from (
        \\  select t.id, coalesce(t.title,'') as title from tasks t where t.plan_id = ?
        \\  union
        \\  select t.id, coalesce(t.title,'') as title from tasks t
        \\  join entity_links el on el.from_kind='task' and el.from_id=t.id
        \\  where el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
        \\) order by id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = plan_id }, .{ .int = plan_id } }) catch return Error.QueryFailed;
    while (true) {
        const step = stmt.step() catch return Error.QueryFailed;
        if (step == .done) break;
        const id = stmt.columnInt(0);
        const title = try stmt.columnTextAlloc(1, allocator);
        try out.append(allocator, .{ .id = id, .title = title });
    }
    return try out.toOwnedSlice(allocator);
}

/// directTasksOf returns tasks attached directly to the anchor (NOT via a
/// child plan). Mirrors `directTasksOf` in Go.
pub fn directTasksOf(allocator: std.mem.Allocator, d: *db.sqlite.Db, anchor_plan_id: i64) Error![]TaskRow {
    return try tasksUnderPlan(allocator, d, anchor_plan_id);
}

// -----------------------------------------------------------------------------
// Repo resolution
// -----------------------------------------------------------------------------

pub const RepoCoords = struct {
    owner: []const u8,
    repo: []const u8,
};

/// resolveTargetRepo returns the single owner/repo for a single-repo feature.
/// Walks `distinctReposInFeature` (the same recursive CTE the strategy
/// selector uses) and returns coords for the first repo. Returns
/// `NoTouchedRepos` when the feature has none, `CannotResolveRepo` when the
/// repo can't be mapped to a github owner/repo.
pub fn resolveTargetRepo(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
) Error!RepoCoords {
    const repos = try distinctReposInFeatureIds(allocator, d, anchor_plan_id);
    defer allocator.free(repos);
    if (repos.len == 0) return Error.NoTouchedRepos;
    return (try projectGitHubCoords(allocator, d, repos[0])) orelse Error.CannotResolveRepo;
}

fn distinctReposInFeatureIds(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
) Error![]i64 {
    var out: std.ArrayList(i64) = .empty;
    errdefer out.deinit(allocator);
    var stmt = d.prepare(
        \\with recursive plan_tree(id) as (
        \\  select ? union all
        \\  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id
        \\),
        \\tasks_in_tree as (
        \\  select t.id, t.scope_kind, t.scope_id
        \\  from tasks t join plan_tree pt on t.plan_id = pt.id
        \\  union
        \\  select t.id, t.scope_kind, t.scope_id
        \\  from tasks t
        \\  join entity_links el on el.from_kind = 'task' and el.from_id = t.id
        \\                       and el.to_kind = 'plan' and el.relationship = 'derives-from'
        \\  join plan_tree pt on el.to_id = pt.id
        \\),
        \\task_repos as (
        \\  select scope_id as repo_id from tasks_in_tree where scope_kind = 'repo'
        \\  union
        \\  select el2.to_id as repo_id
        \\  from tasks_in_tree tit
        \\  join entity_links el2 on el2.from_kind = 'task' and el2.from_id = tit.id
        \\                        and el2.to_kind = 'repo' and el2.relationship = 'touches'
        \\)
        \\select distinct repo_id from task_repos where repo_id is not null order by repo_id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
    while (true) {
        const step = stmt.step() catch return Error.QueryFailed;
        if (step == .done) break;
        try out.append(allocator, stmt.columnInt(0));
    }
    return try out.toOwnedSlice(allocator);
}

/// projectGitHubCoords resolves a projects.id to (owner, repo), preferring
/// the git_remote column and falling back to the slug parsed as "owner/repo".
/// Returns null when the project row is missing or unresolvable. Caller owns
/// both returned slices.
pub fn projectGitHubCoords(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    project_id: i64,
) Error!?RepoCoords {
    var stmt = d.prepare("select slug, coalesce(git_remote,'') from projects where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = project_id }}) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return null;
    const slug = try stmt.columnTextAlloc(0, allocator);
    defer allocator.free(slug);
    const git_remote = try stmt.columnTextAlloc(1, allocator);
    defer allocator.free(git_remote);

    if (parseGitHubRepo(git_remote)) |parsed| {
        return RepoCoords{
            .owner = try allocator.dupe(u8, parsed.owner),
            .repo = try allocator.dupe(u8, parsed.repo),
        };
    }

    // Fall back to slug-as-owner/repo (fixtures).
    if (std.mem.indexOfScalar(u8, slug, '/')) |i| {
        const o = slug[0..i];
        const r = slug[i + 1 ..];
        if (o.len > 0 and r.len > 0 and std.mem.indexOfScalar(u8, r, '/') == null) {
            return RepoCoords{
                .owner = try allocator.dupe(u8, o),
                .repo = try allocator.dupe(u8, r),
            };
        }
    }
    return null;
}

/// parseGitHubRepo extracts owner+repo from a GitHub git remote URL. Returns
/// null for non-GitHub or malformed input. Mirrors Go `parseGitHubRepo` in
/// `github_remote.go`.
pub fn parseGitHubRepo(git_remote: []const u8) ?struct { owner: []const u8, repo: []const u8 } {
    if (git_remote.len == 0) return null;
    var path: []const u8 = undefined;
    if (std.mem.startsWith(u8, git_remote, "git@github.com:")) {
        path = git_remote["git@github.com:".len..];
    } else if (std.mem.startsWith(u8, git_remote, "https://github.com/")) {
        path = git_remote["https://github.com/".len..];
    } else if (std.mem.startsWith(u8, git_remote, "ssh://git@github.com/")) {
        path = git_remote["ssh://git@github.com/".len..];
    } else {
        return null;
    }
    while (path.len > 0 and path[path.len - 1] == '/') path = path[0 .. path.len - 1];
    if (std.mem.endsWith(u8, path, ".git")) path = path[0 .. path.len - 4];
    const slash = std.mem.indexOfScalar(u8, path, '/') orelse return null;
    if (slash == 0 or slash + 1 >= path.len) return null;
    const owner = path[0..slash];
    const repo = path[slash + 1 ..];
    if (owner.len == 0 or repo.len == 0) return null;
    if (std.mem.indexOfScalar(u8, repo, '/') != null) return null;
    return .{ .owner = owner, .repo = repo };
}

// -----------------------------------------------------------------------------
// recordLink (external_links + sync_events 'ok')
// -----------------------------------------------------------------------------

/// recordLink inserts an external_links row and the paired sync_events 'ok'
/// row inside a single transaction. Mirrors Go `recordLink` in jira.go.
pub fn recordLink(
    d: *db.sqlite.Db,
    entity_kind: []const u8,
    entity_id: i64,
    system_id: i64,
    external_id: []const u8,
    external_url: []const u8,
    sync_direction: link_mod.SyncDirection,
) Error!void {
    d.exec("begin immediate") catch return Error.QueryFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    const link_id = d.execParams(
        \\insert into external_links
        \\  (entity_kind, entity_id, system_id, external_id, external_url,
        \\   link_role, sync_direction, last_sync_status)
        \\values (?, ?, ?, ?, ?, 'mirror', ?, 'ok')
    , &.{
        .{ .text = entity_kind },
        .{ .int = entity_id },
        .{ .int = system_id },
        .{ .text = external_id },
        if (external_url.len == 0) .{ .null = {} } else .{ .text = external_url },
        .{ .text = sync_direction.toText() },
    }) catch return Error.QueryFailed;

    _ = d.execParams(
        "insert into sync_events (link_id, direction, outcome) values (?, 'push', 'ok')",
        &.{.{ .int = link_id }},
    ) catch return Error.QueryFailed;

    d.exec("commit") catch return Error.QueryFailed;
    committed = true;
}

// -----------------------------------------------------------------------------
// Decision comments
// -----------------------------------------------------------------------------

/// postDecisionComments posts every decision linked to the anchor plan via
/// `derives-from` as a comment on the parent issue. Errors are logged and the
/// loop continues. Mirrors Go `postGithubDecisionComments`.
fn postDecisionComments(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    client: GhClient,
    anchor_plan_id: i64,
    parent_external_id: []const u8,
) Error!void {
    var stmt = d.prepare(
        \\select d.id, coalesce(d.title,''), coalesce(d.body,'')
        \\from decisions d
        \\join entity_links el on el.from_kind = 'decision' and el.from_id = d.id
        \\                     and el.to_kind = 'plan' and el.to_id = ?
        \\                     and el.relationship = 'derives-from'
        \\order by d.id
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = anchor_plan_id }}) catch return Error.QueryFailed;
    while (true) {
        const step = stmt.step() catch return Error.QueryFailed;
        if (step == .done) break;
        const id = stmt.columnInt(0);
        _ = id;
        const title = try stmt.columnTextAlloc(1, allocator);
        defer allocator.free(title);
        const body = try stmt.columnTextAlloc(2, allocator);
        defer allocator.free(body);
        const comment = try std.fmt.allocPrint(allocator, "**Decision: {s}**\n\n{s}", .{ title, body });
        defer allocator.free(comment);
        client.postCommentFn(client.ctx, allocator, parent_external_id, comment) catch |e| {
            std.log.warn("decision comment failed (non-fatal): {s}", .{@errorName(e)});
        };
    }
}

// -----------------------------------------------------------------------------
// misc helpers
// -----------------------------------------------------------------------------

fn parseIssueNumberFromExternalID(external_id: []const u8) ?i64 {
    const hash = std.mem.lastIndexOfScalar(u8, external_id, '#') orelse return null;
    if (hash + 1 >= external_id.len) return null;
    return std.fmt.parseInt(i64, external_id[hash + 1 ..], 10) catch null;
}

fn writeJsonString(w: *std.Io.Writer, s: []const u8) !void {
    var jsw: std.json.Stringify = .{ .writer = w, .options = .{} };
    try jsw.write(s);
}

// -----------------------------------------------------------------------------
// tests
// -----------------------------------------------------------------------------

const testing = std.testing;

test "parseGitHubRepo accepts ssh, https, ssh-explicit forms" {
    {
        const p = parseGitHubRepo("git@github.com:acme/api.git").?;
        try testing.expectEqualStrings("acme", p.owner);
        try testing.expectEqualStrings("api", p.repo);
    }
    {
        const p = parseGitHubRepo("https://github.com/acme/api").?;
        try testing.expectEqualStrings("acme", p.owner);
        try testing.expectEqualStrings("api", p.repo);
    }
    {
        const p = parseGitHubRepo("ssh://git@github.com/acme/api.git/").?;
        try testing.expectEqualStrings("acme", p.owner);
        try testing.expectEqualStrings("api", p.repo);
    }
    try testing.expect(parseGitHubRepo("https://gitlab.com/acme/api") == null);
    try testing.expect(parseGitHubRepo("") == null);
    try testing.expect(parseGitHubRepo("git@github.com:acme") == null);
    try testing.expect(parseGitHubRepo("https://github.com/acme/api/extra") == null);
}

fn setupDb() !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    try @import("db").migrate.applyAll(&d, testing.allocator);
    return d;
}

test "projectGitHubCoords prefers git_remote, falls back to slug" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/api','acme/api','https://github.com/foo/bar.git')", &.{});
    {
        const c = (try projectGitHubCoords(testing.allocator, &d, 1)).?;
        defer testing.allocator.free(c.owner);
        defer testing.allocator.free(c.repo);
        try testing.expectEqualStrings("foo", c.owner);
        try testing.expectEqualStrings("bar", c.repo);
    }
    _ = try d.execParams("insert into projects (slug,name) values ('o2/r2','o2')", &.{});
    {
        const c = (try projectGitHubCoords(testing.allocator, &d, 2)).?;
        defer testing.allocator.free(c.owner);
        defer testing.allocator.free(c.repo);
        try testing.expectEqualStrings("o2", c.owner);
        try testing.expectEqualStrings("r2", c.repo);
    }
}

test "recordLink inserts external_links + sync_events 'ok' atomically" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues','gh','','','token-env','X')",
        &.{},
    );
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','p','p')", &.{});
    try recordLink(&d, "plan", 1, 1, "o/r#1", "https://github.com/o/r/issues/1", .@"two-way");
    try testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from external_links where entity_kind='plan'"));
    try testing.expectEqual(@as(i64, 1), try d.intQuery("select count(*) from sync_events where outcome='ok'"));
}

// ---- fake client + end-to-end engine test ----------------------------------

const FakeClient = struct {
    issue_counter: i64 = 0,
    creates: usize = 0,
    links: usize = 0,
    probe_supported: bool = true,

    fn probe(ctx: *anyopaque, _: std.mem.Allocator, _: []const u8, _: []const u8) anyerror!void {
        const self: *FakeClient = @ptrCast(@alignCast(ctx));
        if (!self.probe_supported) return error.NotFound;
    }

    fn createIssue(
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        _: []const u8,
        _: []const u8,
        _: []const u8,
        _: []const u8,
        _: []const []const u8,
    ) anyerror!CreatedIssue {
        const self: *FakeClient = @ptrCast(@alignCast(ctx));
        self.issue_counter += 1;
        self.creates += 1;
        const node_id = try std.fmt.allocPrint(allocator, "N{d}", .{self.issue_counter});
        return .{ .number = self.issue_counter, .node_id = node_id };
    }

    fn linkSub(
        ctx: *anyopaque,
        _: std.mem.Allocator,
        _: []const u8,
        _: []const u8,
        _: i64,
        _: i64,
    ) anyerror!void {
        const self: *FakeClient = @ptrCast(@alignCast(ctx));
        self.links += 1;
    }

    fn postComment(
        _: *anyopaque,
        _: std.mem.Allocator,
        _: []const u8,
        _: []const u8,
    ) anyerror!void {}

    fn client(self: *FakeClient) GhClient {
        return .{
            .ctx = self,
            .probeFn = probe,
            .createIssueFn = createIssue,
            .linkSubIssueFn = linkSub,
            .postCommentFn = postComment,
        };
    }
};

test "propagateParentIssueWithRepo creates anchor + child + tasks, second run skips all" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues','gh','','','token-env','X')",
        &.{},
    );
    // 1 anchor plan, 1 child plan, 2 tasks attached to child.
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    _ = try d.execParams("insert into plans (scope_kind, title, slug, parent_plan_id) values ('global','child','c',1)", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, title, plan_id) values ('global','t1',2)", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, title, plan_id) values ('global','t2',2)", &.{});

    var fake = FakeClient{};
    var report = try propagateParentIssueWithRepo(
        testing.allocator,
        &d,
        fake.client(),
        1,
        "acme",
        "checkout",
        .{ .sys_id = 1, .sys_slug = "gh" },
    );
    defer report.deinit(testing.allocator);

    try testing.expectEqual(@as(usize, 4), report.created);
    try testing.expectEqual(@as(usize, 0), report.skipped);
    try testing.expectEqual(@as(usize, 0), report.failed);
    try testing.expectEqual(@as(usize, 4), fake.creates);
    // 1 (child→anchor) + 2 (tasks→child) = 3 sub-issue link calls.
    try testing.expectEqual(@as(usize, 3), fake.links);
    try testing.expectEqual(@as(i64, 4), try d.intQuery("select count(*) from external_links where link_role='mirror'"));
    try testing.expectEqual(@as(i64, 4), try d.intQuery("select count(*) from sync_events where outcome='ok'"));
    // Anchor config_json carries the strategy + parent coords.
    var stmt = try d.prepare("select config_json from external_links where entity_kind='plan' and entity_id=1");
    defer stmt.finalize();
    _ = try stmt.step();
    const cfg = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(cfg);
    try testing.expect(std.mem.indexOf(u8, cfg, "\"strategy\":\"github-parent-issue\"") != null);
    try testing.expect(std.mem.indexOf(u8, cfg, "\"parent_issue_repo\":\"acme/checkout\"") != null);
    try testing.expect(std.mem.indexOf(u8, cfg, "\"parent_issue_num\":1") != null);

    // Second run: all 4 skipped, no new REST calls.
    var fake2 = FakeClient{};
    var report2 = try propagateParentIssueWithRepo(
        testing.allocator,
        &d,
        fake2.client(),
        1,
        "acme",
        "checkout",
        .{ .sys_id = 1, .sys_slug = "gh" },
    );
    defer report2.deinit(testing.allocator);

    try testing.expectEqual(@as(usize, 0), report2.created);
    try testing.expectEqual(@as(usize, 4), report2.skipped);
    try testing.expectEqual(@as(usize, 0), fake2.creates);
    try testing.expectEqual(@as(usize, 0), fake2.links);
}

test "propagateParentIssueWithRepo dry_run does not contact remote and creates no links" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues','gh','','','token-env','X')",
        &.{},
    );
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    _ = try d.execParams("insert into plans (scope_kind, title, slug, parent_plan_id) values ('global','child','c',1)", &.{});

    var fake = FakeClient{};
    var report = try propagateParentIssueWithRepo(
        testing.allocator,
        &d,
        fake.client(),
        1,
        "acme",
        "x",
        .{ .sys_id = 1, .sys_slug = "gh", .dry_run = true },
    );
    defer report.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 0), fake.creates);
    try testing.expectEqual(@as(i64, 0), try d.intQuery("select count(*) from external_links"));
    try testing.expectEqual(@as(usize, 2), report.created);
}

test "propagateParentIssueWithRepo returns SubIssueUnsupported when probe 404s" {
    var d = try setupDb();
    defer d.close();
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues','gh','','','token-env','X')",
        &.{},
    );
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});

    var fake = FakeClient{ .probe_supported = false };
    try testing.expectError(Error.SubIssueUnsupported, propagateParentIssueWithRepo(
        testing.allocator,
        &d,
        fake.client(),
        1,
        "o",
        "r",
        .{ .sys_id = 1, .sys_slug = "gh" },
    ));
}

test "propagateZeroRepo rejects empty and malformed lead_repo" {
    var d = try setupDb();
    defer d.close();
    var fake = FakeClient{};
    try testing.expectError(Error.BadConfig, propagateZeroRepo(testing.allocator, &d, fake.client(), 1, "", .{ .sys_id = 1, .sys_slug = "gh" }));
    try testing.expectError(Error.BadConfig, propagateZeroRepo(testing.allocator, &d, fake.client(), 1, "no-slash", .{ .sys_id = 1, .sys_slug = "gh" }));
    try testing.expectError(Error.BadConfig, propagateZeroRepo(testing.allocator, &d, fake.client(), 1, "/missing-owner", .{ .sys_id = 1, .sys_slug = "gh" }));
    try testing.expectError(Error.BadConfig, propagateZeroRepo(testing.allocator, &d, fake.client(), 1, "owner/", .{ .sys_id = 1, .sys_slug = "gh" }));
}
