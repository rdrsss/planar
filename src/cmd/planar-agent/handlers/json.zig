//! handlers/json — shared JSON-emission helpers for planar-agent verbs.
//!
//! Hand-rolls the JSON shapes documented in the tech spec
//! § "JSON shapes" — claim row, action row, task row. Centralized so
//! every verb's `--json` output decodes against the same contract; the
//! integration tests assert the shape.
//!
//! Why hand-rolled rather than std.json.Stringify.value on the typed
//! struct: the Claim / Action structs include enum fields (ClaimStatus,
//! EntityKind, Dirty, ActionKind, Outcome) that `std.json.Stringify`
//! renders as integer ordinals by default. The wire contract pins the
//! string form (`"status": "active"`, `"dirty": "clean"`, etc.). We
//! write each field explicitly so the shape stays stable and human-
//! readable.

const std = @import("std");
const engine = @import("engine");
const types = engine.runtime.agentactivity.types;

/// Write a claim_token / vendor-style nullable string. Always emits the
/// key; the value is either a JSON string or `null`.
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

/// Emit the canonical ClaimRow JSON shape (snake_case keys matching
/// `agent_work_claims` columns). The leading `{` and trailing `}` are
/// written by the helper; callers wrap it in the enclosing object.
pub fn writeClaim(w: *std.Io.Writer, c: types.Claim) !void {
    try w.print("{{\"id\":{d}", .{c.id});
    try w.print(",\"claim_token\":", .{});
    try std.json.Stringify.encodeJsonString(c.claim_token, .{}, w);
    try w.print(",\"session_id\":{d}", .{c.session_id});
    try w.print(",\"entity_kind\":\"{s}\"", .{c.entity_kind.toText()});
    try w.print(",\"entity_id\":{d}", .{c.entity_id});
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
    try w.print("}}", .{});
}

/// Emit a Task row using the same field set as `planar task show --json`.
/// Hand-rolled so we don't need to import the cmd/planar output layer.
pub fn writeTask(w: *std.Io.Writer, t: engine.planning.task.Task) !void {
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
