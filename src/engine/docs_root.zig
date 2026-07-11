//! DB-free engine surface linked by planar-doc.

pub const docs = struct {
    pub const merkle = @import("docs/merkle.zig");
    pub const manifest_v2 = @import("docs/manifest_v2.zig");
    pub const walk = @import("docs/walk.zig");
    pub const builder = @import("docs/builder.zig");
    pub const differ = @import("docs/differ.zig");
    pub const cover = @import("docs/cover.zig");
};

test {
    _ = docs.merkle;
    _ = docs.manifest_v2;
    _ = docs.walk;
    _ = docs.builder;
    _ = docs.differ;
    _ = docs.cover;
}
