//! engine/planning/closeout — `planar plan closeout` delivery-evidence gate.
//!
//! Evaluates whether a plan is safe to close and, when the hard gate passes
//! and the caller requests apply, marks the plan `done`.
//!
//! ## Hard gate (DB-evidence; each failure is a reason that blocks apply)
//!
//! 1. All tasks terminal — every task on the plan (and recursively on
//!    descendant plans) is `done` or `cancelled`. Open tasks block.
//! 2. All descendant plans terminal — every child plan (recursively, via
//!    `parent_plan_id`) is `done` or `abandoned`. Open descendants block.
//! 3. No live claims — no `active`, non-expired `agent_work_claims` on the
//!    plan's tasks. Expired/stale claims do NOT block (report as warning).
//!
//! Cancelled tasks are terminal history — they are counted in the audit
//! output and do NOT block closeout.
//!
//! ## Advisory git-evidence (reported, never blocks)
//!
//! Best-effort ancestry checks from `agent_work_claims` locality columns.
//! When `session_commits` is not yet populated, reports "no commit
//! attribution — inconclusive".
//!
//! ## Apply semantics
//!
//! When `apply=true` and the hard gate passes, the plan's status is set to
//! `done` via a direct SQL UPDATE (same as recomputeStatus apply path, which
//! bypasses the policy status matrix — this is operator-owned and allowed
//! for anchor plans too). When the plan is already terminal, apply is a no-op
//! (`applied=false`).

const std = @import("std");
const db = @import("db");
const policy = @import("../policy.zig");

// =========================================================================
// Public types
// =========================================================================

/// Task-status counts for the plan.
pub const TaskCounts = struct {
    open: i64 = 0, // todo + doing + blocked
    done: i64 = 0,
    cancelled: i64 = 0,
};

/// Descendant plan counts.
pub const DescendantCounts = struct {
    open: i64 = 0, // draft + active + paused
    terminal: i64 = 0, // done + abandoned
};

/// Active vs stale claim counts.
pub const ClaimCounts = struct {
    live: i64 = 0, // active and not yet expired
    stale: i64 = 0, // active but lease_expires_at has passed
};

/// One advisory git-evidence entry derived from a single
/// (repo_root, branch, head_sha_at_claim) tuple from agent_work_claims.
pub const GitEvidence = struct {
    repo_root: []const u8,
    branch: ?[]const u8, // null when not recorded
    target_branch: ?[]const u8, // null when detection failed
    base_merged: ?bool, // null = inconclusive / unavailable
    branch_merged: ?bool, // null = inconclusive / branch absent or unavailable
    note: []const u8, // human-readable status string; always set
};

/// The full result of a closeout evaluation.
pub const CloseoutResult = struct {
    plan_id: i64,
    ready: bool,
    applied: bool,
    hard_evidence: struct {
        tasks: TaskCounts,
        descendants: DescendantCounts,
        claims: ClaimCounts,
    },
    blocked_by: []const []const u8, // owned slices; freed by deinit
    git_evidence: []const GitEvidence, // owned; freed by deinit
    warnings: []const []const u8, // owned; freed by deinit

    pub fn deinit(self: CloseoutResult, allocator: std.mem.Allocator) void {
        for (self.blocked_by) |s| allocator.free(s);
        allocator.free(self.blocked_by);
        for (self.git_evidence) |e| {
            allocator.free(e.repo_root);
            if (e.branch) |b| allocator.free(b);
            if (e.target_branch) |t| allocator.free(t);
            allocator.free(e.note);
        }
        allocator.free(self.git_evidence);
        for (self.warnings) |w| allocator.free(w);
        allocator.free(self.warnings);
    }
};

pub const Error =
    error{
        NotFound,
        QueryFailed,
        AlreadyTerminal,
    } ||
    std.mem.Allocator.Error ||
    policy.audit.Error;

// =========================================================================
// Main entry point
// =========================================================================

