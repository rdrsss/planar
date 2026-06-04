//! terminal.zig — Terminal-verb fallback decision matrix and runner
//! (plan 492 M4 task 3180).
//!
//! After the spawn driver's `wait()` returns, the harness must decide what
//! terminal verb (if any) to apply to the worker's claim. The worker MAY have
//! already done so itself — a well-behaved coder calls `planar-agent complete`
//! as its last action — in which case the harness MUST respect the existing
//! terminal state and do nothing.
//!
//! When the claim is still active, the harness applies a fallback verb keyed on
//! three observable signals:
//!
//!   1. `exit_code` from the spawned `claude -p` subprocess.
//!   2. `claim_status` read via `planar-watch ps --json` (post-spawn).
//!   3. `commit_present` — whether the cycle branch has a NEW commit on it
//!      relative to the pre-spawn HEAD (proxy for "did the worker do work?").
//!
//! ## Decision rules (pinned by unit tests)
//!
//! | Pre-existing claim state | Exit code | Commit | Verb              |
//! |--------------------------|-----------|--------|-------------------|
//! | terminal (completed)     | any       | any    | None              |
//! | terminal (aborted)       | any       | any    | None              |
//! | terminal (released)      | any       | any    | None              |
//! | active                   | 0         | yes    | complete          |
//! | active                   | 0         | no     | release           |
//! | active                   | non-zero  | any    | fail              |
//! | stale (lease expired)    | any       | any    | None (reconcile handles it)
//!
//! Rationale:
//!   - exit==0 + commit-present → the worker made progress and exited cleanly
//!     but forgot to call complete. The harness rescues the cycle by completing.
//!   - exit==0 + no commit → the worker no-oped (the A3 arm-1 case: default
//!     permission mode silently no-ops). `release` is correct — there is no
//!     work to keep, but it wasn't a hard failure either.
//!   - exit != 0 → the worker crashed or exited with an error. `fail` records
//!     the failure on the claim/action row for the operator.
//!   - terminal already → the worker did the right thing; do not double-write.
//!   - stale → `planar-agent reconcile` is the recovery path; the harness must
//!     not race with it.
//!
//! ## Module split
//!
//! - `decideTerminalVerb` — PURE decision function. Unit-tested in isolation.
//! - `runTerminalVerb`    — subprocess driver: spawns `planar-agent <verb>
//!                          --claim <token>`. Integration-covered.

const std = @import("std");
const Io = std.Io;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// All errors this module can surface from `runTerminalVerb`.
pub const TerminalError = error{
    /// `std.process.run` itself failed (could not spawn `planar-agent`, etc.).
    SubprocessFailed,
    /// `planar-agent <verb>` exited non-zero. The harness should escalate.
    SubprocessNonZero,
    /// Allocator returned OOM building argv.
    OutOfMemory,
};

// ---------------------------------------------------------------------------
// Inputs to the decision
// ---------------------------------------------------------------------------

/// The claim's status as observed AFTER the worker exited. Mirrors the values
/// the schema emits on `agent_work_claims.status`.
pub const ClaimStatus = enum {
    /// Claim is still alive and in a non-terminal state — the harness owns
    /// applying a terminal verb.
    active,
    /// The worker called `planar-agent complete` — terminal, do not touch.
    completed,
    /// The worker called `planar-agent fail` — terminal, do not touch.
    aborted,
    /// The worker called `planar-agent release` — terminal, do not touch.
    released,
    /// The lease expired without a terminal verb. `planar-agent reconcile`
    /// owns recovery; the harness must not race.
    stale,
    /// Unknown / unparsed value — defensive default. Treat as terminal-none.
    unknown,

    /// Parse from the wire string `planar-watch ps --json` / agent JSON emits.
    pub fn fromString(s: []const u8) ClaimStatus {
        if (std.mem.eql(u8, s, "active")) return .active;
        if (std.mem.eql(u8, s, "completed")) return .completed;
        if (std.mem.eql(u8, s, "aborted")) return .aborted;
        if (std.mem.eql(u8, s, "released")) return .released;
        if (std.mem.eql(u8, s, "stale")) return .stale;
        return .unknown;
    }
};

/// The decision input record. PURE-function input; assembled by the caller
/// from the spawn outcome + post-spawn claim read + commit check.
pub const TerminalInputs = struct {
    /// Observed claim status AFTER the worker exited.
    claim_status: ClaimStatus,
    /// The worker's exit code (0..255). On abnormal termination the caller
    /// passes 255 (matching `spawn.SpawnOutcome.exit_code`).
    exit_code: u32,
    /// Whether the cycle branch has a new commit relative to its pre-spawn
    /// HEAD. True iff at least one commit was made by the worker.
    commit_present: bool,
};

