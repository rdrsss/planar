//! engine/workspace — workspace routing + AGENTS regeneration.

pub const routing = @import("workspace/routing.zig");
pub const regenerate = @import("workspace/regenerate.zig");

test {
    _ = routing;
    _ = regenerate;
}
