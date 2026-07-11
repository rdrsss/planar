//! engine/extsync/projects_v2 — GitHub `projects-v2` propagation strategy.
//!
//! Mirrors Go `internal/extsync/github.go::propagateProjectsV2` for the
//! multi-repo Projects v2 flow:
//!
//!   - Anchor plan → ProjectsV2 project (org-level first, user-level fallback).
//!   - Child plans → issues in their lead repo + added as Project items.
//!   - Tasks under a child plan → issues in the task's touched/scoped repo +
//!     added as Project items + optional `Parent` field set to the child
//!     plan's `owner/repo#N`.
//!   - Anchor-direct tasks → issues + added as Project items.
//!
//! Idempotency: the anchor's mirror link carries `config_json.project_node_id`
//! + `config_json.project_location`. On re-runs the project is reused
//! (skipped). Each entity's mirror link is reused via `loadExistingMirror`.
//!
//! Caller injects a `GhProjectsClient` of function pointers so unit tests can
//! drive the engine without HTTP. The CLI handler in `handlers/ext/propagate.zig`
//! wraps the real `engine.extsync.github.GithubAdapter` in a bridge.

const std = @import("std");
const db = @import("db");
const link_mod = @import("../external/link.zig");
const parent_issue = @import("parent_issue.zig");
const adapter = @import("github.zig");

// -----------------------------------------------------------------------------
// Public shape — Op, EntityResult, Report (compatible with parent_issue)
// -----------------------------------------------------------------------------

/// Op is the per-entity outcome the engine reports.
pub const Op = enum { created, skipped, failed };

/// EntityResult is one row in the report's results slice. All slices are
/// allocator-owned and freed by `Report.deinit`.
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
    /// Effective strategy after fallback resolution.
    strategy: []const u8 = "github-projects-v2",
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

/// Opts captures the subset of Go PropagateOpts the projects-v2 path needs.
///
/// `parent_field_names` is the operator-configured list of candidate field
/// names (e.g. `Parent`, `Initiative`, `Tracking`). The first match (case
/// insensitive) becomes the Project field whose value gets set to each task's
/// child-plan coordinates. When the list is empty, the default
/// `["Parent","Initiative","Tracking"]` is used.
pub const Opts = struct {
    sys_id: i64,
    sys_slug: []const u8,
    dry_run: bool = false,
    sync_direction: link_mod.SyncDirection = .@"two-way",
    parent_field_names: []const []const u8 = &.{},
};

/// CreatedIssue mirrors the parent_issue counterpart so bridge code can use
/// the same shape from either engine.
pub const CreatedIssue = struct {
    number: i64,
    node_id: []const u8,
};

/// CreatedProject is the GraphQL `createProjectV2` response shape. Caller owns
/// both slices.
pub const CreatedProject = struct {
    node_id: []const u8,
    url: []const u8,
};

/// ProjectField mirrors the GraphQL `ProjectV2.fields.nodes` shape. Caller
/// owns every slice.
pub const ProjectField = struct {
    id: []const u8,
    name: []const u8,
    data_type: []const u8,
};

/// GhProjectsClient is the adapter surface this engine needs. Function
/// pointers let unit tests inject fakes; the real CLI handler wraps a
/// `GithubAdapter`. Five callbacks cover the GraphQL surface plus issue
/// creation; the `createIssue` callback also handles the REST issue create
/// that returns `node_id` (which the project's `addProjectV2ItemById`
/// mutation needs).
pub const GhProjectsClient = struct {
    ctx: *anyopaque,

    /// getAuthenticatedOwnerFn returns `(user_node_id, org_ids)`. Caller owns
    /// every slice.
    getAuthenticatedOwnerFn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
    ) anyerror!AuthenticatedOwner,

    /// createProjectV2Fn creates a Projects v2 project at the given owner
    /// (org or user node id). Returns `(node_id, url)`.
    createProjectV2Fn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        owner_node_id: []const u8,
        title: []const u8,
    ) anyerror!CreatedProject,

    /// getProjectV2FieldsFn returns the project's field definitions. The
    /// engine releases the slice via `freeProjectFields`.
    getProjectV2FieldsFn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
    ) anyerror![]ProjectField,

    /// addProjectV2ItemFn adds a content (issue) node id as a project item.
    /// Returns the project item id (allocator-owned).
    addProjectV2ItemFn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
        content_node_id: []const u8,
    ) anyerror![]const u8,

    /// setProjectV2ItemFieldValueFn sets a TEXT-typed field value on a
    /// project item. Non-text fields are not supported (matches Go).
    setProjectV2ItemFieldValueFn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
        item_id: []const u8,
        field_id: []const u8,
        text_value: []const u8,
    ) anyerror!void,

    /// createIssueFn creates a regular issue. Caller owns `node_id` (may be "").
    createIssueFn: *const fn (
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
        title: []const u8,
        body: []const u8,
        labels: []const []const u8,
    ) anyerror!CreatedIssue,
};

/// AuthenticatedOwner mirrors the GraphQL viewer envelope. Caller owns every
/// allocated slice.
pub const AuthenticatedOwner = struct {
    user_node_id: []const u8,
    org_ids: []const []const u8,
};

pub fn freeAuthenticatedOwner(owner: AuthenticatedOwner, allocator: std.mem.Allocator) void {
    allocator.free(owner.user_node_id);
    for (owner.org_ids) |id| allocator.free(id);
    allocator.free(owner.org_ids);
}