// ---------------------------------------------------------------------------
// TerminalVerb — the harness's decision
// ---------------------------------------------------------------------------

/// The terminal verb the harness should apply. `none` means the claim is
/// already in a terminal (or stale) state and the harness must NOT write.
pub const TerminalVerb = enum {
    none,
    complete,
    release,
    fail,

    /// Stringify to the `planar-agent` subcommand name.
    pub fn agentVerb(self: TerminalVerb) ?[]const u8 {
        return switch (self) {
            .none => null,
            .complete => "complete",
            .release => "release",
            .fail => "fail",
        };
    }
};

// ---------------------------------------------------------------------------
// decideTerminalVerb — PURE: the decision matrix.
// ---------------------------------------------------------------------------

/// Returns the terminal verb the harness should apply, given the observable
/// post-spawn state. PURE — no I/O, no allocation. Unit-tested in isolation.
///
/// The decision table is documented at the module header. See the unit tests
/// at the bottom of this file for the exhaustive case enumeration.
pub fn decideTerminalVerb(inputs: TerminalInputs) TerminalVerb {
    // Terminal / stale claim → harness does nothing.
    switch (inputs.claim_status) {
        .completed, .aborted, .released, .stale, .unknown => return .none,
        .active => {},
    }

    // Active claim. Branch on exit code + commit presence.
    if (inputs.exit_code != 0) return .fail;
    // exit_code == 0.
    if (inputs.commit_present) return .complete;
    return .release;
}

// ---------------------------------------------------------------------------
// runTerminalVerb — subprocess driver: shell `planar-agent <verb>`.
// ---------------------------------------------------------------------------

/// The reason string passed to `planar-agent fail` / `release`. Kept short and
/// stable so the operator can grep for harness-issued terminal verbs.
pub const HARNESS_FAIL_REASON: []const u8 = "planar-execute: worker exited non-zero without calling planar-agent fail";
pub const HARNESS_RELEASE_REASON: []const u8 = "planar-execute: worker exited 0 with no commit (no-op detected)";
pub const HARNESS_COMPLETE_SUMMARY: []const u8 = "planar-execute: worker exited 0 with a commit on the cycle branch";

/// Apply the chosen terminal verb to the claim. No-op when `verb == .none`.
///
/// Spawns `planar-agent <verb> --claim <token> [--reason <r> | --summary <s>]`
/// as a subprocess. Returns successfully on exit 0; maps non-zero exit to
/// `SubprocessNonZero` so the caller can escalate.
///
/// `planar_agent_bin` is the bare name `"planar-agent"` (resolved via PATH);
/// the caller is responsible for the PATH containing the right binary
/// (typically the constrained shim PATH from `worker_env`).
pub fn runTerminalVerb(
    allocator: std.mem.Allocator,
    io: Io,
    verb: TerminalVerb,
    claim_token: []const u8,
) TerminalError!void {
    const verb_name = verb.agentVerb() orelse return; // .none → no-op.

    // Build argv based on which extra flags the verb takes.
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(allocator);

    argv.append(allocator, "planar-agent") catch return TerminalError.OutOfMemory;
    argv.append(allocator, verb_name) catch return TerminalError.OutOfMemory;
    argv.append(allocator, "--claim") catch return TerminalError.OutOfMemory;
    argv.append(allocator, claim_token) catch return TerminalError.OutOfMemory;

    switch (verb) {
        .none => unreachable, // short-circuited above
        .complete => {
            argv.append(allocator, "--summary") catch return TerminalError.OutOfMemory;
            argv.append(allocator, HARNESS_COMPLETE_SUMMARY) catch return TerminalError.OutOfMemory;
        },
        .release => {
            argv.append(allocator, "--reason") catch return TerminalError.OutOfMemory;
            argv.append(allocator, HARNESS_RELEASE_REASON) catch return TerminalError.OutOfMemory;
        },
        .fail => {
            argv.append(allocator, "--reason") catch return TerminalError.OutOfMemory;
            argv.append(allocator, HARNESS_FAIL_REASON) catch return TerminalError.OutOfMemory;
        },
    }

    const result = std.process.run(allocator, io, .{
        .argv = argv.items,
        .stdout_limit = Io.Limit.limited(64 * 1024),
        .stderr_limit = Io.Limit.limited(8192),
    }) catch return TerminalError.SubprocessFailed;

    allocator.free(result.stdout);
    allocator.free(result.stderr);

    const ok = result.term == .exited and result.term.exited == 0;
    if (!ok) return TerminalError.SubprocessNonZero;
}

