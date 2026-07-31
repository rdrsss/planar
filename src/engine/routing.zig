//! engine.routing — adaptive routing persistence contracts.

pub const store = @import("routing/store.zig");
pub const packet = @import("routing/packet.zig");
pub const profile = @import("routing/profile.zig");
pub const evidence = @import("routing/evidence.zig");
pub const dispatch = @import("routing/dispatch.zig");
pub const ranking = @import("routing/ranking.zig");
pub const roles = @import("routing/roles.zig");
pub const views = @import("routing/views.zig");

test {
    _ = store;
    _ = packet;
    _ = profile;
    _ = evidence;
    _ = dispatch;
    _ = ranking;
    _ = roles;
    _ = views;
}
