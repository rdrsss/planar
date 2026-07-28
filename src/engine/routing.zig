//! engine.routing — adaptive routing persistence contracts.

pub const store = @import("routing/store.zig");
pub const packet = @import("routing/packet.zig");

test {
    _ = store;
    _ = packet;
}