pub fn freeProjectFields(fields: []ProjectField, allocator: std.mem.Allocator) void {
    for (fields) |f| {
        allocator.free(f.id);
        allocator.free(f.name);
        allocator.free(f.data_type);
    }
    allocator.free(fields);
}

pub const Error = error{
    NoTouchedRepos,
    CannotResolveRepo,
    ProjectCreateFailed,
    QueryFailed,
    BadConfig,
    WriteFailed,
    /// Re-exported from parent_issue.Error so `try` against shared helpers
    /// (loadExistingMirror, childPlansOf, …) propagates cleanly.
    SubIssueUnsupported,
} || std.mem.Allocator.Error;

// -----------------------------------------------------------------------------
// Public entry point
// -----------------------------------------------------------------------------

/// propagateProjectsV2 runs the multi-repo Projects v2 flow for
/// `anchor_plan_id`. Creates (or reuses) a project, walks the plan tree,
/// creates issues in each entity's repo, adds them as Project items, and
/// optionally sets the `Parent` field on task items.
///
/// Returns `NoTouchedRepos` when the feature has no touched repos at all
/// (caller should fall back to zero-repo / tracking-issue).
pub fn propagateProjectsV2(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    client: GhProjectsClient,
    anchor_plan_id: i64,
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

    // ---- step 1: anchor plan → Project (create or reuse) ----
    // Use the local mirror of parent_issue.entityForCreate for the title.
    const anchor_local = parent_issue_entityForCreate(allocator, d, "plan", anchor_plan_id) catch return Error.QueryFailed;
    defer allocator.free(anchor_local.body);
    defer allocator.free(anchor_local.title);
    const anchor_title: []const u8 = anchor_local.title;

    var project_node_id: []const u8 = "";
    var project_url: []const u8 = "";
    var project_location: []const u8 = "";
    // We own project_node_id, project_url, project_location when they came
    // from create or from the cache lookup. We free them at the end.
    var owns_project_node = false;
    var owns_project_url = false;
    var owns_project_location = false;
    defer if (owns_project_node) allocator.free(project_node_id);
    defer if (owns_project_url) allocator.free(project_url);
    defer if (owns_project_location) allocator.free(project_location);

    const existing_anchor = try parent_issue.loadExistingMirror(allocator, d, "plan", anchor_plan_id, opts.sys_id);
    if (existing_anchor) |em| {
        defer allocator.free(em.external_id);
        defer allocator.free(em.external_url);
        // Recover project_node_id from anchor's config_json.
        const recovered = try readProjectConfigJSON(allocator, d, anchor_plan_id, opts.sys_id);
        project_node_id = recovered.node_id;
        project_location = recovered.location;
        owns_project_node = recovered.node_id.len > 0;
        owns_project_location = recovered.location.len > 0;
        project_url = try allocator.dupe(u8, em.external_url);
        owns_project_url = true;

        // Append a "skipped" result row for the anchor.
        try results.append(allocator, .{
            .entity_kind = try allocator.dupe(u8, "plan"),
            .entity_id = anchor_plan_id,
            .title = try allocator.dupe(u8, anchor_title),
            .op = .skipped,
            .external_id = try allocator.dupe(u8, em.external_id),
            .external_url = try allocator.dupe(u8, em.external_url),
            .error_name = try allocator.dupe(u8, ""),
        });
    } else if (opts.dry_run) {
        // No remote calls; record a synthetic created result.
        try results.append(allocator, .{
            .entity_kind = try allocator.dupe(u8, "plan"),
            .entity_id = anchor_plan_id,
            .title = try allocator.dupe(u8, anchor_title),
            .op = .created,
            .external_id = try allocator.dupe(u8, "(dry-run)"),
            .external_url = try allocator.dupe(u8, ""),
            .error_name = try allocator.dupe(u8, ""),
        });
        project_node_id = "(dry-run)";
        project_location = "dry-run";
    } else {
        // Create the project (org-first, user fallback).
        const created = createProject(allocator, client, anchor_title) catch |e| {
            try appendFailure(allocator, &results, "plan", anchor_plan_id, anchor_title, e);
            return Error.ProjectCreateFailed;
        };
        project_node_id = created.node_id;
        owns_project_node = true;
        project_url = created.url;
        owns_project_url = true;
        project_location = created.location;
        owns_project_location = true;

        // Record the anchor's external_links row with config_json carrying
        // the strategy + project coords.
        const cfg_json = try std.fmt.allocPrint(
            allocator,
            "{{\"strategy\":\"github-projects-v2\",\"project_node_id\":\"{s}\",\"project_location\":\"{s}\"}}",
            .{ project_node_id, project_location },
        );
        defer allocator.free(cfg_json);
        recordLinkWithConfigJSON(
            d,
            "plan",
            anchor_plan_id,
            opts.sys_id,
            project_node_id,
            project_url,
            opts.sync_direction,
            cfg_json,
        ) catch |e| {
            try appendFailure(allocator, &results, "plan", anchor_plan_id, anchor_title, e);
            return Error.WriteFailed;
        };

        try results.append(allocator, .{
            .entity_kind = try allocator.dupe(u8, "plan"),
            .entity_id = anchor_plan_id,
            .title = try allocator.dupe(u8, anchor_title),
            .op = .created,
            .external_id = try allocator.dupe(u8, project_node_id),
            .external_url = try allocator.dupe(u8, project_url),
            .error_name = try allocator.dupe(u8, ""),
        });
    }

    // ---- step 2: discover the Project's Parent field (best-effort) ----
    var parent_field_id_opt: ?[]const u8 = null;
    defer if (parent_field_id_opt) |id| allocator.free(id);
    if (!opts.dry_run and project_node_id.len > 0 and !std.mem.eql(u8, project_node_id, "(dry-run)")) {
        parent_field_id_opt = discoverParentField(allocator, client, project_node_id, opts.parent_field_names) catch null;
    }

    // ---- step 3: walk child plans + tasks ----
    const fallback = firstTouchedRepo(allocator, d, anchor_plan_id) catch RepoSlug{ .owner = "", .repo = "" };
    defer {
        if (fallback.owner.len > 0) allocator.free(fallback.owner);
        if (fallback.repo.len > 0) allocator.free(fallback.repo);
    }

    const child_plans = parent_issue.childPlansOf(allocator, d, anchor_plan_id) catch &[_]parent_issue.PlanRow{};
    defer parent_issue.freePlanRows(child_plans, allocator);

    for (child_plans) |cp| {
        // Resolve the child plan's repo; fall back to first-touched.
        const cp_repo = repoForPlanOrFallback(allocator, d, cp.id, fallback) catch RepoSlug{ .owner = "", .repo = "" };
        defer {
            if (cp_repo.owner.len > 0) allocator.free(cp_repo.owner);
            if (cp_repo.repo.len > 0) allocator.free(cp_repo.repo);
        }
        if (cp_repo.owner.len == 0 or cp_repo.repo.len == 0) {
            try appendFailure(allocator, &results, "plan", cp.id, cp.title, error.CannotResolveRepo);
            continue;
        }

        const cp_outcome = createOrSkipIssueForProject(
            allocator,
            d,
            client,
            opts,
            "plan",
            cp.id,
            cp_repo.owner,
            cp_repo.repo,
            project_node_id,
            &results,
        ) catch continue;
        defer if (cp_outcome.item_id.len > 0) allocator.free(cp_outcome.item_id);

        // Tasks under this child plan.
        const tasks = parent_issue.tasksUnderPlan(allocator, d, cp.id) catch &[_]parent_issue.TaskRow{};
        defer parent_issue.freeTaskRows(tasks, allocator);
        for (tasks) |t| {
            const t_repo = repoForTaskOrFallback(allocator, d, t.id, cp_repo) catch cp_repo;
            // Only free t_repo allocations when distinct from cp_repo (the
            // fallback returns the cp_repo borrowed slices). Distinguish by
            // checking pointer equality on owner.
            const t_owns = t_repo.owner.ptr != cp_repo.owner.ptr;
            defer if (t_owns) {
                if (t_repo.owner.len > 0) allocator.free(t_repo.owner);
                if (t_repo.repo.len > 0) allocator.free(t_repo.repo);
            };
            if (t_repo.owner.len == 0 or t_repo.repo.len == 0) {
                try appendFailure(allocator, &results, "task", t.id, t.title, error.CannotResolveRepo);
                continue;
            }

            const t_outcome = createOrSkipIssueForProject(
                allocator,
                d,
                client,
                opts,
                "task",
                t.id,
                t_repo.owner,
                t_repo.repo,
                project_node_id,
                &results,
            ) catch continue;
            defer if (t_outcome.item_id.len > 0) allocator.free(t_outcome.item_id);

            // Set Parent field on the task item if both:
            // (a) we have a parent field id, and
            // (b) the child plan was created/skipped with a known number.
            if (!opts.dry_run and parent_field_id_opt != null and t_outcome.item_id.len > 0 and cp_outcome.number > 0) {
                const parent_val = std.fmt.allocPrint(
                    allocator,
                    "{s}/{s}#{d}",
                    .{ cp_repo.owner, cp_repo.repo, cp_outcome.number },
                ) catch continue;
                defer allocator.free(parent_val);
                client.setProjectV2ItemFieldValueFn(
                    client.ctx,
                    allocator,
                    project_node_id,
                    t_outcome.item_id,
                    parent_field_id_opt.?,
                    parent_val,
                ) catch |e| {
                    std.log.warn("set Parent field on task {d} in project: {s}", .{ t.id, @errorName(e) });
                };
            }
        }
    }

    // ---- step 4: direct anchor tasks ----
    const direct = parent_issue.directTasksOf(allocator, d, anchor_plan_id) catch &[_]parent_issue.TaskRow{};
    defer parent_issue.freeTaskRows(direct, allocator);
    for (direct) |t| {
        const t_repo = repoForTaskOrFallback(allocator, d, t.id, fallback) catch fallback;
        const t_owns = t_repo.owner.ptr != fallback.owner.ptr;
        defer if (t_owns) {
            if (t_repo.owner.len > 0) allocator.free(t_repo.owner);
            if (t_repo.repo.len > 0) allocator.free(t_repo.repo);
        };
        if (t_repo.owner.len == 0 or t_repo.repo.len == 0) {
            try appendFailure(allocator, &results, "task", t.id, t.title, error.CannotResolveRepo);
            continue;
        }
        const dt_outcome = createOrSkipIssueForProject(
            allocator,
            d,
            client,
            opts,
            "task",
            t.id,
            t_repo.owner,
            t_repo.repo,
            project_node_id,
            &results,
        ) catch continue;
        if (dt_outcome.item_id.len > 0) allocator.free(dt_outcome.item_id);
    }

    // Tally.
    for (results.items) |r| switch (r.op) {
        .created => report.created += 1,
        .skipped => report.skipped += 1,
        .failed => report.failed += 1,
    };
    report.results = try results.toOwnedSlice(allocator);
    return report;
}

