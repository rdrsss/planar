//! engine/external/agentingest — vendor hook event ingestion.
//!
//! M4 lands the Claude adapter; M6 adds the GitHub Copilot adapter
//! without changing the interface. Layer organization:
//!
//!   interface.zig — normalized Event shape + ParseError contract
//!   claude.zig    — Claude Code hook payload → Event
//!   copilot.zig   — GitHub Copilot hook payload → Event (M6)
//!   dispatch.zig  — Event → engine.runtime.session + agentactivity.store
//!
//! Imported by `src/cmd/planar-agent/handlers/ingest.zig` to wire the
//! `planar-agent ingest --vendor <v> --event @<src>` verb.

pub const interface = @import("agentingest/interface.zig");
pub const claude = @import("agentingest/claude.zig");
pub const copilot = @import("agentingest/copilot.zig");
pub const dispatch = @import("agentingest/dispatch.zig");

test {
    _ = interface;
    _ = claude;
    _ = copilot;
    _ = dispatch;
}
