//! agentactivity/types — value types shared across store + atomic ops.
//!
//! These mirror the `agent_work_claims` and `agent_actions` row shapes
//! from `migrations/00015_agent_activity.up.sql`. All string fields are
//! owned by the allocator passed to the function that produced them; use
//! the per-type `deinit` helpers to release them.

const std = @import("std");

/// Locality snapshot — the per-event git context recorded on claims and
/// actions. NULL values map to `null` here; the `dirty` field uses the
/// same enum the schema CHECK enforces.
pub const Dirty = enum {
    clean,
    dirty,
    unknown,

    pub fn fromText(s: []const u8) ?Dirty {
        if (std.mem.eql(u8, s, "clean")) return .clean;
        if (std.mem.eql(u8, s, "dirty")) return .dirty;
        if (std.mem.eql(u8, s, "unknown")) return .unknown;
        return null;
    }

    pub fn toText(self: Dirty) []const u8 {
        return @tagName(self);
    }
};

/// Locality probe result. All fields allocator-owned (caller's allocator);
/// `null` means the probe was skipped or could not determine the value
/// (non-git directory, missing `git` binary, subprocess error).
pub const Locality = struct {
    /// Canonical absolute path of the checkout the agent is operating in.
    /// `null` when no repo_root was supplied and process cwd lookup failed.
    repo_root: ?[]const u8 = null,
    /// Branch name. `null` for detached HEAD.
    branch: ?[]const u8 = null,
    /// Resolved git HEAD SHA at probe time.
    head_sha: ?[]const u8 = null,
    /// Working-tree cleanliness at probe time. `unknown` when the probe
    /// did not run or failed.
    dirty: Dirty = .unknown,

    pub fn deinit(self: Locality, allocator: std.mem.Allocator) void {
        if (self.repo_root) |s| allocator.free(s);
        if (self.branch) |s| allocator.free(s);
        if (self.head_sha) |s| allocator.free(s);
    }

    /// The skipped / probe-not-run sentinel: all-NULL with dirty=unknown.
    pub const skipped: Locality = .{};
};

pub const ClaimScope = enum {
    exclusive,
    shared,

    pub fn fromText(s: []const u8) ?ClaimScope {
        if (std.mem.eql(u8, s, "exclusive")) return .exclusive;
        if (std.mem.eql(u8, s, "shared")) return .shared;
        return null;
    }

    pub fn toText(self: ClaimScope) []const u8 {
        return @tagName(self);
    }
};

pub const ClaimStatus = enum {
    active,
    released,
    completed,
    aborted,
    stale,

    pub fn fromText(s: []const u8) ?ClaimStatus {
        if (std.mem.eql(u8, s, "active")) return .active;
        if (std.mem.eql(u8, s, "released")) return .released;
        if (std.mem.eql(u8, s, "completed")) return .completed;
        if (std.mem.eql(u8, s, "aborted")) return .aborted;
        if (std.mem.eql(u8, s, "stale")) return .stale;
        return null;
    }

    pub fn toText(self: ClaimStatus) []const u8 {
        return @tagName(self);
    }
};

pub const EntityKind = enum {
    plan,
    plan_step,
    task,

    pub fn fromText(s: []const u8) ?EntityKind {
        if (std.mem.eql(u8, s, "plan")) return .plan;
        if (std.mem.eql(u8, s, "plan_step")) return .plan_step;
        if (std.mem.eql(u8, s, "task")) return .task;
        return null;
    }

    pub fn toText(self: EntityKind) []const u8 {
        return @tagName(self);
    }
};