// -----------------------------------------------------------------------------
// Project creation (org-first, user fallback)
// -----------------------------------------------------------------------------

const CreatedProjectFull = struct {
    node_id: []const u8,
    url: []const u8,
    location: []const u8, // "org" or "user"
};

/// createProject calls `getAuthenticatedOwner` and prefers the first org id;
/// on failure (or no orgs) falls back to the user id. Mirrors Go
/// `(e *Engine).createProject`.
fn createProject(
    allocator: std.mem.Allocator,
    client: GhProjectsClient,
    title: []const u8,
) anyerror!CreatedProjectFull {
    const owner = client.getAuthenticatedOwnerFn(client.ctx, allocator) catch |e| {
        std.log.debug("getAuthenticatedOwner failed; trying user fallback: {s}", .{@errorName(e)});
        return error.ProjectCreateFailed;
    };
    defer freeAuthenticatedOwner(owner, allocator);

    if (owner.org_ids.len > 0) {
        const created = client.createProjectV2Fn(client.ctx, allocator, owner.org_ids[0], title) catch |e| {
            std.log.debug("org-level project create failed; falling back to user: {s}", .{@errorName(e)});
            // Fall through to user-level.
            const created2 = try client.createProjectV2Fn(client.ctx, allocator, owner.user_node_id, title);
            return .{
                .node_id = created2.node_id,
                .url = created2.url,
                .location = try allocator.dupe(u8, "user"),
            };
        };
        return .{
            .node_id = created.node_id,
            .url = created.url,
            .location = try allocator.dupe(u8, "org"),
        };
    }

    if (owner.user_node_id.len == 0) return error.ProjectCreateFailed;
    const created = try client.createProjectV2Fn(client.ctx, allocator, owner.user_node_id, title);
    return .{
        .node_id = created.node_id,
        .url = created.url,
        .location = try allocator.dupe(u8, "user"),
    };
}

