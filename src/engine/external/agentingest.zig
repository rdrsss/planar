//! engine/external/agentingest — vendor hook event ingestion.
//!
//! M4 lands the Claude adapter; M6 adds a second vendor without
//! changing the interface. Layer organization:
//!
//!   interface.zig — normalized Event shape + ParseError contract
//!   claude.zig    — Claude Code hook payload → Event
//!   dispatch.zig  — Event → engine.runtime.session + agentactivity.store
//!
//! Imported by `src/cmd/planar-agent/handlers/ingest.zig` to wire the
//! `planar-agent ingest --vendor <v> --event @<src>` verb.

pub const interface = @import("agentingest/interface.zig");
pub const claude = @import("agentingest/claude.zig");
pub const dispatch = @import("agentingest/dispatch.zig");

test {
    _ = interface;
    _ = claude;
    _ = dispatch;
}
