//! Live-spawn integration test connection point for `planar-execute`'s
//! `agent()` host function (plan 492 M4 tasks 3175 + 3176 + 3178 + 3180).
//!
//! ## Honest framing
//!
//! This test is a CONNECTION POINT — the gate (`PLANAR_EXECUTE_LIVE_AGENT=1`)
//! triggers a real `claude --print ...` spawn, but the smoke body is
//! INTENTIONALLY UNIMPLEMENTED in this milestone. It returns
//! `error.SkipZigTest` whether the gate is set or not, with the gated path
//! falling through to an explicit unimplemented marker.
//!
//! The follow-up implementation work is tracked as **plan 497, task 3241**
//! (filed during cycle C iter 2 of plan 492). An operator with the
//! `claude` CLI and an active subscription fills it in for occasional
//! local verification; CI never runs it.
//!
//! ## Why this is deliberately deferred
//!
//! Real `claude -p` spawns cost API credit (~$0.10–$0.50 each) and cannot
//! run on CI. The contract surface that needs live coverage is narrow —
//! "the spawn happens, the worker can write a commit inside the worktree,
//! and the harness recognises it" — and is fully pinned by FakeSpawner
//! unit tests at the `planar-execute` crate level. The live path's only
//! purpose is to confirm that the FakeSpawner contract matches what
//! `claude --print` actually does on the operator's machine.
//!
//! ## Gate: PLANAR_EXECUTE_LIVE_AGENT=1
//!
//! When the env var is absent (the default — `make test-integration`
//! never sets it), the test returns `error.SkipZigTest`. CI never burns
//! credit. When the operator opts in:
//!
//!     PLANAR_EXECUTE_LIVE_AGENT=1 make test-integration
//!
//! the test currently still returns `error.SkipZigTest` with an explicit
//! marker — the smoke body has not been wired up yet. See plan 497 task
//! 3241 for the implementation outline (fixture plan + cycle worktree +
//! real driver spawn + post-spawn assertions on cycle-branch HEAD, task
//! status, and stranded-worktree absence).

const std = @import("std");
const harness = @import("harness");

fn envValue(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key) and s.len > key.len and s[key.len] == '=') {
            return s[key.len + 1 ..];
        }
    }
    return null;
}

test "planar-execute agent() LIVE spawn — connection point (smoke body deferred to plan 497 task 3241)" {
    // CI / make test-integration: skip silently. PLANAR_EXECUTE_LIVE_AGENT
    // is the opt-in gate; without it, return SkipZigTest immediately.
    if (envValue("PLANAR_EXECUTE_LIVE_AGENT") == null) return error.SkipZigTest;

    // Operator-gated path: the smoke body is NOT implemented yet. Tracked
    // as plan 497 task 3241 (filed cycle C iter 2 of plan 492). The
    // implementation will:
    //   1. Seed a fixture DB with a tiny plan + task.
    //   2. ensureEpic + createCycle; capture pre-spawn cycle-branch HEAD.
    //   3. Drive `driveAgentCall` via the real spawner + the
    //      `defaultEnvBuilder` (so PATH = shim, PLANAR_DB stripped, etc.).
    //   4. Wait for the spawn to complete.
    //   5. Assert: post-spawn cycle-branch HEAD != pre-spawn HEAD; task
    //      status == "done"; no active claims; no stranded worktrees.
    //   6. Teardown the fixture plan + repo.
    //
    // Until that lands, the gate's purpose is to refuse to silently pass
    // when the operator opts in. SkipZigTest with the explicit message
    // makes the deferred status visible.
    std.debug.print(
        "\nplanar_execute_agent_live_test: PLANAR_EXECUTE_LIVE_AGENT is set, but the smoke body is intentionally unimplemented. " ++
            "Tracked as plan 497 task 3241.\n",
        .{},
    );
    return error.SkipZigTest;
}
