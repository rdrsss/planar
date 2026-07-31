//! Black-box contract for `planar models experiments|outcomes` — the read-only
//! windows onto the routing evidence plane (plan 950 task 5531).
//!
//! The invariant these pin: an exclusion is always NAMED. A recorded run that
//! does not count toward a recommendation is the most informative row in the
//! table — it says a dispatch happened, was kept, and was deliberately set
//! aside. Hiding it understates the evidence; showing it without a reason is
//! indistinguishable from a bug.

const std = @import("std");
const harness = @import("harness");

fn contains(haystack: []const u8, needle: []const u8) !void {
    try std.testing.expect(std.mem.indexOf(u8, haystack, needle) != null);
}

test "an empty evidence plane reports nothing rather than failing" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const root = suite.registerProject("views-empty");

    const experiments = suite.mustRunInDir(root, &.{ "models", "experiments" });
    defer suite.allocator.free(experiments);
    try contains(experiments, "no declared routing experiments");

    const outcomes = suite.mustRunInDir(root, &.{ "models", "outcomes" });
    defer suite.allocator.free(outcomes);
    try contains(outcomes, "no recorded terminal outcomes");
}

test "both views are versioned in JSON so a consumer can tell the shape apart" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const root = suite.registerProject("views-json");

    const experiments = suite.mustRunInDir(root, &.{ "models", "experiments", "--json" });
    defer suite.allocator.free(experiments);
    try contains(experiments, "\"views_version\":\"routing-views-v1\"");
    try contains(experiments, "\"experiments\":[]");

    const outcomes = suite.mustRunInDir(root, &.{ "models", "outcomes", "--json" });
    defer suite.allocator.free(outcomes);
    try contains(outcomes, "\"views_version\":\"routing-views-v1\"");
    try contains(outcomes, "\"outcomes\":[]");
}

test "outcomes rejects a nonsense limit instead of silently showing everything" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const root = suite.registerProject("views-limit");

    // Silently treating 0 or a non-number as "no limit" would show an operator
    // far more than they asked for and look like the flag was honoured.
    for ([_][]const u8{ "0", "-3", "many" }) |bad| {
        const res = suite.execWithInDir(root, &.{ "models", "outcomes", "--limit", bad }, &.{});
        defer res.deinit(suite.allocator);
        try std.testing.expect(res.term.exited != 0);
    }

    const ok = suite.mustRunInDir(root, &.{ "models", "outcomes", "--limit", "5" });
    defer suite.allocator.free(ok);
}
