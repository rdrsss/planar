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

test "models resolve never shows a tier without saying where it came from" {
    var suite = harness.Suite.init(std.testing.allocator);
    defer suite.deinit();
    const root = suite.registerProject("resolve-provenance");
    suite.addAssoc("resolve-provenance", null);

    const plan_out = suite.mustRunInDir(root, &.{ "plan", "create", "Resolve plan", "--json" });
    defer suite.allocator.free(plan_out);
    const task_out = suite.mustRunInDir(root, &.{ "task", "add", "--plan", "1", "--json", "bare task" });
    defer suite.allocator.free(task_out);

    // A bare task has an unready packet, so the tier MUST be reported as a
    // fallback with its reason — a tier shown without provenance reads
    // identically to one derived from real evidence.
    const out = suite.mustRunInDir(root, &.{ "models", "resolve", "--role", "coder", "--task", "1", "--json" });
    defer suite.allocator.free(out);
    try contains(out, "\"resolution_version\":\"routing-roles-v1\"");
    try contains(out, "\"packet_backed\":false");
    try contains(out, "\"fallback_reason\":\"packet_not_ready\"");
    // Nothing was derived, so neither classification may be asserted.
    try contains(out, "\"work_type\":null");
    try contains(out, "\"complexity\":null");

    // A pre-task role resolves from a planning packet, not a task profile.
    const planner = suite.mustRunInDir(root, &.{ "models", "resolve", "--role", "planner", "--json" });
    defer suite.allocator.free(planner);
    try contains(planner, "\"packet_class\":\"planning\"");
    try contains(planner, "\"fallback_reason\":\"no_packet\"");

    // An unknown role is refused rather than defaulted to something plausible.
    const bad = suite.execWithInDir(root, &.{ "models", "resolve", "--role", "wizard", "--task", "1" }, &.{});
    defer bad.deinit(suite.allocator);
    try std.testing.expect(bad.term.exited != 0);
}