/// One row from `agent_work_claims`. All `[]const u8` fields owned by
/// the allocator passed to the producing function.
pub const Claim = struct {
    id: i64,
    claim_token: []const u8,
    session_id: i64,
    entity_kind: EntityKind,
    entity_id: i64,
    claim_scope: ClaimScope,
    status: ClaimStatus,
    vendor: []const u8,
    vendor_session_id: ?[]const u8,
    role: ?[]const u8,
    model: ?[]const u8,
    worktree_id: ?i64,
    worktree_path: ?[]const u8,
    repo_root: ?[]const u8,
    branch: ?[]const u8,
    head_sha_at_claim: ?[]const u8,
    dirty_at_claim: ?Dirty,
    purpose: ?[]const u8,
    base_ref: ?[]const u8,
    claimed_at: []const u8,
    last_heartbeat_at: []const u8,
    lease_expires_at: []const u8,
    released_at: ?[]const u8,
    release_reason: ?[]const u8,
    /// Nullable FK to workflow_runs.id; non-null when the claim was
    /// acquired inside a planar-execute workflow run via --run <id>.
    run_id: ?i64 = null,
    /// Free-text stage name from the workflow that dispatched this
    /// worker (e.g. "plan", "code", "review"). Null when the claim was
    /// not acquired inside a run, or when --stage was omitted.
    stage: ?[]const u8 = null,

    pub fn deinit(self: Claim, allocator: std.mem.Allocator) void {
        allocator.free(self.claim_token);
        allocator.free(self.vendor);
        if (self.vendor_session_id) |s| allocator.free(s);
        if (self.role) |s| allocator.free(s);
        if (self.model) |s| allocator.free(s);
        if (self.worktree_path) |s| allocator.free(s);
        if (self.repo_root) |s| allocator.free(s);
        if (self.branch) |s| allocator.free(s);
        if (self.head_sha_at_claim) |s| allocator.free(s);
        if (self.purpose) |s| allocator.free(s);
        if (self.base_ref) |s| allocator.free(s);
        allocator.free(self.claimed_at);
        allocator.free(self.last_heartbeat_at);
        allocator.free(self.lease_expires_at);
        if (self.released_at) |s| allocator.free(s);
        if (self.release_reason) |s| allocator.free(s);
        if (self.stage) |s| allocator.free(s);
    }

    pub fn deinitMany(items: []const Claim, allocator: std.mem.Allocator) void {
        for (items) |c| c.deinit(allocator);
        allocator.free(items);
    }
};

/// Resolved entity-scope context for a single claim — the scope the
/// claim's underlying plan / plan_step / task lives in. Surfaced by
/// planar-watch ps / claims so operators can answer "what work is
/// being done where" without joining tables by eye.
///
/// `kind` is the schema-level scope_kind string ("global" /
/// "association" / "repo"); the string form is stable across
/// migrations and what consumers compare against. `slug` is the
/// human-readable resolution from `associations.slug` / `projects.slug`;
/// `null` for global, or when the underlying scope row was deleted
/// out from under the claim.
///
/// `slug` is allocator-owned when non-null; release via `deinit`.
/// `kind` is a static string literal — do not free.
pub const ClaimScopeInfo = struct {
    kind: []const u8,
    slug: ?[]const u8,

    pub fn deinit(self: ClaimScopeInfo, allocator: std.mem.Allocator) void {
        if (self.slug) |s| allocator.free(s);
    }

    /// Display label — `slug` when present, otherwise the kind name.
    /// Operators see `scope:project:planar` or `scope:global`, never
    /// an empty value.
    pub fn label(self: ClaimScopeInfo) []const u8 {
        return self.slug orelse self.kind;
    }

    /// Sentinel for the "lookup failed / row missing" path. Callers
    /// can pass this through display surfaces without special-casing.
    pub const unknown: ClaimScopeInfo = .{ .kind = "?", .slug = null };
};

/// All action kinds the schema CHECK allows. The probe-default decision
/// (`probeDefault`) maps each kind to "yes, run git probe" or "no,
/// short-lived telemetry — skip".
pub const ActionKind = enum {
    planner,
    ingestor,
    coder,
    test_coder,
    reviewer,
    ext_sync,
    ext_propagate,
    orchestrator,
    @"resume",
    workbench_sync,
    spec_draft,
    claim_check,
    heartbeat,
    tool_call,
    user_message,
    assistant_message,
    other,

    pub fn fromText(s: []const u8) ?ActionKind {
        inline for (@typeInfo(ActionKind).@"enum".fields) |f| {
            if (std.mem.eql(u8, s, f.name)) return @field(ActionKind, f.name);
        }
        return null;
    }

    pub fn toText(self: ActionKind) []const u8 {
        return @tagName(self);
    }

    /// Per-action-kind locality-probe default per tech-spec § Locality
    /// tracking. Role kinds (planner/coder/reviewer/test_coder) probe by
    /// default; high-volume telemetry kinds (heartbeat/tool_call) skip
    /// the probe to avoid forking `git` per heartbeat. The caller can
    /// always override via `--no-locality-probe` at the CLI.
    pub fn probeDefault(self: ActionKind) bool {
        return switch (self) {
            .heartbeat, .tool_call => false,
            else => true,
        };
    }
};

