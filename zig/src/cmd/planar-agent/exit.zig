//! cmd/planar-agent/exit — domain-error → user-message + exit-code path.
//!
//! Mirrors `src/cmd/planar/exit.zig`. Kept as a small per-binary file so
//! each binary's main.zig can `@import("exit.zig")` without crossing
//! source-tree boundaries.
//!
//! Exit-code convention. NOT identical to `src/cmd/planar/exit.zig`'s, and
//! the difference is on `2` — see below.
//!   0  — success
//!   1  — generic failure, INCLUDING every CLI parse failure
//!   2  — semantic user-input failure: error.InvalidEntityRef and
//!        error.InvalidInput only
//!   7  — schema version mismatch (DB older than binary's embedded
//!        minimum, OR newer than binary's embedded max). Distinct from
//!        sync conflict (3) so scripts can detect "stale binary / DB
//!        drift" cleanly.
//!   64 — not implemented yet
//!
//! ## Why parse failures are 1 here and 2 on `planar` (task 6063)
//!
//! `src/cmd/planar/exit.zig` maps `cli.Parse.*` to 2 with an explicit arm.
//! `codeFor` below has NO Parse arm, so a parse failure falls through
//! `else => 1`. This file's header used to claim `2 — user-input failure
//! (cli.Parse.*)`, documenting an intent the code never implemented:
//!
//!     planar       task bogus                 -> exit 2
//!     planar-agent fail --reason x            -> exit 1  (missing --claim)
//!     planar-agent bogusverb                  -> exit 1
//!
//! Task 6063 resolved the disagreement in favour of the CODE, not the
//! comment. Adding the Parse arm would change an observable CLI contract
//! that every agent-facing caller of this binary already depends on, for
//! no behavioural gain; and the C++26 port is bound by D2 to preserve
//! behaviour, so it implements 1 (`src/cmd/planar-agent/exit.cppm`) and
//! must keep doing so. The per-binary difference on `2` is therefore
//! DELIBERATE and load-bearing, not drift to be tidied away later.

const std = @import("std");
const runtime = @import("runtime");

/// Map a domain error to the operator's shell exit code.
pub fn codeFor(err: anyerror) u8 {
    return switch (err) {
        error.NotImplemented => 64,
        error.InvalidEntityRef, error.InvalidInput => 2,
        error.SchemaVersionAhead, error.SchemaVersionBehind => 7,
        else => 1,
    };
}

/// Print `error: <fmt>` to stderr, tear down runtime, exit with the
/// mapped code. Never returns.
pub fn die(
    ctx: *const runtime.Ctx,
    err: anyerror,
    comptime fmt: []const u8,
    args: anytype,
) noreturn {
    ctx.stderr.print("error: " ++ fmt ++ "\n", args) catch {};
    runtime.shutdown();
    std.process.exit(codeFor(err));
}
