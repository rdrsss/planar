//! engine/external — operational-plane entities.
//!
//! M8 scope: external systems, external links, and sync state machine.

pub const system = @import("external/system.zig");
pub const link = @import("external/link.zig");
pub const sync = @import("external/sync.zig");

test {
    _ = system;
    _ = link;
    _ = sync;
}
