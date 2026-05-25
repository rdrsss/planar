//! engine/docs - outward-facing documentation support.

pub const lint = @import("docs/lint.zig");
pub const manifest = @import("docs/manifest.zig");
pub const promote = @import("docs/promote.zig");
pub const regenerate = @import("docs/regenerate.zig");
pub const queries = @import("docs/queries.zig");

test {
    _ = lint;
    _ = manifest;
    _ = promote;
    _ = regenerate;
    _ = queries;
}