// -----------------------------------------------------------------------------
// Parent field discovery
// -----------------------------------------------------------------------------

/// discoverParentField asks the Project for its fields and returns the id of
/// the first field whose name matches (case-insensitive) any candidate. When
/// `candidates` is empty, the default set `["Parent","Initiative","Tracking"]`
/// is used. Returns an allocator-owned slice or null if no match.
fn discoverParentField(
    allocator: std.mem.Allocator,
    client: GhProjectsClient,
    project_node_id: []const u8,
    candidates: []const []const u8,
) anyerror!?[]const u8 {
    const default_candidates = [_][]const u8{ "Parent", "Initiative", "Tracking" };
    const eff_candidates: []const []const u8 = if (candidates.len > 0) candidates else &default_candidates;

    const fields = client.getProjectV2FieldsFn(client.ctx, allocator, project_node_id) catch |e| {
        std.log.debug("getProjectV2Fields failed (non-fatal): {s}", .{@errorName(e)});
        return null;
    };
    defer freeProjectFields(fields, allocator);

    for (eff_candidates) |cand| {
        for (fields) |f| {
            if (std.ascii.eqlIgnoreCase(f.name, cand)) {
                return try allocator.dupe(u8, f.id);
            }
        }
    }
    return null;
}

// -----------------------------------------------------------------------------
// createOrSkipIssueForProject
// -----------------------------------------------------------------------------

const IssueOutcome = struct {
    number: i64,
    /// Project item id (allocator-owned, empty if not added or dry-run).
    item_id: []const u8,
};

