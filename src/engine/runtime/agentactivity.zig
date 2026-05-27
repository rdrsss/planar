//! engine/runtime/agentactivity — claim coordination + typed agent
//! activity record.
//!
//! Submodules:
//!
//!   types     — value types shared across store + atomic (Claim, Action,
//!               Locality, enums).
//!   locality  — best-effort `git` probe; never refuses.
//!   store     — single-table CRUD primitives.
//!   atomic    — multi-table atomic operation wrappers (pull / complete /
//!               fail / release / block), each one `BEGIN IMMEDIATE`-
//!               wrapped over the primitives plus task-status updates.
//!
//! Called from `planar-agent` (write paths) and from `planar` /
//! `planar-watch` (read paths via store.listActive / store.nextWork /
//! store.listByEntity).
//!
//! See:
//!   ~/.planar/workbench/.../58-agent-activity-tracking-tech-spec.md
//!   ~/.planar/workbench/.../59-agent-activity-tracking-roadmap.md § M1

pub const types = @import("agentactivity/types.zig");
pub const locality = @import("agentactivity/locality.zig");
pub const store = @import("agentactivity/store.zig");
pub const atomic = @import("agentactivity/atomic.zig");

test {
    _ = types;
    _ = locality;
    _ = store;
    _ = atomic;
}
