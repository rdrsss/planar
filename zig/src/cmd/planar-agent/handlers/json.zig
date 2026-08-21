//! handlers/json — re-exports of the shared JSON helpers.
//!
//! The canonical wire shapes for ClaimRow, ActionRow, and Task live in
//! `engine.runtime.agentactivity.json` so the `planar`, `planar-agent`,
//! and `planar-watch` binaries all render identically (locked by plan
//! 85 M3). This module is a thin re-export so existing planar-agent
//! handler call sites (`json.writeClaim(...)`) keep working without
//! reaching across the engine boundary.

const engine = @import("engine");
const shared_json = engine.runtime.agentactivity.json;

pub const writeClaim = shared_json.writeClaim;
pub const writeAction = shared_json.writeAction;
pub const writeTask = shared_json.writeTask;