/// createOrSkipIssueForProject creates a regular issue (no sub-issue link),
/// records the external_links row + sync_events, and adds the issue as a
/// Project item. Returns the (issue number, project item id) for follow-on
/// field-value updates.
fn createOrSkipIssueForProject(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    client: GhProjectsClient,
    opts: Opts,
    entity_kind: []const u8,
    entity_id: i64,
    owner: []const u8,
    repo: []const u8,
    project_node_id: []const u8,
    results: *std.ArrayList(EntityResult),
) anyerror!IssueOutcome {
    if (try parent_issue.loadExistingMirror(allocator, d, entity_kind, entity_id, opts.sys_id)) |existing| {
        defer allocator.free(existing.external_id);
        defer allocator.free(existing.external_url);
        const title = (try parent_issue.entityTitleOpt(allocator, d, entity_kind, entity_id)) orelse try allocator.dupe(u8, "");
        try results.append(allocator, .{
            .entity_kind = try allocator.dupe(u8, entity_kind),
            .entity_id = entity_id,
            .title = title,
            .op = .skipped,
            .external_id = try allocator.dupe(u8, existing.external_id),
            .external_url = try allocator.dupe(u8, existing.external_url),
            .error_name = try allocator.dupe(u8, ""),
        });
        const num = parseIssueNumberFromExternalID(existing.external_id) orelse 0;
        return .{ .number = num, .item_id = "" };
    }

    // Load title + body.
    const local = parent_issue_entityForCreate(allocator, d, entity_kind, entity_id) catch |e| {
        try appendFailure(allocator, results, entity_kind, entity_id, "", e);
        return e;
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
        return .{ .number = 0, .item_id = "" };
    }

    // Create the issue via REST.
    const body_or_default = if (local.body.len == 0) "_No description provided._" else local.body;
    const created = client.createIssueFn(client.ctx, allocator, owner, repo, local.title, body_or_default, &.{}) catch |e| {
        try appendFailure(allocator, results, entity_kind, entity_id, local.title, e);
        return e;
    };
    defer allocator.free(created.node_id);

    const external_id = try std.fmt.allocPrint(allocator, "{s}/{s}#{d}", .{ owner, repo, created.number });
    errdefer allocator.free(external_id);
    const external_url = try std.fmt.allocPrint(allocator, "https://github.com/{s}/{s}/issues/{d}", .{ owner, repo, created.number });
    errdefer allocator.free(external_url);

    _ = try parent_issue.recordLink(d, entity_kind, entity_id, opts.sys_id, external_id, external_url, opts.sync_direction);

    // Add to project (best-effort; failure is non-fatal).
    var item_id: []const u8 = "";
    if (created.node_id.len > 0 and project_node_id.len > 0 and !std.mem.eql(u8, project_node_id, "(dry-run)")) {
        if (client.addProjectV2ItemFn(client.ctx, allocator, project_node_id, created.node_id)) |id| {
            item_id = id;
        } else |e| {
            std.log.debug("addProjectV2Item failed (non-fatal) for {s}:{d}: {s}", .{ entity_kind, entity_id, @errorName(e) });
        }
    }

    try results.append(allocator, .{
        .entity_kind = try allocator.dupe(u8, entity_kind),
        .entity_id = entity_id,
        .title = try allocator.dupe(u8, local.title),
        .op = .created,
        .external_id = external_id,
        .external_url = external_url,
        .error_name = try allocator.dupe(u8, ""),
    });
    return .{ .number = created.number, .item_id = item_id };
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
// Per-entity repo resolution
// -----------------------------------------------------------------------------

/// RepoSlug is the (owner, repo) pair, both allocator-owned.
const RepoSlug = struct { owner: []const u8, repo: []const u8 };

/// repoForPlanOrFallback returns the GitHub coords for `plan_id` based on its
/// scope (repo) or falls back to `fb`. Returned slices are allocator-owned
/// when they came from `projectGitHubCoords`; when the fallback is returned,
/// the caller MUST NOT free the borrowed fallback slices (distinguished by
/// pointer equality on `owner`).
fn repoForPlanOrFallback(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    plan_id: i64,
    fb: RepoSlug,
) Error!RepoSlug {
    var stmt = d.prepare("select scope_kind, coalesce(scope_id, 0) from plans where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return fb;
    const scope_kind = stmt.columnTextAlloc(0, allocator) catch return Error.QueryFailed;
    defer allocator.free(scope_kind);
    const scope_id = stmt.columnInt(1);
    if (!std.mem.eql(u8, scope_kind, "repo") or scope_id == 0) return fb;

    const coords = parent_issue.projectGitHubCoords(allocator, d, scope_id) catch return fb;
    if (coords) |c| return .{ .owner = c.owner, .repo = c.repo };
    return fb;
}

/// repoForTaskOrFallback returns coords for `task_id`'s scope (repo) or its
/// first `touches` link, falling back to `fb`. See `repoForPlanOrFallback`
/// for ownership rules.
fn repoForTaskOrFallback(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    task_id: i64,
    fb: RepoSlug,
) Error!RepoSlug {
    // Try scope_kind='repo' first.
    {
        var stmt = d.prepare("select scope_kind, coalesce(scope_id, 0) from tasks where id = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
        const step = stmt.step() catch return Error.QueryFailed;
        if (step != .done) {
            const scope_kind = stmt.columnTextAlloc(0, allocator) catch return Error.QueryFailed;
            defer allocator.free(scope_kind);
            const scope_id = stmt.columnInt(1);
            if (std.mem.eql(u8, scope_kind, "repo") and scope_id > 0) {
                if (parent_issue.projectGitHubCoords(allocator, d, scope_id) catch null) |c| {
                    return .{ .owner = c.owner, .repo = c.repo };
                }
            }
        }
    }
    // Fall back to first touches link.
    var stmt = d.prepare(
        \\select to_id from entity_links
        \\where from_kind = 'task' and from_id = ? and to_kind = 'repo' and relationship = 'touches'
        \\limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return fb;
    const touched_id = stmt.columnInt(0);
    if (parent_issue.projectGitHubCoords(allocator, d, touched_id) catch null) |c| {
        return .{ .owner = c.owner, .repo = c.repo };
    }
    return fb;
}

/// firstTouchedRepo returns the GitHub coords of the first repo touched
/// anywhere in the feature subtree. Used as a fallback for plans/tasks that
/// have no own scope or touches link.
fn firstTouchedRepo(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
) Error!RepoSlug {
    const target = parent_issue.resolveTargetRepo(allocator, d, anchor_plan_id) catch return RepoSlug{ .owner = "", .repo = "" };
    return .{ .owner = target.owner, .repo = target.repo };
}

// -----------------------------------------------------------------------------
// config_json read/write for the anchor link
// -----------------------------------------------------------------------------

const ProjectConfig = struct {
    node_id: []const u8,
    location: []const u8,
};

/// readProjectConfigJSON pulls `project_node_id` + `project_location` from
/// the anchor plan's external_links.config_json. Returns empty slices when
/// the row or fields are missing. Caller owns both slices.
fn readProjectConfigJSON(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    anchor_plan_id: i64,
    system_id: i64,
) Error!ProjectConfig {
    var stmt = d.prepare(
        \\select coalesce(config_json, '') from external_links
        \\where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .int = anchor_plan_id }, .{ .int = system_id } }) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return .{ .node_id = "", .location = "" };
    const raw = try stmt.columnTextAlloc(0, allocator);
    defer allocator.free(raw);
    if (raw.len == 0) return .{ .node_id = "", .location = "" };
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, raw, .{}) catch return .{ .node_id = "", .location = "" };
    defer parsed.deinit();
    if (parsed.value != .object) return .{ .node_id = "", .location = "" };

    var node_id_out: []const u8 = "";
    var location_out: []const u8 = "";
    if (parsed.value.object.get("project_node_id")) |v| if (v == .string) {
        node_id_out = try allocator.dupe(u8, v.string);
    };
    if (parsed.value.object.get("project_location")) |v| if (v == .string) {
        location_out = try allocator.dupe(u8, v.string);
    };
    return .{ .node_id = node_id_out, .location = location_out };
}

/// recordLinkWithConfigJSON inserts the external_links row + paired
/// sync_events 'ok' row atomically, with config_json populated. Mirrors Go
/// `recordLinkWithConfigJSON`.
fn recordLinkWithConfigJSON(
    d: *db.sqlite.Db,
    entity_kind: []const u8,
    entity_id: i64,
    system_id: i64,
    external_id: []const u8,
    external_url: []const u8,
    sync_direction: link_mod.SyncDirection,
    config_json: []const u8,
) Error!void {
    d.exec("begin immediate") catch return Error.WriteFailed;
    var committed = false;
    defer if (!committed) d.exec("rollback") catch {};

    const link_id = d.execParams(
        \\insert into external_links
        \\  (entity_kind, entity_id, system_id, external_id, external_url,
        \\   link_role, sync_direction, last_sync_status, config_json)
        \\values (?, ?, ?, ?, ?, 'mirror', ?, 'ok', ?)
    , &.{
        .{ .text = entity_kind },
        .{ .int = entity_id },
        .{ .int = system_id },
        .{ .text = external_id },
        if (external_url.len == 0) .{ .null = {} } else .{ .text = external_url },
        .{ .text = sync_direction.toText() },
        if (config_json.len == 0) .{ .null = {} } else .{ .text = config_json },
    }) catch return Error.WriteFailed;

    _ = d.execParams(
        "insert into sync_events (link_id, direction, outcome) values (?, 'push', 'ok')",
        &.{.{ .int = link_id }},
    ) catch return Error.WriteFailed;

    d.exec("commit") catch return Error.WriteFailed;
    committed = true;
}

// -----------------------------------------------------------------------------
// misc helpers
// -----------------------------------------------------------------------------

const LocalCreate = struct {
    title: []const u8,
    body: []const u8,
};

/// parent_issue_entityForCreate proxies into parent_issue's private
/// entityForCreate by re-implementing it locally with the same shape. The
/// parent_issue function is unexported; rather than expose it we mirror it.
fn parent_issue_entityForCreate(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    entity_kind: []const u8,
    entity_id: i64,
) anyerror!LocalCreate {
    if (std.mem.eql(u8, entity_kind, "plan")) {
        var stmt = d.prepare("select coalesce(title,''), coalesce(summary,'') from plans where id = ?") catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = entity_id }}) catch return error.QueryFailed;
        const step = stmt.step() catch return error.QueryFailed;
        if (step == .done) return error.QueryFailed;
        return .{
            .title = try stmt.columnTextAlloc(0, allocator),
            .body = try stmt.columnTextAlloc(1, allocator),
        };
    } else if (std.mem.eql(u8, entity_kind, "task")) {
        var stmt = d.prepare("select coalesce(title,''), coalesce(body,'') from tasks where id = ?") catch return error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = entity_id }}) catch return error.QueryFailed;
        const step = stmt.step() catch return error.QueryFailed;
        if (step == .done) return error.QueryFailed;
        return .{
            .title = try stmt.columnTextAlloc(0, allocator),
            .body = try stmt.columnTextAlloc(1, allocator),
        };
    }
    return error.QueryFailed;
}

