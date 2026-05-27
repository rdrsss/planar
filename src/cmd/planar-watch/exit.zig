//! cmd/planar-watch/exit — domain-error → user-message + exit-code path.
//!
//! Mirrors `src/cmd/planar-agent/exit.zig`. Kept as a small per-binary
//! file so each binary's main.zig can `@import("exit.zig")` without
//! crossing source-tree boundaries.
//!
//! Exit-code convention matches the operator binary's:
//!   0  — success
//!   1  — generic failure
//!   2  — user-input failure (cli.Parse.*)
//!   7  — schema version mismatch (DB older than binary's embedded
//!        minimum, OR newer than binary's embedded max). Same code
//!        `planar-agent` uses so script-level handling is uniform
//!        across the three binaries.
//!   64 — not implemented yet
//!   130 — SIGINT (the standard "killed by SIGINT" exit code; emitted
//!        on graceful `--follow` shutdown).

const std = @import("std");
const runtime = @import("runtime");

/// Map a domain error to the shell exit code.
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