/// Evaluate the closeout gate for `plan_id`. When `apply=true` and the
/// hard gate passes and the plan is not already terminal, marks the plan
/// `done`. The caller owns the returned CloseoutResult and must call
/// `result.deinit(allocator)`.
pub fn evaluate(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
    apply: bool,
) Error!CloseoutResult {
    // Verify the plan exists and grab its current status.
    const current_status = try fetchPlanStatus(d, allocator, plan_id);

    // If already terminal, short-circuit: apply is a no-op.
    const already_terminal = std.mem.eql(u8, current_status, "done") or
        std.mem.eql(u8, current_status, "abandoned");
    allocator.free(current_status);

    if (already_terminal) {
        return CloseoutResult{
            .plan_id = plan_id,
            .ready = true,
            .applied = false,
            .hard_evidence = .{
                .tasks = .{},
                .descendants = .{},
                .claims = .{},
            },
            .blocked_by = try allocator.alloc([]const u8, 0),
            .git_evidence = try allocator.alloc(GitEvidence, 0),
            .warnings = &.{},
        };
    }

    // Collect hard-gate evidence.
    const task_counts = try collectTaskCounts(d, plan_id);
    const desc_counts = try collectDescendantCounts(d, plan_id);
    const claim_counts = try collectClaimCounts(d, plan_id);

    // Build blocked_by list.
    var reasons: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (reasons.items) |s| allocator.free(s);
        reasons.deinit(allocator);
    }

    if (task_counts.open > 0) {
        const msg = try std.fmt.allocPrint(
            allocator,
            "{d} open task(s) on plan (todo/doing/blocked)",
            .{task_counts.open},
        );
        try reasons.append(allocator, msg);
    }

    if (desc_counts.open > 0) {
        const msg = try std.fmt.allocPrint(
            allocator,
            "{d} open descendant plan(s) (draft/active/paused)",
            .{desc_counts.open},
        );
        try reasons.append(allocator, msg);
    }

    if (claim_counts.live > 0) {
        const msg = try std.fmt.allocPrint(
            allocator,
            "{d} live claim(s) still active on plan tasks",
            .{claim_counts.live},
        );
        try reasons.append(allocator, msg);
    }

    const blocked_by = try reasons.toOwnedSlice(allocator);
    const ready = blocked_by.len == 0;

    // Build warnings (stale claims are advisory, not blocking).
    var warns: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (warns.items) |w| allocator.free(w);
        warns.deinit(allocator);
    }

    if (claim_counts.stale > 0) {
        const w = try std.fmt.allocPrint(
            allocator,
            "{d} stale/expired claim(s) on plan tasks — reconcilable, not blocking",
            .{claim_counts.stale},
        );
        try warns.append(allocator, w);
    }

    const warnings = try warns.toOwnedSlice(allocator);

    // Collect advisory git evidence.
    const git_evidence = try collectGitEvidence(d, allocator, plan_id);

    // Apply if requested and gate passes.
    const applied = if (apply and ready) blk: {
        _ = d.execParams(
            "update plans set status = 'done', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
            &.{.{ .int = plan_id }},
        ) catch return Error.QueryFailed;
        try policy.audit.record(d, .{
            .verb = .status_change,
            .entity = .{ .kind = "plan", .id = plan_id },
            .summary = null,
        });
        break :blk true;
    } else false;

    return CloseoutResult{
        .plan_id = plan_id,
        .ready = ready,
        .applied = applied,
        .hard_evidence = .{
            .tasks = task_counts,
            .descendants = desc_counts,
            .claims = claim_counts,
        },
        .blocked_by = blocked_by,
        .git_evidence = git_evidence,
        .warnings = warnings,
    };
}

// =========================================================================
// Hard-gate data collection
// =========================================================================

fn fetchPlanStatus(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) Error![]const u8 {
    var stmt = d.prepare("select status from plans where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => return stmt.columnTextAlloc(0, allocator) catch return Error.QueryFailed,
    }
}

fn collectTaskCounts(d: *db.sqlite.Db, plan_id: i64) Error!TaskCounts {
    var counts = TaskCounts{};
    // Sum tasks directly on this plan.
    {
        var stmt = d.prepare(
            \\select
            \\  sum(case when status in ('todo','doing','blocked') then 1 else 0 end),
            \\  sum(case when status = 'done' then 1 else 0 end),
            \\  sum(case when status = 'cancelled' then 1 else 0 end)
            \\from tasks where plan_id = ?
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {},
            .row => {
                counts.open += stmt.columnInt(0);
                counts.done += stmt.columnInt(1);
                counts.cancelled += stmt.columnInt(2);
            },
        }
    }

    // Also count tasks on all descendant plans (recursive walk via a
    // WITH RECURSIVE CTE). SQLite supports recursive CTEs since 3.8.3
    // which is well before the vendored amalgamation version.
    {
        var stmt = d.prepare(
            \\with recursive desc_plans(id) as (
            \\  select id from plans where parent_plan_id = ?
            \\  union all
            \\  select p.id from plans p join desc_plans d on p.parent_plan_id = d.id
            \\)
            \\select
            \\  sum(case when t.status in ('todo','doing','blocked') then 1 else 0 end),
            \\  sum(case when t.status = 'done' then 1 else 0 end),
            \\  sum(case when t.status = 'cancelled' then 1 else 0 end)
            \\from tasks t
            \\where t.plan_id in (select id from desc_plans)
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => {},
            .row => {
                counts.open += stmt.columnInt(0);
                counts.done += stmt.columnInt(1);
                counts.cancelled += stmt.columnInt(2);
            },
        }
    }

    return counts;
}

