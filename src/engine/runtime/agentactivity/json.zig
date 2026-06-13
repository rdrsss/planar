//! agentactivity/json — shared JSON-emission helpers for the
//! `agent_work_claims` / `agent_actions` row shapes.
//!
//! Lifted out of the per-binary `planar-agent/handlers/json.zig`
//! (plan 85 M3) so both `planar` and `planar-agent` (and future
//! `planar-watch`) render the same wire contract for claim and action
//! rows. The shape mirrors the tech-spec § "JSON shapes" → `ClaimRow`,
//! `ActionRow` — snake_case keys matching the table columns, including
//! the locality columns (`repo_root`, `branch`, `head_sha_at_claim`,
//! `dirty_at_claim`) and worktree columns (`worktree_id`,
//! `worktree_path`).
//!
//! Hand-rolled rather than `std.json.Stringify.value` on the typed
//! struct because the enum fields (ClaimStatus, EntityKind, Dirty,
//! ActionKind, Outcome) need to render as their textual form
//! (`"status":"active"`) rather than integer ordinals.

const std = @import("std");
const types = @import("types.zig");
const task_mod = @import("../../planning/task.zig");

fn writeStringOpt(w: *std.Io.Writer, key: []const u8, val: ?[]const u8) !void {
    try w.print(",\"{s}\":", .{key});
    if (val) |s| {
        try std.json.Stringify.encodeJsonString(s, .{}, w);
    } else {
        try w.print("null", .{});
    }
}

fn writeIntOpt(w: *std.Io.Writer, key: []const u8, val: ?i64) !void {
    if (val) |v| {
        try w.print(",\"{s}\":{d}", .{ key, v });
    } else {
        try w.print(",\"{s}\":null", .{key});
    }
}

/// Compact latest-action snapshot for the `latest_action` JSON field on
/// `planar-watch ps` claim rows. Pass `null` to omit the field (callers
/// outside `planar-watch ps` do not need it).
pub const LatestActionInfo = struct {
    kind: []const u8,
    summary: ?[]const u8,
    started_at: []const u8,
};

/// Emit the canonical ClaimRow JSON shape. The helper writes the
/// surrounding `{ … }`; callers wrap it in the enclosing object.
///
/// `entity_scope` is optional — pass `null` for the lean shape (used
/// by surfaces that don't care about the underlying entity's storage
/// scope, e.g. log timelines) or a resolved
/// `types.ClaimScopeInfo` from `store.resolveClaimScope` for the
/// operator-facing surfaces (planar-watch ps / claims / feed). When
/// non-null, the field is emitted right after `entity_id` so the
/// "where is this entity stored" context lives next to the "which
/// entity" reference in the JSON shape.
///
/// `latest_action` is optional — pass a `LatestActionInfo` to add a
/// `"latest_action":{kind,summary,started_at}` field near the end of
/// the claim object. Pass `null` to omit the field for backward
/// compatibility. Used by `planar-watch ps` (plan 467 M3 task 3056).
pub fn writeClaim(
    w: *std.Io.Writer,
    c: types.Claim,
    entity_scope: ?types.ClaimScopeInfo,
) !void {
    return writeClaimWithActivity(w, c, entity_scope, false, null);
}

/// Like `writeClaim` but also emits `"latest_action"` when
/// `include_latest_action` is true. `action` may be null (→ emits
/// `"latest_action":null`) or populated (→ emits the object).
/// All callers outside `planar-watch ps` use `writeClaim` which sets
/// `include_latest_action = false` so backward compatibility is preserved.
pub fn writeClaimWithActivity(
    w: *std.Io.Writer,
    c: types.Claim,
    entity_scope: ?types.ClaimScopeInfo,
    include_latest_action: bool,
    action: ?LatestActionInfo,
) !void {
    try w.print("{{\"id\":{d}", .{c.id});
    try w.print(",\"claim_token\":", .{});
    try std.json.Stringify.encodeJsonString(c.claim_token, .{}, w);
    try w.print(",\"session_id\":{d}", .{c.session_id});
    try w.print(",\"entity_kind\":\"{s}\"", .{c.entity_kind.toText()});
    try w.print(",\"entity_id\":{d}", .{c.entity_id});
    if (entity_scope) |s| {
        try w.print(",\"entity_scope\":{{\"kind\":\"{s}\",\"slug\":", .{s.kind});
        if (s.slug) |slug| {
            try std.json.Stringify.encodeJsonString(slug, .{}, w);
        } else {
            try w.print("null", .{});
        }
        try w.print("}}", .{});
    }
    try w.print(",\"claim_scope\":\"{s}\"", .{c.claim_scope.toText()});
    try w.print(",\"status\":\"{s}\"", .{c.status.toText()});
    try w.print(",\"vendor\":", .{});
    try std.json.Stringify.encodeJsonString(c.vendor, .{}, w);
    try writeStringOpt(w, "vendor_session_id", c.vendor_session_id);
    try writeStringOpt(w, "role", c.role);
    try writeStringOpt(w, "model", c.model);
    try writeIntOpt(w, "worktree_id", c.worktree_id);
    try writeStringOpt(w, "worktree_path", c.worktree_path);
    try writeStringOpt(w, "repo_root", c.repo_root);
    try writeStringOpt(w, "branch", c.branch);
    try writeStringOpt(w, "head_sha_at_claim", c.head_sha_at_claim);
    try w.print(",\"dirty_at_claim\":", .{});
    if (c.dirty_at_claim) |d| {
        try w.print("\"{s}\"", .{d.toText()});
    } else {
        try w.print("null", .{});
    }
    try writeStringOpt(w, "purpose", c.purpose);
    try writeStringOpt(w, "base_ref", c.base_ref);
    try w.print(",\"claimed_at\":", .{});
    try std.json.Stringify.encodeJsonString(c.claimed_at, .{}, w);
    try w.print(",\"last_heartbeat_at\":", .{});
    try std.json.Stringify.encodeJsonString(c.last_heartbeat_at, .{}, w);
    try w.print(",\"lease_expires_at\":", .{});
    try std.json.Stringify.encodeJsonString(c.lease_expires_at, .{}, w);
    try writeStringOpt(w, "released_at", c.released_at);
    try writeStringOpt(w, "release_reason", c.release_reason);
    try writeIntOpt(w, "run_id", c.run_id);
    try writeStringOpt(w, "stage", c.stage);
    // latest_action — emitted only by planar-watch ps (plan 467 M3
    // task 3056). When include_latest_action is false the field is
    // omitted entirely for backward compatibility with other consumers
    // of writeClaim. When true, emits an object or null.
    if (include_latest_action) {
        if (action) |a| {
            try w.print(",\"latest_action\":{{\"kind\":\"{s}\"", .{a.kind});
            try writeStringOpt(w, "summary", a.summary);
            try w.print(",\"started_at\":", .{});
            try std.json.Stringify.encodeJsonString(a.started_at, .{}, w);
            try w.print("}}", .{});
        } else {
            try w.print(",\"latest_action\":null", .{});
        }
    }
    try w.print("}}", .{});
}

