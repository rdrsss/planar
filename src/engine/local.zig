//! engine/local — local sandbox skills/agents discovery + install surfaces.

pub const manifest = @import("local/manifest.zig");
pub const link = @import("local/link.zig");
pub const import = @import("local/import.zig");

test {
    _ = manifest;
    _ = link;
    _ = import;
}
