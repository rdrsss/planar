//! engine/external/agentingest/interface — the vendor-event parser
//! boundary.
//!
//! M4 lands the Claude adapter; M6 will add a second-vendor adapter
//! (Codex or Copilot). Both adapters target the SAME normalized
//! `Event` shape defined here; the ingest engine dispatches normalized
//! events into the agentactivity store primitives without caring which
//! vendor's hook produced them.
//!
//! Design contract:
//!
//!   1. The adapter's `parse` function MUST be pure: it consumes a
//!      `[]const u8` payload (one JSON-encoded vendor hook event) and
//!      returns either a normalized `Event` or a typed parse error.
//!      No DB writes, no IO, no allocation outside the supplied
//!      allocator.
//!   2. The error set distinguishes "malformed" (the payload itself
//!      is not valid JSON or lacks required envelope fields) from
//!      "unknown event_type" (the payload parses but its event_type
//!      field is not in the adapter's known set). The CLI handler
//!      maps these to distinct exit-code paths per the test-spec.
//!   3. The normalized `Event` shape is intentionally minimal — it
//!      captures only what the agentactivity store primitives need
//!      (session lifecycle + action start/end). Vendor-specific extra
//!      fields are preserved as `summary` blobs; no per-vendor enums
//!      leak into this layer.
//!
//! See `claude.zig` for the Claude Code adapter implementation and
//! `dispatch.zig` for the engine wrapper that walks a parsed Event
//! and calls into `engine.runtime.session` + `agentactivity.store`.

const std = @import("std");
const types = @import("../../runtime/agentactivity/types.zig");

// =========================================================================
// Errors
// =========================================================================

/// Parse-time errors. `Malformed` covers JSON parse failures, missing
/// envelope fields, type mismatches on required fields. `UnknownEventType`
/// covers a syntactically-valid payload whose event_type is not in the
/// adapter's known set. The CLI handler distinguishes the two so it can
/// emit distinct error messages per the test-spec (both still exit
/// non-zero; neither writes any rows).
pub const ParseError = error{
    /// Payload is not parseable as JSON or is missing required envelope
    /// fields (e.g. no `event_type`, no `session_id`).
    Malformed,
    /// Payload parses but its event_type is not one the adapter knows.
    UnknownEventType,
} || std.mem.Allocator.Error;

// =========================================================================
// Normalized event shape
// =========================================================================

/// Outcome of an action — re-exported for adapter convenience so adapters
/// can construct `.action_end` events without reaching into
/// `engine/runtime/agentactivity/types.zig` themselves.
pub const Outcome = types.Outcome;

/// Action kind classifier — re-exported for adapter convenience.
pub const ActionKind = types.ActionKind;

/// Common envelope every normalized event carries. The vendor adapter
/// fills this in from the hook payload's vendor session id (plus
/// optional model + role metadata). The dispatch layer uses
/// `vendor_session_id` to resolve / auto-open the `sessions` row.
pub const Envelope = struct {
    /// Vendor's session id (e.g. Claude's `session_id` field). MAY be
    /// null only for vendors that genuinely lack a session-id concept;
    /// the dispatch layer treats null as "ensure a singleton session
    /// for this vendor".
    vendor_session_id: ?[]const u8,
    /// Free-text role (e.g. "coder", "reviewer"). Optional.
    vendor_role: ?[]const u8 = null,
    /// Model identifier (e.g. "claude-opus-4-7"). Optional.
    model: ?[]const u8 = null,

    pub fn deinit(self: Envelope, allocator: std.mem.Allocator) void {
        if (self.vendor_session_id) |s| allocator.free(s);
        if (self.vendor_role) |s| allocator.free(s);
        if (self.model) |s| allocator.free(s);
    }
};

