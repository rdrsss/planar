//! handlers/cmd — assembles the `planar-watch` read-only verb tree.
//!
//! Imported by `src/cmd/planar-watch/main.zig` as the root tree's
//! `.cmds` slice. Per the tech-spec § "CLI surface → planar-watch":
//! exactly six read verbs plus the conventional `version` /
//! `completion` helpers. There is no write verb anywhere in this
//! tree — that is the FIRST line of defense behind this binary's
//! capability boundary (the SECOND is the strict-read-only DB
//! handle).
//!
//! Verb set:
//!   - feed       — cross-cutting activity feed (default invocation).
//!   - ps         — active + stale claim snapshot.
//!   - claims     — list claims with status filter.
//!   - actions    — list actions with kind / entity filters.
//!   - plans      — plans with in-flight work.
//!   - log        — per-entity / per-claim history (union of
//!                  actions + claim transitions).
//!   - version    — print binary version (no DB touch).
//!   - completion — shell autocompletion script (no DB touch).

const cli = @import("cli");

const version_h = @import("version.zig");
const completion_h = @import("completion.zig");
const schema_h = @import("schema.zig");
const feed_h = @import("feed.zig");
const ps_h = @import("ps.zig");
const claims_h = @import("claims.zig");
const actions_h = @import("actions.zig");
const plans_h = @import("plans.zig");
const log_h = @import("log.zig");

pub const verbs: []const cli.Cmd = &.{
    feed_h.verb,
    ps_h.verb,
    claims_h.verb,
    actions_h.verb,
    plans_h.verb,
    log_h.verb,
    version_h.verb,
    completion_h.verb,
    schema_h.verb,
};
