//! cmd/planar/exit — unified domain-error → user-message + exit-code path.
//!
//! Handlers should not call `std.process.exit` directly with magic numbers.
//! `die(ctx, err, fmt, args)` writes a one-line error message to stderr,
//! flushes the runtime writers, and exits with the code that matches the
//! error category. This keeps the cli's exit-code contract in one place
//! and makes it easy to grep for which verbs raise which categories.
//!
//! Exit-code convention (mirrors Go side, src/internal/cperr/cperr.go):
//!   0  — success
//!   1  — generic failure (default for unmapped errors); also entity
//!        not found, matching Go's behavior and the canonical Unix
//!        not-found convention. Parity-triage §F-exit-code-not-found
//!        (plan 351) folded NotFound back into the generic-1 bucket so
//!        scripts can `|| exit 1` cleanly.
//!   2  — user-input failure (parse errors, bad flag values, …)
//!        (main.zig maps cli.Parse.* here directly; engine code raises
//!        InvalidInput / InvalidEntityRef which land here too)
//!   3  — sync conflict (Go cperr.ExitConflict)
//!   5  — scope violation (ScopeMismatch / cross-scope guard refusal)
//!   6  — conflict / precondition failure (SlugConflict, AlreadyExists, …)
//!   7  — schema version ahead of binary (SchemaVersionAhead; "stale binary")
//!   64 — not implemented yet (placeholder handlers)

const std = @import("std");
const runtime = @import("runtime");
const cli_log = @import("cli_log.zig");

/// Map a domain error to the exit code the operator's shell should see.
/// Unknown errors fall through to 1.
pub fn codeFor(err: anyerror) u8 {
    return switch (err) {
        error.NotImplemented => 64,
        error.InvalidEntityRef, error.InvalidInput => 2,
        error.Conflict => 3,
        error.ScopeMismatch => 5,
        error.SlugConflict, error.AlreadyExists => 6,
        // Fatal startup guard: DB was migrated by a newer binary.
        // Exit 7 — distinct from sync conflict exit 3 and from domain errors 4/5/6 so scripts
        // can distinguish "stale binary" cleanly.
        error.SchemaVersionAhead => 7,
        else => 1,
    };
}

/// Write a one-line `error: <fmt>` to stderr, run the full runtime
/// teardown (flush writers + close DB), and exit with the code that
/// matches `err`. Never returns.
///
/// Going through `runtime.shutdown` here matters because
/// `std.process.exit` skips deferred cleanup — without the explicit
/// call the DB handle would leak open and stderr could be left
/// half-written.
pub fn die(
    ctx: *const runtime.Ctx,
    err: anyerror,
    comptime fmt: []const u8,
    args: anytype,
) noreturn {
    ctx.stderr.print("error: " ++ fmt ++ "\n", args) catch {};
    // Capture the failed invocation (fail-open — any error is swallowed).
    // Use the start timestamp set in main.zig (0 if not yet set, which
    // means duration will be NULL in the recorded row, but that is safe).
    cli_log.record(codeFor(err), err, cli_log.startNs());
    runtime.shutdown();
    std.process.exit(codeFor(err));
}

/// Same as `die` but pre-fills the formatted message with the error
/// name. Use when the engine error already carries enough context that
/// a custom message would just repeat it.
pub fn dieErrName(ctx: *const runtime.Ctx, err: anyerror) noreturn {
    die(ctx, err, "{s}", .{@errorName(err)});
}
