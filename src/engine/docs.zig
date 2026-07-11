//! engine/docs - outward-facing documentation support.

// Plan 423 document-manifest implementation. The planar-doc build links the
// narrower docs_root.zig surface so legacy workspace manifest modules remain
// available to the operator engine without pulling SQLite into planar-doc.
pub const merkle = @import("docs/merkle.zig");
pub const manifest_v2 = @import("docs/manifest_v2.zig");
pub const walk = @import("docs/walk.zig");
pub const builder = @import("docs/builder.zig");
pub const differ = @import("docs/differ.zig");
pub const cover = @import("docs/cover.zig");

test {
    _ = merkle;
    _ = manifest_v2;
    _ = walk;
    _ = builder;
    _ = differ;
    _ = cover;
}