pub const ActionEntityKind = enum {
    plan,
    plan_step,
    task,
    question,
    test_scenario,
    artifact,
    decision,

    pub fn fromText(s: []const u8) ?ActionEntityKind {
        inline for (@typeInfo(ActionEntityKind).@"enum".fields) |f| {
            if (std.mem.eql(u8, s, f.name)) return @field(ActionEntityKind, f.name);
        }
        return null;
    }

    pub fn toText(self: ActionEntityKind) []const u8 {
        return @tagName(self);
    }
};

pub const Outcome = enum {
    ok,
    @"error",
    aborted,
    timeout,

    pub fn fromText(s: []const u8) ?Outcome {
        if (std.mem.eql(u8, s, "ok")) return .ok;
        if (std.mem.eql(u8, s, "error")) return .@"error";
        if (std.mem.eql(u8, s, "aborted")) return .aborted;
        if (std.mem.eql(u8, s, "timeout")) return .timeout;
        return null;
    }

    pub fn toText(self: Outcome) []const u8 {
        return @tagName(self);
    }
};

/// One row from `agent_actions`. Owned strings released via `deinit`.
///
/// `metadata` is free-form caller-attached JSON text (migration 00016).
/// The engine treats it as opaque text — only consumers like the
/// orchestrator strategy gate parse it. NULL when the caller did not
/// supply metadata.
pub const Action = struct {
    id: i64,
    session_id: i64,
    session_entry_id: ?i64,
    parent_action_id: ?i64,
    claim_id: ?i64,
    action_kind: ActionKind,
    entity_kind: ?ActionEntityKind,
    entity_id: ?i64,
    vendor: []const u8,
    vendor_role: ?[]const u8,
    model: ?[]const u8,
    started_at: []const u8,
    ended_at: ?[]const u8,
    outcome: ?Outcome,
    summary: ?[]const u8,
    head_sha: ?[]const u8,
    dirty: ?Dirty,
    metadata: ?[]const u8 = null,

    pub fn deinit(self: Action, allocator: std.mem.Allocator) void {
        allocator.free(self.vendor);
        if (self.vendor_role) |s| allocator.free(s);
        if (self.model) |s| allocator.free(s);
        allocator.free(self.started_at);
        if (self.ended_at) |s| allocator.free(s);
        if (self.summary) |s| allocator.free(s);
        if (self.head_sha) |s| allocator.free(s);
        if (self.metadata) |s| allocator.free(s);
    }

    pub fn deinitMany(items: []const Action, allocator: std.mem.Allocator) void {
        for (items) |a| a.deinit(allocator);
        allocator.free(items);
    }
};

test "ActionKind.probeDefault: heartbeats and tool_calls skip" {
    try std.testing.expect(!ActionKind.heartbeat.probeDefault());
    try std.testing.expect(!ActionKind.tool_call.probeDefault());
    try std.testing.expect(ActionKind.coder.probeDefault());
    try std.testing.expect(ActionKind.planner.probeDefault());
    try std.testing.expect(ActionKind.reviewer.probeDefault());
    try std.testing.expect(ActionKind.test_coder.probeDefault());
}

test "Dirty enum round-trips fromText/toText" {
    try std.testing.expectEqual(Dirty.clean, Dirty.fromText("clean").?);
    try std.testing.expectEqual(Dirty.dirty, Dirty.fromText("dirty").?);
    try std.testing.expectEqual(Dirty.unknown, Dirty.fromText("unknown").?);
    try std.testing.expect(Dirty.fromText("bogus") == null);
}

test "ActionKind enum recognizes every schema-CHECK value" {
    // Sanity: spot-check the more exotic names.
    try std.testing.expectEqual(ActionKind.test_coder, ActionKind.fromText("test_coder").?);
    try std.testing.expectEqual(ActionKind.ext_propagate, ActionKind.fromText("ext_propagate").?);
    try std.testing.expectEqual(ActionKind.assistant_message, ActionKind.fromText("assistant_message").?);
    try std.testing.expectEqual(ActionKind.@"resume", ActionKind.fromText("resume").?);
}
