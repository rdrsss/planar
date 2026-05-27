//! engine/external/agentingest/copilot — GitHub Copilot hook payload parser.
//!
//! M6 second-vendor adapter. Targets the GitHub Copilot Coding Agent /
//! Copilot Chat event taxonomy as documented in the public GitHub
//! Copilot developer docs (May 2026). Copilot's per-event envelope is
//! slightly richer than Claude's — it namespaces event names with a
//! dot (e.g. `session.started`, `turn.user`, `tool.invocation`) — but
//! the carried fields are equivalent. We collapse the namespacing into
//! the same normalized `Event` shape Claude uses; no vendor-specific
//! Event variants leak through the interface boundary.
//!
//! Envelope shape (the load-bearing fields; Copilot payloads also
//! include `repo`, `actor`, `correlation_id`, etc. that we deliberately
//! ignore at this layer — the dispatch layer doesn't need them and
//! preserving them would dirty the normalized shape):
//!
//!   {
//!     "event":          "session.started" | "session.completed"
//!                     | "turn.user"       | "turn.assistant"
//!                     | "tool.invocation",
//!     "session_id":     "<vendor session id>",    -- required
//!     "model":          "<model identifier>" | null,
//!     "role":           "coder" | "reviewer" | ... | null,
//!     "summary":        "<free text>" | null,
//!     "status":         "ok" | "error" | "aborted" | "timeout"
//!                       -- optional on turn.* / tool.invocation;
//!                          defaults to "ok"
//!   }
//!
//! Mapping to normalized events (matches Claude's adapter so the
//! dispatch layer is genuinely vendor-agnostic):
//!
//!   session.started     → Event.session_start
//!   session.completed   → Event.session_end
//!   turn.user           → Event.action_atomic{kind=user_message,outcome=*}
//!   turn.assistant      → Event.action_atomic{kind=assistant_message,outcome=*}
//!   tool.invocation     → Event.action_atomic{kind=tool_call,outcome=*}
//!
//! Mapping caveats documented for future maintainers:
//!
//!   - Copilot's `status` field is named differently from Claude's
//!     `outcome`, but the value set is identical. We accept both names
//!     so operators piping raw Copilot events get the documented
//!     behavior, and the normalized `Outcome` is what dispatch sees.
//!   - Copilot emits `event.namespaced.like.this`. We split on the
//!     first dot and route on the full string; this keeps adding new
//!     event types a one-line change to `mapEvent`.
//!   - Unknown / missing event surfaces `ParseError.UnknownEventType`
//!     just like Claude's adapter. Missing required envelope fields
//!     surface `ParseError.Malformed`. The CLI handler exit-codes the
//!     two paths identically across vendors.

const std = @import("std");
const iface = @import("interface.zig");

/// Parse a Copilot hook payload into a normalized event. Pure function;
/// no IO, no DB, no global state. The returned event owns its string
/// fields against `allocator`.
pub fn parse(allocator: std.mem.Allocator, payload: []const u8) iface.ParseError!iface.Event {
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, payload, .{}) catch {
        return iface.ParseError.Malformed;
    };
    defer parsed.deinit();

    if (parsed.value != .object) return iface.ParseError.Malformed;
    const obj = parsed.value.object;

    const event_name = stringField(obj, "event") orelse return iface.ParseError.Malformed;
    const session_id_raw = stringField(obj, "session_id") orelse return iface.ParseError.Malformed;

    const vendor_session_id = try allocator.dupe(u8, session_id_raw);
    errdefer allocator.free(vendor_session_id);

    var model_opt: ?[]u8 = null;
    if (stringField(obj, "model")) |m| {
        model_opt = try allocator.dupe(u8, m);
    }
    errdefer if (model_opt) |m| allocator.free(m);

    var role_opt: ?[]u8 = null;
    if (stringField(obj, "role")) |r| {
        role_opt = try allocator.dupe(u8, r);
    }
    errdefer if (role_opt) |r| allocator.free(r);

    var summary_opt: ?[]u8 = null;
    if (stringField(obj, "summary")) |s| {
        summary_opt = try allocator.dupe(u8, s);
    }
    errdefer if (summary_opt) |s| allocator.free(s);

    const envelope: iface.Envelope = .{
        .vendor_session_id = vendor_session_id,
        .vendor_role = role_opt,
        .model = model_opt,
    };

    if (std.mem.eql(u8, event_name, "session.started")) {
        return .{ .session_start = .{ .envelope = envelope, .summary = summary_opt } };
    }
    if (std.mem.eql(u8, event_name, "session.completed")) {
        return .{ .session_end = .{ .envelope = envelope, .summary = summary_opt } };
    }

    const atomic_kind: ?iface.ActionKind = if (std.mem.eql(u8, event_name, "turn.user"))
        .user_message
    else if (std.mem.eql(u8, event_name, "turn.assistant"))
        .assistant_message
    else if (std.mem.eql(u8, event_name, "tool.invocation"))
        .tool_call
    else
        null;

    if (atomic_kind) |k| {
        // Outcome defaults to ok for atomic short-lived events. Copilot
        // calls the field `status`; accept that AND `outcome` so an
        // operator can normalize on either side without losing data.
        var outcome: iface.Outcome = .ok;
        if (stringField(obj, "status")) |s| {
            outcome = iface.Outcome.fromText(s) orelse return iface.ParseError.Malformed;
        } else if (stringField(obj, "outcome")) |o| {
            outcome = iface.Outcome.fromText(o) orelse return iface.ParseError.Malformed;
        }
        return .{ .action_atomic = .{
            .envelope = envelope,
            .action_kind = k,
            .outcome = outcome,
            .summary = summary_opt,
        } };
    }

    // event field is present but not in the known set. The errdefers
    // above release `vendor_session_id`, `model_opt`, `role_opt`, and
    // `summary_opt` as the error unwinds.
    return iface.ParseError.UnknownEventType;
}