fn parseIssueNumberFromExternalID(external_id: []const u8) ?i64 {
    const hash = std.mem.lastIndexOfScalar(u8, external_id, '#') orelse return null;
    if (hash + 1 >= external_id.len) return null;
    return std.fmt.parseInt(i64, external_id[hash + 1 ..], 10) catch null;
}

// =============================================================================
// tests
// =============================================================================

const testing = std.testing;

fn setupDb() !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    try @import("db").migrate.applyAll(&d, testing.allocator);
    return d;
}

/// FakeClient implements GhProjectsClient against in-memory counters; tests
/// drive the engine without hitting the network.
const FakeClient = struct {
    issue_counter: i64 = 0,
    item_counter: i64 = 0,
    project_created: bool = false,
    issues_created: usize = 0,
    items_added: usize = 0,
    field_sets: usize = 0,
    last_parent_value: ?[]u8 = null,
    org_ids_present: bool = true,
    /// When true, the FIRST createProjectV2 call fails (so the engine falls
    /// back from org-level to user-level). Subsequent calls succeed.
    fail_first_project: bool = false,
    org_create_attempts: usize = 0,
    has_parent_field: bool = true,
    alloc: std.mem.Allocator,

    fn deinit(self: *FakeClient) void {
        if (self.last_parent_value) |v| self.alloc.free(v);
    }

    fn getAuthenticatedOwner(ctx: *anyopaque, allocator: std.mem.Allocator) anyerror!AuthenticatedOwner {
        const self: *FakeClient = @ptrCast(@alignCast(ctx));
        const user = try allocator.dupe(u8, "USER_NODE");
        if (self.org_ids_present) {
            var orgs = try allocator.alloc([]const u8, 1);
            orgs[0] = try allocator.dupe(u8, "ORG_NODE");
            return .{ .user_node_id = user, .org_ids = orgs };
        }
        const orgs = try allocator.alloc([]const u8, 0);
        return .{ .user_node_id = user, .org_ids = orgs };
    }

    fn createProjectV2(
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        owner_node_id: []const u8,
        _: []const u8,
    ) anyerror!CreatedProject {
        const self: *FakeClient = @ptrCast(@alignCast(ctx));
        // If owner is ORG_NODE, this is the org-level attempt.
        const is_org = std.mem.eql(u8, owner_node_id, "ORG_NODE");
        if (is_org) self.org_create_attempts += 1;
        if (is_org and self.fail_first_project) return error.OrgCreateRefused;
        self.project_created = true;
        const id = try std.fmt.allocPrint(allocator, "PROJ_{d}", .{1});
        const url = try std.fmt.allocPrint(allocator, "https://github.com/projects/{d}", .{1});
        return .{ .node_id = id, .url = url };
    }

    fn getProjectV2Fields(
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        _: []const u8,
    ) anyerror![]ProjectField {
        const self: *FakeClient = @ptrCast(@alignCast(ctx));
        if (!self.has_parent_field) return try allocator.alloc(ProjectField, 0);
        var out = try allocator.alloc(ProjectField, 2);
        out[0] = .{
            .id = try allocator.dupe(u8, "FIELD_STATUS"),
            .name = try allocator.dupe(u8, "Status"),
            .data_type = try allocator.dupe(u8, "SINGLE_SELECT"),
        };
        out[1] = .{
            .id = try allocator.dupe(u8, "FIELD_PARENT"),
            .name = try allocator.dupe(u8, "Parent"),
            .data_type = try allocator.dupe(u8, "TEXT"),
        };
        return out;
    }

    fn addProjectV2Item(
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        _: []const u8,
        _: []const u8,
    ) anyerror![]const u8 {
        const self: *FakeClient = @ptrCast(@alignCast(ctx));
        self.item_counter += 1;
        self.items_added += 1;
        return try std.fmt.allocPrint(allocator, "ITEM_{d}", .{self.item_counter});
    }

    fn setProjectV2ItemFieldValue(
        ctx: *anyopaque,
        allocator: std.mem.Allocator,
        _: []const u8,
        _: []const u8,
        _: []const u8,
        text_value: []const u8,
    ) anyerror!void {
        const self: *FakeClient = @ptrCast(@alignCast(ctx));
        self.field_sets += 1;
        if (self.last_parent_value) |v| self.alloc.free(v);
        self.last_parent_value = try self.alloc.dupe(u8, text_value);
        _ = allocator;
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
        self.issues_created += 1;
        const node_id = try std.fmt.allocPrint(allocator, "ISSUE_NODE_{d}", .{self.issue_counter});
        return .{ .number = self.issue_counter, .node_id = node_id };
    }

    fn client(self: *FakeClient) GhProjectsClient {
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
};

fn seedSystem(d: *db.sqlite.Db) !void {
    _ = try d.execParams(
        "insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) values ('github-issues','gh','','','token-env','X')",
        &.{},
    );
}

test "propagateProjectsV2 happy path with multi-repo creates project + items + sets Parent" {
    var d = try setupDb();
    defer d.close();
    try seedSystem(&d);
    // 2 repos.
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/api','api','https://github.com/acme/api.git')", &.{});
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/web','web','https://github.com/acme/web.git')", &.{});
    // Anchor + 1 child plan scoped to repo 1 (acme/api).
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    _ = try d.execParams("insert into plans (scope_kind, scope_id, title, slug, parent_plan_id) values ('repo', 1, 'child','c',1)", &.{});
    // 2 tasks scoped to repo 1 + repo 2 respectively under child.
    _ = try d.execParams("insert into tasks (scope_kind, scope_id, title, plan_id) values ('repo', 1, 't1', 2)", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, scope_id, title, plan_id) values ('repo', 2, 't2', 2)", &.{});

    var fake = FakeClient{ .alloc = testing.allocator };
    defer fake.deinit();
    var report = try propagateProjectsV2(
        testing.allocator,
        &d,
        fake.client(),
        1,
        .{ .sys_id = 1, .sys_slug = "gh" },
    );
    defer report.deinit(testing.allocator);

    // Anchor (project) + 1 child plan + 2 tasks = 4 created.
    try testing.expectEqual(@as(usize, 4), report.created);
    try testing.expectEqual(@as(usize, 0), report.skipped);
    try testing.expectEqual(@as(usize, 0), report.failed);
    try testing.expect(fake.project_created);
    try testing.expectEqual(@as(usize, 3), fake.issues_created);
    try testing.expectEqual(@as(usize, 3), fake.items_added);
    // Both tasks should have their Parent field set to the child plan's
    // owner/repo#N coords ("acme/api#1" — first issue created is the child plan).
    try testing.expectEqual(@as(usize, 2), fake.field_sets);
    try testing.expect(fake.last_parent_value != null);
    try testing.expectEqualStrings("acme/api#1", fake.last_parent_value.?);

    // Anchor link carries the strategy + project coords in config_json.
    var stmt = try d.prepare("select config_json from external_links where entity_kind='plan' and entity_id=1");
    defer stmt.finalize();
    _ = try stmt.step();
    const cfg = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(cfg);
    try testing.expect(std.mem.indexOf(u8, cfg, "\"strategy\":\"github-projects-v2\"") != null);
    try testing.expect(std.mem.indexOf(u8, cfg, "\"project_node_id\":\"PROJ_1\"") != null);
    try testing.expect(std.mem.indexOf(u8, cfg, "\"project_location\":\"org\"") != null);
}

test "propagateProjectsV2 second run is idempotent (everything skipped)" {
    var d = try setupDb();
    defer d.close();
    try seedSystem(&d);
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/api','api','https://github.com/acme/api.git')", &.{});
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/web','web','https://github.com/acme/web.git')", &.{});
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    _ = try d.execParams("insert into plans (scope_kind, scope_id, title, slug, parent_plan_id) values ('repo', 1, 'child','c',1)", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, scope_id, title, plan_id) values ('repo', 1, 't1', 2)", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, scope_id, title, plan_id) values ('repo', 2, 't2', 2)", &.{});

    var fake = FakeClient{ .alloc = testing.allocator };
    defer fake.deinit();
    var r1 = try propagateProjectsV2(testing.allocator, &d, fake.client(), 1, .{ .sys_id = 1, .sys_slug = "gh" });
    defer r1.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 4), r1.created);

    var fake2 = FakeClient{ .alloc = testing.allocator };
    defer fake2.deinit();
    var r2 = try propagateProjectsV2(testing.allocator, &d, fake2.client(), 1, .{ .sys_id = 1, .sys_slug = "gh" });
    defer r2.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 0), r2.created);
    try testing.expectEqual(@as(usize, 4), r2.skipped);
    try testing.expectEqual(@as(usize, 0), fake2.issues_created);
    try testing.expectEqual(@as(usize, 0), fake2.items_added);
    try testing.expect(!fake2.project_created); // No new project — recovered from config_json.
}

