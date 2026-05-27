//! engine/external/agentingest/claude — Claude Code hook payload parser.
//!
//! Reads a Claude hook JSON payload and returns a normalized `Event`
//! per `interface.zig`. The Claude hook envelope (per the Claude Code
//! public hook docs, May 2026) has this minimal shape:
//!
//!   {
//!     "event_type": "session_start" | "session_end"
//!                  | "user_message"  | "assistant_message"
//!                  | "tool_call",
//!     "session_id": "<vendor session id>",          -- required
//!     "model":      "<model identifier>" | null,    -- optional
//!     "summary":    "<free text>"        | null,    -- optional
//!     "outcome":    "ok" | "error" | "aborted" | "timeout"  -- optional;
//!                                                     defaults to "ok"
//!                                                     on user/assistant/
//!                                                     tool_call events
//!   }
//!
//! Mapping to normalized events:
//!
//!   session_start      → Event.session_start
//!   session_end        → Event.session_end
//!   user_message       → Event.action_atomic{kind=user_message,outcome=ok}
//!   assistant_message  → Event.action_atomic{kind=assistant_message,outcome=ok}
//!   tool_call          → Event.action_atomic{kind=tool_call,outcome=ok}
//!
//! Unknown / missing event_type is `ParseError.UnknownEventType` (the
//! payload is valid JSON but the event_type field is not in the set
//! above). Missing required envelope fields (no event_type at all, no
//! session_id) is `ParseError.Malformed`.
//!
//! This adapter is intentionally a small, viable subset of Claude's
//! full hook surface. More event_types can be wired by adding entries
//! to `mapEventType` without touching the interface.

const std = @import("std");
const iface = @import("interface.zig");

/// Parse a Claude hook payload into a normalized event. Pure function;
/// no IO, no DB, no global state. The returned event owns its string
/// fields against `allocator`.
pub fn parse(allocator: std.mem.Allocator, payload: []const u8) iface.ParseError!iface.Event {
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, payload, .{}) catch {
        return iface.ParseError.Malformed;
    };
    defer parsed.deinit();

    if (parsed.value != .object) return iface.ParseError.Malformed;
    const obj = parsed.value.object;

    const event_type = stringField(obj, "event_type") orelse return iface.ParseError.Malformed;
    const session_id_raw = stringField(obj, "session_id") orelse return iface.ParseError.Malformed;

    const vendor_session_id = try allocator.dupe(u8, session_id_raw);
    errdefer allocator.free(vendor_session_id);

    var model_opt: ?[]u8 = null;
    if (stringField(obj, "model")) |m| {
        model_opt = try allocator.dupe(u8, m);
    }
    errdefer if (model_opt) |m| allocator.free(m);

    var summary_opt: ?[]u8 = null;
    if (stringField(obj, "summary")) |s| {
        summary_opt = try allocator.dupe(u8, s);
    }
    errdefer if (summary_opt) |s| allocator.free(s);

    const envelope: iface.Envelope = .{
        .vendor_session_id = vendor_session_id,
        .model = model_opt,
    };

    if (std.mem.eql(u8, event_type, "session_start")) {
        return .{ .session_start = .{ .envelope = envelope, .summary = summary_opt } };
    }
    if (std.mem.eql(u8, event_type, "session_end")) {
        return .{ .session_end = .{ .envelope = envelope, .summary = summary_opt } };
    }

    const atomic_kind: ?iface.ActionKind = if (std.mem.eql(u8, event_type, "user_message"))
        .user_message
    else if (std.mem.eql(u8, event_type, "assistant_message"))
        .assistant_message
    else if (std.mem.eql(u8, event_type, "tool_call"))
        .tool_call
    else
        null;

    if (atomic_kind) |k| {
        // Outcome defaults to ok for atomic short-lived events; the
        // payload MAY override.
        var outcome: iface.Outcome = .ok;
        if (stringField(obj, "outcome")) |o| {
            outcome = iface.Outcome.fromText(o) orelse return iface.ParseError.Malformed;
        }
        return .{ .action_atomic = .{
            .envelope = envelope,
            .action_kind = k,
            .outcome = outcome,
            .summary = summary_opt,
        } };
    }

    // event_type field is present but not in the known set. The
    // errdefers above release `vendor_session_id`, `model_opt`, and
    // `summary_opt` as the error unwinds — we don't free them again
    // here or we'd double-free.
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

test "parse session_start with all envelope fields" {
    const a = std.testing.allocator;
    const payload =
        \\{"event_type":"session_start","session_id":"sid-1","model":"claude-opus-4-7"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expect(evt == .session_start);
    try std.testing.expectEqualStrings("sid-1", evt.session_start.envelope.vendor_session_id.?);
    try std.testing.expectEqualStrings("claude-opus-4-7", evt.session_start.envelope.model.?);
}

test "parse session_end without model field" {
    const a = std.testing.allocator;
    const payload =
        \\{"event_type":"session_end","session_id":"sid-x"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expect(evt == .session_end);
    try std.testing.expect(evt.session_end.envelope.model == null);
}

test "parse tool_call becomes action_atomic with kind=tool_call" {
    const a = std.testing.allocator;
    const payload =
        \\{"event_type":"tool_call","session_id":"sid-tc","summary":"ran bash"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expect(evt == .action_atomic);
    try std.testing.expectEqual(iface.ActionKind.tool_call, evt.action_atomic.action_kind);
    try std.testing.expectEqual(iface.Outcome.ok, evt.action_atomic.outcome);
    try std.testing.expectEqualStrings("ran bash", evt.action_atomic.summary.?);
}

test "parse user_message defaults outcome to ok" {
    const a = std.testing.allocator;
    const payload =
        \\{"event_type":"user_message","session_id":"sid-um"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expect(evt == .action_atomic);
    try std.testing.expectEqual(iface.ActionKind.user_message, evt.action_atomic.action_kind);
    try std.testing.expectEqual(iface.Outcome.ok, evt.action_atomic.outcome);
}

test "parse assistant_message accepts explicit outcome" {
    const a = std.testing.allocator;
    const payload =
        \\{"event_type":"assistant_message","session_id":"sid-am","outcome":"error"}
    ;
    const evt = try parse(a, payload);
    defer evt.deinit(a);
    try std.testing.expectEqual(iface.Outcome.@"error", evt.action_atomic.outcome);
}

test "malformed: not JSON" {
    const a = std.testing.allocator;
    try std.testing.expectError(iface.ParseError.Malformed, parse(a, "not json at all"));
}

test "malformed: JSON but not an object" {
    const a = std.testing.allocator;
    try std.testing.expectError(iface.ParseError.Malformed, parse(a, "[1,2,3]"));
}

test "malformed: missing event_type" {
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
        parse(a, "{\"event_type\":\"session_start\"}"),
    );
}

test "unknown event_type surfaces UnknownEventType (NOT Malformed)" {
    const a = std.testing.allocator;
    const payload =
        \\{"event_type":"telemetry_blob","session_id":"sid-x"}
    ;
    try std.testing.expectError(
        iface.ParseError.UnknownEventType,
        parse(a, payload),
    );
}

test "malformed: outcome field present but not a known value" {
    const a = std.testing.allocator;
    const payload =
        \\{"event_type":"tool_call","session_id":"sid","outcome":"weird"}
    ;
    try std.testing.expectError(iface.ParseError.Malformed, parse(a, payload));
}