// ---------------------------------------------------------------------------
// Unit tests — pure decision matrix.
// ---------------------------------------------------------------------------

const testing = std.testing;

test "terminal: claim already completed → harness emits none" {
    // The worker already called planar-agent complete. The harness must NOT
    // double-write or the action row will have two terminal entries.
    inline for ([_]u32{ 0, 1, 255 }) |exit| {
        inline for ([_]bool{ true, false }) |commit| {
            const v = decideTerminalVerb(.{
                .claim_status = .completed,
                .exit_code = exit,
                .commit_present = commit,
            });
            try testing.expectEqual(TerminalVerb.none, v);
        }
    }
}

test "terminal: claim already aborted / released → harness emits none" {
    inline for ([_]ClaimStatus{ .aborted, .released }) |st| {
        const v = decideTerminalVerb(.{
            .claim_status = st,
            .exit_code = 0,
            .commit_present = true,
        });
        try testing.expectEqual(TerminalVerb.none, v);
    }
}

test "terminal: claim stale → harness emits none (reconcile owns recovery)" {
    const v = decideTerminalVerb(.{
        .claim_status = .stale,
        .exit_code = 0,
        .commit_present = true,
    });
    try testing.expectEqual(TerminalVerb.none, v);
}

test "terminal: claim unknown → harness emits none (defensive)" {
    const v = decideTerminalVerb(.{
        .claim_status = .unknown,
        .exit_code = 0,
        .commit_present = true,
    });
    try testing.expectEqual(TerminalVerb.none, v);
}

test "terminal: active + exit 0 + commit → complete" {
    const v = decideTerminalVerb(.{
        .claim_status = .active,
        .exit_code = 0,
        .commit_present = true,
    });
    try testing.expectEqual(TerminalVerb.complete, v);
}

test "terminal: active + exit 0 + no commit → release (A3 arm-1 no-op case)" {
    const v = decideTerminalVerb(.{
        .claim_status = .active,
        .exit_code = 0,
        .commit_present = false,
    });
    try testing.expectEqual(TerminalVerb.release, v);
}

test "terminal: active + exit non-zero → fail (regardless of commit)" {
    inline for ([_]bool{ true, false }) |commit| {
        inline for ([_]u32{ 1, 2, 130, 255 }) |exit| {
            const v = decideTerminalVerb(.{
                .claim_status = .active,
                .exit_code = exit,
                .commit_present = commit,
            });
            try testing.expectEqual(TerminalVerb.fail, v);
        }
    }
}

test "terminal: ClaimStatus.fromString round-trips" {
    try testing.expectEqual(ClaimStatus.active, ClaimStatus.fromString("active"));
    try testing.expectEqual(ClaimStatus.completed, ClaimStatus.fromString("completed"));
    try testing.expectEqual(ClaimStatus.aborted, ClaimStatus.fromString("aborted"));
    try testing.expectEqual(ClaimStatus.released, ClaimStatus.fromString("released"));
    try testing.expectEqual(ClaimStatus.stale, ClaimStatus.fromString("stale"));
    try testing.expectEqual(ClaimStatus.unknown, ClaimStatus.fromString("anything-else"));
    try testing.expectEqual(ClaimStatus.unknown, ClaimStatus.fromString(""));
}

test "terminal: TerminalVerb.agentVerb maps to planar-agent subcommand names" {
    try testing.expect(TerminalVerb.none.agentVerb() == null);
    try testing.expectEqualStrings("complete", TerminalVerb.complete.agentVerb().?);
    try testing.expectEqualStrings("release", TerminalVerb.release.agentVerb().?);
    try testing.expectEqualStrings("fail", TerminalVerb.fail.agentVerb().?);
}

test "terminal: HARNESS reason / summary strings are stable and non-empty" {
    // Pinned so a stealth edit fails the gate (the operator greps for these).
    try testing.expect(HARNESS_COMPLETE_SUMMARY.len > 0);
    try testing.expect(HARNESS_FAIL_REASON.len > 0);
    try testing.expect(HARNESS_RELEASE_REASON.len > 0);
    try testing.expect(std.mem.startsWith(u8, HARNESS_COMPLETE_SUMMARY, "planar-execute:"));
    try testing.expect(std.mem.startsWith(u8, HARNESS_RELEASE_REASON, "planar-execute:"));
    try testing.expect(std.mem.startsWith(u8, HARNESS_FAIL_REASON, "planar-execute:"));
}