fn collectDescendantCounts(d: *db.sqlite.Db, plan_id: i64) Error!DescendantCounts {
    var counts = DescendantCounts{};
    var stmt = d.prepare(
        \\with recursive desc_plans(id) as (
        \\  select id from plans where parent_plan_id = ?
        \\  union all
        \\  select p.id from plans p join desc_plans d on p.parent_plan_id = d.id
        \\)
        \\select
        \\  sum(case when p.status in ('draft','active','paused') then 1 else 0 end),
        \\  sum(case when p.status in ('done','abandoned') then 1 else 0 end)
        \\from plans p
        \\where p.id in (select id from desc_plans)
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => {},
        .row => {
            counts.open = stmt.columnInt(0);
            counts.terminal = stmt.columnInt(1);
        },
    }
    return counts;
}

fn collectClaimCounts(d: *db.sqlite.Db, plan_id: i64) Error!ClaimCounts {
    var counts = ClaimCounts{};
    // Count active claims on tasks belonging to this plan (direct tasks only —
    // descendant plan tasks are covered via desc_plans join below).
    var stmt = d.prepare(
        \\with recursive desc_plans(id) as (
        \\  select ? as id
        \\  union all
        \\  select p.id from plans p join desc_plans d on p.parent_plan_id = d.id
        \\)
        \\select
        \\  sum(case when c.lease_expires_at > strftime('%Y-%m-%dT%H:%M:%fZ','now') then 1 else 0 end),
        \\  sum(case when c.lease_expires_at <= strftime('%Y-%m-%dT%H:%M:%fZ','now') then 1 else 0 end)
        \\from agent_work_claims c
        \\join tasks t on t.id = c.entity_id
        \\where c.entity_kind = 'task'
        \\  and c.status = 'active'
        \\  and t.plan_id in (select id from desc_plans)
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;
    switch (stmt.step() catch return Error.QueryFailed) {
        .done => {},
        .row => {
            counts.live = stmt.columnInt(0);
            counts.stale = stmt.columnInt(1);
        },
    }
    return counts;
}

// =========================================================================
// Advisory git-evidence collection
// =========================================================================

/// Collect distinct (repo_root, branch, head_sha_at_claim) tuples from
/// agent_work_claims for tasks on this plan, then perform best-effort git
/// checks for each.
fn collectGitEvidence(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
) Error![]GitEvidence {
    // Collect distinct locality tuples.
    const LocalityRow = struct {
        repo_root: ?[]const u8,
        branch: ?[]const u8,
        head_sha: ?[]const u8,
    };

    var rows: std.ArrayList(LocalityRow) = .empty;
    defer {
        for (rows.items) |r| {
            if (r.repo_root) |p| allocator.free(p);
            if (r.branch) |b| allocator.free(b);
            if (r.head_sha) |s| allocator.free(s);
        }
        rows.deinit(allocator);
    }

    {
        var stmt = d.prepare(
            \\with recursive desc_plans(id) as (
            \\  select ? as id
            \\  union all
            \\  select p.id from plans p join desc_plans d on p.parent_plan_id = d.id
            \\)
            \\select distinct c.repo_root, c.branch, c.head_sha_at_claim
            \\from agent_work_claims c
            \\join tasks t on t.id = c.entity_id
            \\where c.entity_kind = 'task'
            \\  and t.plan_id in (select id from desc_plans)
            \\  and c.repo_root is not null
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;

        while (true) {
            switch (stmt.step() catch return Error.QueryFailed) {
                .done => break,
                .row => {
                    const row = LocalityRow{
                        .repo_root = stmt.columnTextOpt(0, allocator) catch return Error.QueryFailed,
                        .branch = stmt.columnTextOpt(1, allocator) catch return Error.QueryFailed,
                        .head_sha = stmt.columnTextOpt(2, allocator) catch return Error.QueryFailed,
                    };
                    rows.append(allocator, row) catch return error.OutOfMemory;
                },
            }
        }
    }

    if (rows.items.len == 0) {
        // No locality data at all — emit a single inconclusive summary entry.
        var evs = try allocator.alloc(GitEvidence, 1);
        evs[0] = .{
            .repo_root = try allocator.dupe(u8, "(none)"),
            .branch = null,
            .target_branch = null,
            .base_merged = null,
            .branch_merged = null,
            .note = try allocator.dupe(
                u8,
                "no commit attribution — inconclusive (hardens once session-commit capture is wired)",
            ),
        };
        return evs;
    }

    var evs: std.ArrayList(GitEvidence) = .empty;
    errdefer {
        for (evs.items) |e| {
            allocator.free(e.repo_root);
            if (e.branch) |b| allocator.free(b);
            if (e.target_branch) |t| allocator.free(t);
            allocator.free(e.note);
        }
        evs.deinit(allocator);
    }

    for (rows.items) |row| {
        const ev = try probeGitEvidence(allocator, row.repo_root, row.branch, row.head_sha);
        evs.append(allocator, ev) catch return error.OutOfMemory;
    }

    return evs.toOwnedSlice(allocator);
}

/// Run best-effort git probes for a single (repo_root, branch, sha) tuple.
fn probeGitEvidence(
    allocator: std.mem.Allocator,
    repo_root_opt: ?[]const u8,
    branch_opt: ?[]const u8,
    sha_opt: ?[]const u8,
) std.mem.Allocator.Error!GitEvidence {
    const repo_root = repo_root_opt orelse "(unknown)";
    const repo_root_owned = try allocator.dupe(u8, repo_root);
    errdefer allocator.free(repo_root_owned);

    const branch_owned: ?[]const u8 = if (branch_opt) |b| try allocator.dupe(u8, b) else null;
    errdefer if (branch_owned) |b| allocator.free(b);

    // Detect target branch: symbolic-ref origin/HEAD → strip "origin/".
    const target_branch = detectTargetBranch(allocator, repo_root);
    errdefer if (target_branch) |t| allocator.free(t);

    if (target_branch == null) {
        // Git unavailable or not a repo.
        const note = try allocator.dupe(u8, "git-evidence unavailable");
        return .{
            .repo_root = repo_root_owned,
            .branch = branch_owned,
            .target_branch = null,
            .base_merged = null,
            .branch_merged = null,
            .note = note,
        };
    }

    const target = target_branch.?;

    // base_merged: check if head_sha is an ancestor of target.
    var base_merged: ?bool = null;
    if (sha_opt) |sha| {
        base_merged = checkIsAncestor(allocator, repo_root, sha, target);
    }

    // branch_merged: check if branch is merged into target.
    var branch_merged: ?bool = null;
    var branch_note: ?[]u8 = null;
    if (branch_opt) |branch| {
        // Check if branch still exists.
        const exists = branchExists(allocator, repo_root, branch);
        if (exists) {
            branch_merged = checkBranchMerged(allocator, repo_root, branch, target);
        } else {
            branch_note = try allocator.dupe(u8, "branch absent — inconclusive");
        }
    }
    defer if (branch_note) |n| allocator.free(n);

    // Build note string.
    const note = blk: {
        if (branch_note) |n| {
            break :blk try allocator.dupe(u8, n);
        }
        if (base_merged == null and branch_merged == null) {
            break :blk try allocator.dupe(u8, "no sha to check — inconclusive");
        }
        const bm_str: []const u8 = if (base_merged) |bm| (if (bm) "base-merged=true (weak signal)" else "base-merged=false") else "base-merged=unknown";
        const brm_str: []const u8 = if (branch_merged) |brm| (if (brm) "branch-merged=true" else "branch-merged=false") else "";
        if (brm_str.len == 0) {
            break :blk try allocator.dupe(u8, bm_str);
        }
        break :blk try std.fmt.allocPrint(allocator, "{s}; {s}", .{ bm_str, brm_str });
    };

    return .{
        .repo_root = repo_root_owned,
        .branch = branch_owned,
        .target_branch = target_branch,
        .base_merged = base_merged,
        .branch_merged = branch_merged,
        .note = note,
    };
}

// =========================================================================
// Git subprocess helpers (best-effort; all failures return null/false)
// =========================================================================

/// The IO handle for spawning git subprocesses. Using the global
/// single-threaded handle (same as skillrender.zig) — git probes are
/// best-effort advisory evidence and never run concurrently from a
/// handler context.
fn gitIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

fn gitRun(allocator: std.mem.Allocator, repo_root: []const u8, argv_extra: []const []const u8) ?[]u8 {
    var argv_buf: [16][]const u8 = undefined;
    if (3 + argv_extra.len > argv_buf.len) return null;
    argv_buf[0] = "git";
    argv_buf[1] = "-C";
    argv_buf[2] = repo_root;
    for (argv_extra, 0..) |a, i| argv_buf[3 + i] = a;
    const argv = argv_buf[0 .. 3 + argv_extra.len];

    const result = std.process.run(allocator, gitIo(), .{
        .argv = argv,
    }) catch return null;
    defer allocator.free(result.stderr);

    switch (result.term) {
        .exited => |code| {
            if (code != 0) {
                allocator.free(result.stdout);
                return null;
            }
        },
        else => {
            allocator.free(result.stdout);
            return null;
        },
    }
    return result.stdout;
}

fn gitRunTrim(allocator: std.mem.Allocator, repo_root: []const u8, argv_extra: []const []const u8) ?[]u8 {
    const raw = gitRun(allocator, repo_root, argv_extra) orelse return null;
    defer allocator.free(raw);
    const trimmed = std.mem.trim(u8, raw, " \t\r\n");
    if (trimmed.len == 0) return null;
    return allocator.dupe(u8, trimmed) catch null;
}

/// Detect the remote's default branch. Returns an owned slice or null.
fn detectTargetBranch(allocator: std.mem.Allocator, repo_root: []const u8) ?[]u8 {
    // Try: git symbolic-ref --short refs/remotes/origin/HEAD → "origin/main"
    if (gitRunTrim(allocator, repo_root, &.{ "symbolic-ref", "--short", "refs/remotes/origin/HEAD" })) |raw| {
        defer allocator.free(raw);
        const prefix = "origin/";
        if (std.mem.startsWith(u8, raw, prefix)) {
            return allocator.dupe(u8, raw[prefix.len..]) catch null;
        }
        return allocator.dupe(u8, raw) catch null;
    }
    // Fallback: try "main" first, then "master".
    for (&[_][]const u8{ "main", "master" }) |branch| {
        const exists = branchExists(allocator, repo_root, branch);
        if (exists) return allocator.dupe(u8, branch) catch null;
    }
    return null;
}

/// Returns true if `git merge-base --is-ancestor sha target` exits 0.
fn checkIsAncestor(allocator: std.mem.Allocator, repo_root: []const u8, sha: []const u8, target: []const u8) bool {
    const raw = gitRun(allocator, repo_root, &.{ "merge-base", "--is-ancestor", sha, target }) orelse return false;
    allocator.free(raw);
    return true;
}

/// Returns true if branch exists (locally or as a remote-tracking ref).
fn branchExists(allocator: std.mem.Allocator, repo_root: []const u8, branch: []const u8) bool {
    // git rev-parse --verify refs/heads/<branch> exits 0 if branch exists.
    var ref_buf: [128]u8 = undefined;
    const ref = std.fmt.bufPrint(&ref_buf, "refs/heads/{s}", .{branch}) catch return false;
    const raw = gitRun(allocator, repo_root, &.{ "rev-parse", "--verify", ref }) orelse return false;
    allocator.free(raw);
    return true;
}

/// Returns true when `branch` appears in `git branch --merged <target>`.
fn checkBranchMerged(allocator: std.mem.Allocator, repo_root: []const u8, branch: []const u8, target: []const u8) bool {
    const raw = gitRun(allocator, repo_root, &.{ "branch", "--merged", target }) orelse return false;
    defer allocator.free(raw);
    // Each line is "  <branch>" or "* <branch>". Strip whitespace/asterisk.
    var it = std.mem.splitScalar(u8, raw, '\n');
    while (it.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " *\t\r");
        if (std.mem.eql(u8, trimmed, branch)) return true;
    }
    return false;
}
