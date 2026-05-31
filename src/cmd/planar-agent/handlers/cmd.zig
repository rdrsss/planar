//! handlers/cmd — assembles the 13-verb `planar-agent` command surface.
//!
//! Imported by `src/cmd/planar-agent/main.zig` as the root tree's
//! `.cmds` slice. The handlers themselves live one file per verb under
//! this directory; this file is a flat registry.
//!
//! Verb set per tech-spec § "CLI surface → planar-agent":
//!   6 atomic ops    : pull, peek, complete, fail, release, block
//!   2 claim prims   : claim, heartbeat
//!   2 nested actions: action start, action end
//!   1 ingest        : ingest (M2 skeleton; full adapter routing in M4)
//!   2 op recovery   : reconcile, abort
//!
//! Total: 13 verbs.

const cli = @import("cli");

const version_h = @import("version.zig");
const pull_h = @import("pull.zig");
const peek_h = @import("peek.zig");
const complete_h = @import("complete.zig");
const fail_h = @import("fail.zig");
const release_h = @import("release.zig");
const block_h = @import("block.zig");
const claim_h = @import("claim.zig");
const heartbeat_h = @import("heartbeat.zig");
const action_cmd = @import("action/cmd.zig");
const ingest_h = @import("ingest.zig");
const reconcile_h = @import("reconcile.zig");
const abort_h = @import("abort.zig");
const schema_h = @import("schema.zig");

pub const verbs: []const cli.Cmd = &.{
    version_h.verb,
    pull_h.verb,
    peek_h.verb,
    complete_h.verb,
    fail_h.verb,
    release_h.verb,
    block_h.verb,
    claim_h.verb,
    heartbeat_h.verb,
    action_cmd.verb,
    ingest_h.verb,
    reconcile_h.verb,
    abort_h.verb,
    schema_h.verb,
};