/// String-field accessor that returns null for absent keys, null values,
/// or non-string types. Centralizes the "JSON envelope is loose, treat
/// missing as null" contract.
fn stringField(obj: std.json.ObjectMap, key: []const u8) ?[]const u8 {
    const v = obj.get(key) orelse return null;
    return switch (v) {
        .string => |s| s,
        else => null,
    };
}

// =========================================================================
// Tests
// =========================================================================

test "parse session.started with model + role" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"session.started","session_id":"cop-sid-1","model":"gpt-5-copilot","role":"coder"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expect(evt == .session_start);
    try std.testing.expectEqualStrings("cop-sid-1", evt.session_start.envelope.vendor_session_id.?);
    try std.testing.expectEqualStrings("gpt-5-copilot", evt.session_start.envelope.model.?);
    try std.testing.expectEqualStrings("coder", evt.session_start.envelope.vendor_role.?);
}

test "parse session.completed without optional fields" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"session.completed","session_id":"cop-sid-x"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expect(evt == .session_end);
    try std.testing.expect(evt.session_end.envelope.model == null);
    try std.testing.expect(evt.session_end.envelope.vendor_role == null);
}

test "parse tool.invocation becomes action_atomic with kind=tool_call" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"tool.invocation","session_id":"cop-sid-tc","summary":"ran rg"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expect(evt == .action_atomic);
    try std.testing.expectEqual(iface.ActionKind.tool_call, evt.action_atomic.action_kind);
    try std.testing.expectEqual(iface.Outcome.ok, evt.action_atomic.outcome);
    try std.testing.expectEqualStrings("ran rg", evt.action_atomic.summary.?);
}

test "parse turn.user defaults outcome to ok" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"turn.user","session_id":"cop-sid-um"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expect(evt == .action_atomic);
    try std.testing.expectEqual(iface.ActionKind.user_message, evt.action_atomic.action_kind);
    try std.testing.expectEqual(iface.Outcome.ok, evt.action_atomic.outcome);
}

test "parse turn.assistant honors status=error" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"turn.assistant","session_id":"cop-sid-am","status":"error"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expectEqual(iface.Outcome.@"error", evt.action_atomic.outcome);
}

test "parse tool.invocation honors outcome alias when status absent" {
    // We accept `outcome` as a fallback alias so operators piping
    // events through a generic normalizer aren't surprised.
    const a = std.testing.allocator;
    const payload =
        \\{"event":"tool.invocation","session_id":"cop-sid-ali","outcome":"timeout"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expectEqual(iface.Outcome.timeout, evt.action_atomic.outcome);
}

test "parse: status wins over outcome when both present" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"tool.invocation","session_id":"cop-sid-both","status":"aborted","outcome":"ok"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expectEqual(iface.Outcome.aborted, evt.action_atomic.outcome);
}

test "malformed: not JSON" {
    const a = std.testing.allocator;
    try std.testing.expectError(iface.ParseError.Malformed, parse(a, "not json at all"));
}

test "malformed: JSON but not an object" {
    const a = std.testing.allocator;
    try std.testing.expectError(iface.ParseError.Malformed, parse(a, "[1,2,3]"));
}

test "malformed: missing event field" {
    const a = std.testing.allocator;
    try std.testing.expectError(
        iface.ParseError.Malformed,
        parse(a, "{\"session_id\":\"x\"}"),
    );
}

test "malformed: missing session_id" {
    const a = std.testing.allocator;
    try std.testing.expectError(
        iface.ParseError.Malformed,
        parse(a, "{\"event\":\"session.started\"}"),
    );
}

test "unknown event surfaces UnknownEventType (NOT Malformed)" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"telemetry.heartbeat","session_id":"cop-sid-x"}
    ;
    try std.testing.expectError(
        iface.ParseError.UnknownEventType,
        parse(a, payload),
    );
}

test "malformed: status field present but not a known value" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"tool.invocation","session_id":"sid","status":"weird"}
    ;
    try std.testing.expectError(iface.ParseError.Malformed, parse(a, payload));
}

test "malformed: outcome alias present but not a known value" {
    const a = std.testing.allocator;
    const payload =
        \\{"event":"tool.invocation","session_id":"sid","outcome":"weird"}
    ;
    try std.testing.expectError(iface.ParseError.Malformed, parse(a, payload));
}
