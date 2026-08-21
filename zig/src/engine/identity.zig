//! engine/identity — domain barrel: scope, association, workspace, project.
//!
//! These modules own the "who am I and what scope is my work in" layer:
//! parsing the cwd-derived scope, resolving project + association lookups,
//! workspace synthesis for polyrepo setups.

pub const scope = @import("identity/scope.zig");
pub const association = @import("identity/association.zig");
pub const project = @import("identity/project.zig");
pub const workspace = @import("identity/workspace.zig");

test {
    _ = scope;
    _ = association;
    _ = project;
    _ = workspace;
}
