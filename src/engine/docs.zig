//! engine/docs - outward-facing documentation support.

pub const lint = @import("docs/lint.zig");
pub const manifest = @import("docs/manifest.zig");
pub const promote = @import("docs/promote.zig");
pub const regenerate = @import("docs/regenerate.zig");
pub const queries = @import("docs/queries.zig");

// Plan 423 (Documenter) v2 modules. Plan 423 M6 deletes the v1 manifest +
// the lint / promote / regenerate / queries handlers; until then v1 and v2
// co-exist so the build stays green per-milestone.
pub const merkle = @import("docs/merkle.zig");
pub const manifest_v2 = @import("docs/manifest_v2.zig");
pub const walk = @import("docs/walk.zig");
pub const builder = @import("docs/builder.zig");
pub const differ = @import("docs/differ.zig");
pub const cover = @import("docs/cover.zig");

test {
    _ = lint;
    _ = manifest;
    _ = promote;
    _ = regenerate;
    _ = queries;
    _ = merkle;
    _ = manifest_v2;
    _ = walk;
    _ = builder;
    _ = differ;
    _ = cover;
}
