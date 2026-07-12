//! engine — the Planar business-logic layer.
//!
//! Everything in this module is pure domain code: CRUD against the DB,
//! entity types, validation, business rules. It owns no IO writers, no
//! global state, and no CLI vocabulary. Callers (the `cmd/planar/`
//! handlers, integration tests) pass in `*db.sqlite.Db` and an allocator
//! explicitly; the engine returns values + errors and lets the caller do
//! the output.
//!
//! This separation makes the engine independently testable and keeps the
//! CLI handlers thin — they translate argv to engine calls, emit the
//! result, and handle errors.
//!
//! Module layout (Zig-stdlib-style barrel-at-parent + same-name folder):
//!
//!   engine/
//!   ├── root.zig            (this file — composes the namespace)
//!   ├── health.zig          (single-file domain — no subfolder)
//!   ├── identity.zig        ─┐
//!   ├── identity/            │  domain barrel + folder of impl files
//!   │   └── …                │
//!   ├── planning.zig        ─┤
//!   ├── planning/            │
//!   │   └── …                │
//!   └── …                   ─┘

pub const health = @import("health.zig");
pub const models = @import("models.zig");
pub const identity = @import("identity.zig");
pub const policy = @import("policy.zig");
pub const planning = @import("planning.zig");
pub const runs = @import("runs.zig");
pub const promotion = @import("promotion.zig");
pub const entitylink = @import("entitylink.zig");
pub const workbench = @import("workbench.zig");
pub const init = @import("init.zig");
pub const runtime = @import("runtime.zig");
pub const search = @import("search.zig");
pub const tree = @import("tree.zig");
pub const external = @import("external.zig");
pub const extsync = @import("extsync.zig");
pub const config = @import("config.zig");
pub const docs = @import("docs.zig");
pub const ingestor = @import("ingestor.zig");
pub const templates = @import("templates.zig");
pub const workspace = @import("workspace.zig");
pub const local = @import("local.zig");
pub const llm = @import("llm.zig");
pub const import = @import("import.zig");
pub const synthesize = @import("synthesize.zig");
pub const skillrender = @import("skillrender.zig");
pub const installedsurface = @import("installedsurface.zig");
pub const introspect = @import("introspect.zig");
pub const closure = @import("closure.zig");
pub const grouping = @import("grouping.zig");

// Pull every submodule into the test build so per-file `test` blocks
// are reachable from `zig build test`.
test {
    _ = health;
    _ = identity;
    _ = policy;
    _ = planning;
    _ = runs;
    _ = promotion;
    _ = entitylink;
    _ = workbench;
    _ = init;
    _ = runtime;
    _ = search;
    _ = tree;
    _ = external;
    _ = extsync;
    _ = config;
    _ = docs;
    _ = ingestor;
    _ = templates;
    _ = workspace;
    _ = local;
    _ = llm;
    _ = import;
    _ = synthesize;
    _ = skillrender;
    _ = installedsurface;
    _ = introspect;
    _ = closure;
    _ = grouping;
}