test "propagateProjectsV2 dry_run does not contact remote nor create links" {
    var d = try setupDb();
    defer d.close();
    try seedSystem(&d);
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/api','api','https://github.com/acme/api.git')", &.{});
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/web','web','https://github.com/acme/web.git')", &.{});
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});
    _ = try d.execParams("insert into plans (scope_kind, scope_id, title, slug, parent_plan_id) values ('repo', 1, 'child','c',1)", &.{});
    _ = try d.execParams("insert into tasks (scope_kind, scope_id, title, plan_id) values ('repo', 2, 't2', 2)", &.{});

    var fake = FakeClient{ .alloc = testing.allocator };
    defer fake.deinit();
    var r = try propagateProjectsV2(testing.allocator, &d, fake.client(), 1, .{ .sys_id = 1, .sys_slug = "gh", .dry_run = true });
    defer r.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 3), r.created);
    try testing.expect(!fake.project_created);
    try testing.expectEqual(@as(usize, 0), fake.issues_created);
    try testing.expectEqual(@as(usize, 0), fake.items_added);
    try testing.expectEqual(@as(i64, 0), try d.intQuery("select count(*) from external_links"));
}

test "propagateProjectsV2 falls back to user-level when org create fails" {
    var d = try setupDb();
    defer d.close();
    try seedSystem(&d);
    _ = try d.execParams("insert into projects (slug,name,git_remote) values ('acme/api','api','https://github.com/acme/api.git')", &.{});
    _ = try d.execParams("insert into plans (scope_kind, title, slug) values ('global','anchor','a')", &.{});

    var fake = FakeClient{ .alloc = testing.allocator, .fail_first_project = true };
    defer fake.deinit();
    var r = try propagateProjectsV2(testing.allocator, &d, fake.client(), 1, .{ .sys_id = 1, .sys_slug = "gh" });
    defer r.deinit(testing.allocator);
    try testing.expect(fake.project_created);
    try testing.expectEqual(@as(usize, 1), fake.org_create_attempts);
    // Anchor link config_json should record location=user.
    var stmt = try d.prepare("select config_json from external_links where entity_kind='plan' and entity_id=1");
    defer stmt.finalize();
    _ = try stmt.step();
    const cfg = try stmt.columnTextAlloc(0, testing.allocator);
    defer testing.allocator.free(cfg);
    try testing.expect(std.mem.indexOf(u8, cfg, "\"project_location\":\"user\"") != null);
}

test "discoverParentField returns null when no candidate matches" {
    // Drive only the field discovery path against the fake.
    const a = testing.allocator;
    var fake = FakeClient{ .alloc = a, .has_parent_field = false };
    defer fake.deinit();
    const id = try discoverParentField(a, fake.client(), "PROJ_X", &.{});
    try testing.expect(id == null);
}

test "discoverParentField picks first matching candidate case-insensitively" {
    const a = testing.allocator;
    var fake = FakeClient{ .alloc = a };
    defer fake.deinit();
    // Default candidates contains "Parent" — should match FIELD_PARENT.
    const id_default = (try discoverParentField(a, fake.client(), "PROJ_X", &.{})).?;
    defer a.free(id_default);
    try testing.expectEqualStrings("FIELD_PARENT", id_default);

    // Explicit candidates with mixed casing.
    const cands = [_][]const u8{ "initiative", "parent" };
    const id_lower = (try discoverParentField(a, fake.client(), "PROJ_X", &cands)).?;
    defer a.free(id_lower);
    try testing.expectEqualStrings("FIELD_PARENT", id_lower);
}
