//! engine/runtime — durable-handoff machinery.
//!
//! Five submodules:
//!
//!   session      — Sessions table + session_entries timeline.
//!   snapshot     — Context snapshots: resume packet payload.
//!   handoff      — Handoffs entity + status transitions.
//!   resume       — 8-section resume packet builder + validate.
//!   capture      — Operator-facing glue around session+snapshot.
//!   audit_trail  — Read-side queries against audit_log (+ entity_links).
//!
//! `policy.audit.record` is the write path for audit_log; modules here
//! own the lifecycle and read paths.

pub const session = @import("runtime/session.zig");
pub const snapshot = @import("runtime/snapshot.zig");
pub const handoff = @import("runtime/handoff.zig");
pub const @"resume" = @import("runtime/resume.zig");
pub const capture = @import("runtime/capture.zig");
pub const audit_trail = @import("runtime/audit_trail.zig");
pub const agentactivity = @import("runtime/agentactivity.zig");

test {
    _ = session;
    _ = snapshot;
    _ = handoff;
    _ = @"resume";
    _ = capture;
    _ = audit_trail;
    _ = agentactivity;
}
