//! integration_tests/scenarios/scenario_cockpit_entry_test.zig
//!
//! M19 documentation scenario: pins the non-TTY fallback contract for the
//! interactive cockpit and asserts planar-watch is byte-contract-unchanged.
//!
//! Verifies:
//!   - bare `planar` (no verb) in a NON-TTY context (always true under the
//!     harness) prints help/usage and exits 0 — does NOT hang, does NOT launch
//!     the TUI.
//!   - `planar explore` in the same non-TTY context falls back to the explore
//!     verb's help text and exits 0.
//!   - `PLANAR_NO_TUI=1` with `planar explore` still exits 0 with help text
//!     (belt-and-suspenders: env var suppression even if TTY were somehow true).
//!   - `TERM=dumb` with `planar explore` falls back to help, exits 0.
//!   - `--plain` with `planar explore` falls back to help, exits 0.
//!   - `planar-watch` bare invocation (no args → `feed`) exits with a
//!     schema-version guard failure (exit 7) OR outputs feed JSON/text on an
//!     initialized DB: i.e. the binary still starts, recognises its verbs, and
//!     its capability invariant is not disturbed. We confirm the binary is still
//!     a zero-write binary by checking that `--help` lists only the read-only
//!     verb set (feed, ps, claims, actions, plans, log, tree, run, version,
//!     completion, schema) and does NOT list any write verbs.
//!
//! The test harness always pipes stdout (never a TTY), so the cockpit cannot
//! activate under test — this is the correct condition for pinning the
//! non-TTY branch.

const std = @import("std");
const harness = @import("harness");

// -------------------------------------------------------------------------
// Helpers to resolve and invoke planar-watch.
// -------------------------------------------------------------------------

fn resolveWatchBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_WATCH_BIN=")) {
            return s["PLANAR_WATCH_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_WATCH_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

/// Run the planar-watch binary with the suite's isolated DB injected.
fn runWatch(
    suite: *const harness.Suite,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;
    const watch_bin = resolveWatchBin();

    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, watch_bin) catch @panic("OOM");
    for (args) |a| argv_list.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };

    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM injecting PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runWatch spawn failed: {s}", .{@errorName(e)});

    return .{
        .stdout = result.stdout,
        .stderr = result.stderr,
        .term = result.term,
    };
}

// =========================================================================
// Scenario: bare planar in non-TTY context prints help and exits 0.
//
// The harness always pipes stdout — it is never a TTY — so the cockpit
// gate must always fall back to help/usage here. This is the regression
// guard: bare `planar` must never hang waiting for a terminal.
// =========================================================================

test "cockpit-entry: bare planar (no verb) in non-TTY prints help, exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // No verb, no flags — bare invocation. In a non-TTY the gate falls back
    // to the root help text.
    const res = suite.exec(&.{});
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // Must exit 0 (not hang, not exit non-zero).
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u8, 0), res.term.exited);

    // The output must contain the root help/usage content: at minimum the
    // binary name and at least one known subcommand name.
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "planar"));
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "explore"));
}

// =========================================================================
// Scenario: planar explore in non-TTY falls back to explore verb help,
// exits 0.
// =========================================================================

test "cockpit-entry: planar explore in non-TTY falls back to explore help, exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Explicit alias: same gate contract as bare `planar`.
    const out = suite.mustRun(&.{"explore"});
    defer gpa.free(out);

    // The fallback prints the explore verb's help text.
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "explore"));
    // The explore verb's flags must appear (confirms it's the verb help, not
    // a crash or generic root help).
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "--plain"));
}

// =========================================================================
// Scenario: PLANAR_NO_TUI=1 with planar explore exits 0 with help.
// =========================================================================

test "cockpit-entry: PLANAR_NO_TUI forces fallback for planar explore" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = suite.execWith(&.{"explore"}, &.{
        .{ .key = "PLANAR_NO_TUI", .value = "1" },
    });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u8, 0), res.term.exited);
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "explore"));
}

// =========================================================================
// Scenario: TERM=dumb with planar explore exits 0 with help.
// =========================================================================

test "cockpit-entry: TERM=dumb forces fallback for planar explore" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = suite.execWith(&.{"explore"}, &.{
        .{ .key = "TERM", .value = "dumb" },
    });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u8, 0), res.term.exited);
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "explore"));
}

// =========================================================================
// Scenario: --plain flag with planar explore exits 0 with help.
// =========================================================================

test "cockpit-entry: --plain forces fallback for planar explore" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "explore", "--plain" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "explore"));
}

// =========================================================================
// Scenario: planar-watch capability invariant is unchanged.
//
// This test asserts that planar-watch's --help output:
//   a) lists its read-only verb set (feed, ps, claims, actions, plans, log,
//      tree, version, completion, schema — at least a representative subset)
//   b) does NOT list any write verbs that belong to the `planar` binary
//      (plan, task, question, decision, artifact, explore, init)
//
// This pins the "planar-watch is unchanged" half of the M19 acceptance
// criterion and prevents a future change from accidentally widening the
// watch binary's verb set.
// =========================================================================

test "cockpit-entry: planar-watch --help still lists only read-only verbs" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runWatch(&suite, &.{"--help"});
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // planar-watch --help must succeed (exit 0).
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u8, 0), res.term.exited);

    // Confirm known read-only verbs are present.
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "feed"));
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "ps"));
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "claims"));
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "actions"));
    try std.testing.expect(std.mem.containsAtLeast(u8, res.stdout, 1, "plans"));

    // Confirm the `explore` verb (cockpit entry) is NOT on planar-watch.
    // This is the load-bearing invariant: the cockpit lives in `planar`,
    // not in the read-only `planar-watch` binary.
    try std.testing.expect(!std.mem.containsAtLeast(u8, res.stdout, 1, "explore"));

    // Confirm other `planar`-only write verbs are also absent.
    // (These are sampled representatives of the write-verb set; the full
    // invariant is locked by capability_boundary_test.zig.)
    try std.testing.expect(!std.mem.containsAtLeast(u8, res.stdout, 1, "task add"));
    try std.testing.expect(!std.mem.containsAtLeast(u8, res.stdout, 1, "plan create"));
}
