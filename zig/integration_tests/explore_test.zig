//! integration_tests/explore_test.zig — M1 cockpit entry + plain-fallback contract.
//!
//! Covers tasks 4007 (bare-planar-tty-gate), 4008 (explore-alias), and 4011
//! (plain-fallbacks). The TUI itself (task 4010) cannot be exercised in a
//! non-TTY test environment; the gate tests confirm that every fallback path
//! produces help output and exits 0.
//!
//! Gate contract (any of the following → fallback_help):
//!   - stdout is not a TTY  (always true in the test harness)
//!   - --plain flag
//!   - PLANAR_NO_TUI env var set
//!   - TERM=dumb

const std = @import("std");
const harness = @import("harness");

test "explore: verb is registered in root help" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // `planar --help` must list `explore` as a known verb.
    const out = suite.mustRun(&.{"--help"});
    defer gpa.free(out);
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "explore"));
}

test "explore: non-TTY stdout → falls back to help, exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // In the test harness stdout is never a TTY, so the gate always refuses.
    // `planar explore` must print the explore verb's help and exit 0.
    const out = suite.mustRun(&.{"explore"});
    defer gpa.free(out);
    // The help text must contain the verb name and at least one of the
    // canonical flag names.
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "explore"));
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "--plain"));
}

test "explore --plain: explicit plain flag → falls back to help, exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "explore", "--plain" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "explore"));
}

test "explore PLANAR_NO_TUI: env var set → falls back to help, exits 0" {
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

test "explore TERM=dumb: dumb terminal → falls back to help, exits 0" {
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

test "explore --help: explicit help flag prints usage, exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "explore", "--help" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "explore"));
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "--plan"));
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "--task"));
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "--scope"));
}

test "explore --plan / --task / --scope: flags parse and fall back cleanly, exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // In a non-TTY environment, seed flags are parsed but unused (M1 stub).
    // The command must exit 0 and produce help output — no parse error.
    const out = suite.mustRun(&.{ "explore", "--plan", "1", "--task", "42", "--scope", "test:proj" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.containsAtLeast(u8, out, 1, "explore"));
}