/// Emit a Task row using the same field set as `planar task show --json`.
/// Hand-rolled so the JSON shape stays stable independently of any
/// engine-side struct refactor.
pub fn writeTask(w: *std.Io.Writer, t: task_mod.Task) !void {
    try w.print("{{\"id\":{d}", .{t.id});
    try w.print(",\"scope_kind\":\"{s}\"", .{@tagName(t.scope_kind)});
    try writeIntOpt(w, "scope_id", t.scope_id);
    try writeIntOpt(w, "plan_id", t.plan_id);
    try writeIntOpt(w, "parent_task_id", t.parent_task_id);
    try w.print(",\"title\":", .{});
    try std.json.Stringify.encodeJsonString(t.title, .{}, w);
    try writeStringOpt(w, "body", t.body);
    try writeStringOpt(w, "slug", t.slug);
    try w.print(",\"status\":\"{s}\"", .{@tagName(t.status)});
    try w.print(",\"priority\":{d}", .{t.priority});
    try writeStringOpt(w, "next_action", t.next_action);
    try writeStringOpt(w, "due_at", t.due_at);
    try w.print(",\"created_at\":", .{});
    try std.json.Stringify.encodeJsonString(t.created_at, .{}, w);
    try w.print(",\"updated_at\":", .{});
    try std.json.Stringify.encodeJsonString(t.updated_at, .{}, w);
    try w.print("}}", .{});
}

/// Emit the canonical ActionRow JSON shape.
pub fn writeAction(w: *std.Io.Writer, a: types.Action) !void {
    try w.print("{{\"id\":{d}", .{a.id});
    try w.print(",\"session_id\":{d}", .{a.session_id});
    try writeIntOpt(w, "session_entry_id", a.session_entry_id);
    try writeIntOpt(w, "parent_action_id", a.parent_action_id);
    try writeIntOpt(w, "claim_id", a.claim_id);
    try w.print(",\"action_kind\":\"{s}\"", .{a.action_kind.toText()});
    try w.print(",\"entity_kind\":", .{});
    if (a.entity_kind) |k| {
        try w.print("\"{s}\"", .{k.toText()});
    } else {
        try w.print("null", .{});
    }
    try writeIntOpt(w, "entity_id", a.entity_id);
    try w.print(",\"vendor\":", .{});
    try std.json.Stringify.encodeJsonString(a.vendor, .{}, w);
    try writeStringOpt(w, "vendor_role", a.vendor_role);
    try writeStringOpt(w, "model", a.model);
    try w.print(",\"started_at\":", .{});
    try std.json.Stringify.encodeJsonString(a.started_at, .{}, w);
    try writeStringOpt(w, "ended_at", a.ended_at);
    try w.print(",\"outcome\":", .{});
    if (a.outcome) |o| {
        try w.print("\"{s}\"", .{o.toText()});
    } else {
        try w.print("null", .{});
    }
    try writeStringOpt(w, "summary", a.summary);
    try writeStringOpt(w, "head_sha", a.head_sha);
    try w.print(",\"dirty\":", .{});
    if (a.dirty) |d| {
        try w.print("\"{s}\"", .{d.toText()});
    } else {
        try w.print("null", .{});
    }
    // metadata is opaque caller-attached text (typically JSON). Emit as
    // a string field for stability — surfaces that want to round-trip
    // it as structured JSON parse the string themselves. NULL when
    // unset.
    try writeStringOpt(w, "metadata", a.metadata);
    try w.print("}}", .{});
}