/// Normalized event — the single shape every vendor adapter emits and
/// the dispatch layer consumes.
///
/// Variant ↔ store-primitive mapping:
///
///   .session_start  → session.startSession (idempotent)
///   .session_end    → session.endSession   (no-op if already ended)
///   .action_start   → store.startAction (no claim id; bare action row)
///   .action_end     → close most-recent open action of matching kind
///                      under this session, OR no-op if none found
///   .action_atomic  → start+end in one call (for tool_call /
///                     user_message / assistant_message — short-lived
///                     telemetry that's atomic in the hook)
pub const Event = union(enum) {
    session_start: SessionStart,
    session_end: SessionEnd,
    action_start: ActionStart,
    action_end: ActionEnd,
    action_atomic: ActionAtomic,

    pub fn deinit(self: Event, allocator: std.mem.Allocator) void {
        switch (self) {
            .session_start => |e| e.deinit(allocator),
            .session_end => |e| e.deinit(allocator),
            .action_start => |e| e.deinit(allocator),
            .action_end => |e| e.deinit(allocator),
            .action_atomic => |e| e.deinit(allocator),
        }
    }
};

pub const SessionStart = struct {
    envelope: Envelope,
    /// Optional summary recorded on the session_entries timeline.
    summary: ?[]const u8 = null,

    pub fn deinit(self: SessionStart, allocator: std.mem.Allocator) void {
        self.envelope.deinit(allocator);
        if (self.summary) |s| allocator.free(s);
    }
};

pub const SessionEnd = struct {
    envelope: Envelope,
    summary: ?[]const u8 = null,

    pub fn deinit(self: SessionEnd, allocator: std.mem.Allocator) void {
        self.envelope.deinit(allocator);
        if (self.summary) |s| allocator.free(s);
    }
};

pub const ActionStart = struct {
    envelope: Envelope,
    action_kind: ActionKind,
    summary: ?[]const u8 = null,

    pub fn deinit(self: ActionStart, allocator: std.mem.Allocator) void {
        self.envelope.deinit(allocator);
        if (self.summary) |s| allocator.free(s);
    }
};

pub const ActionEnd = struct {
    envelope: Envelope,
    action_kind: ActionKind,
    outcome: Outcome,
    summary: ?[]const u8 = null,

    pub fn deinit(self: ActionEnd, allocator: std.mem.Allocator) void {
        self.envelope.deinit(allocator);
        if (self.summary) |s| allocator.free(s);
    }
};

pub const ActionAtomic = struct {
    envelope: Envelope,
    action_kind: ActionKind,
    outcome: Outcome,
    summary: ?[]const u8 = null,

    pub fn deinit(self: ActionAtomic, allocator: std.mem.Allocator) void {
        self.envelope.deinit(allocator);
        if (self.summary) |s| allocator.free(s);
    }
};

// =========================================================================
// Vendor adapter
// =========================================================================

/// Vendor identifier. The dispatch layer routes by this tag. M4 wires
/// `claude`; M6 will wire one of `codex` / `copilot`.
pub const Vendor = enum {
    claude,
    codex,
    copilot,

    pub fn toText(self: Vendor) []const u8 {
        return @tagName(self);
    }

    pub fn fromText(s: []const u8) ?Vendor {
        if (std.mem.eql(u8, s, "claude")) return .claude;
        if (std.mem.eql(u8, s, "codex")) return .codex;
        if (std.mem.eql(u8, s, "copilot")) return .copilot;
        return null;
    }
};

/// Function pointer type for vendor adapters. Each adapter exposes one
/// `parse` symbol matching this signature; `dispatch.routeAndApply`
/// resolves the function pointer by vendor tag at call time.
pub const ParseFn = *const fn (
    allocator: std.mem.Allocator,
    payload: []const u8,
) ParseError!Event;

test "Vendor.fromText round-trip" {
    try std.testing.expectEqual(Vendor.claude, Vendor.fromText("claude").?);
    try std.testing.expectEqual(Vendor.codex, Vendor.fromText("codex").?);
    try std.testing.expectEqual(Vendor.copilot, Vendor.fromText("copilot").?);
    try std.testing.expect(Vendor.fromText("nope") == null);
}

test "Event.deinit releases owned strings without leaking" {
    const a = std.testing.allocator;
    const evt: Event = .{ .session_start = .{
        .envelope = .{
            .vendor_session_id = try a.dupe(u8, "abc"),
            .model = try a.dupe(u8, "claude"),
        },
        .summary = try a.dupe(u8, "hello"),
    } };
    evt.deinit(a);
}
