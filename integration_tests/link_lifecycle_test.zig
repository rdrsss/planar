//! integration_tests/link_lifecycle_test.zig
//!
//! Covers two previously-untested link surfaces:
//!   - the `links` entity-link verbs add / trail / remove (only list/remove
//!     were indexed and none exercised live), and
//!   - the top-level `link` / `unlink` external-link verbs (only their
//!     --help prose was checked).
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

const Id = struct { id: i64 };
const EntityLink = struct { id: i64, relationship: []const u8 };
const Trail = struct { id: i64, verb: []const u8, entity_kind: []const u8 };
const ExtLink = struct { link_id: i64, external_id: []const u8 };

test "scenario: entity-link add/trail/remove lifecycle" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("link-lifecycle");

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "Link plan" });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const t1 = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--json", "--plan", pid, "--next-action", "a", "Task one" });
    const t2 = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--json", "--plan", pid, "--next-action", "b", "Task two" });

    const from_ref = std.fmt.allocPrint(arena, "task:{d}", .{t1.id}) catch unreachable;
    const to_ref = std.fmt.allocPrint(arena, "task:{d}", .{t2.id}) catch unreachable;

    // add
    const link = suite.mustRunJSON(EntityLink, arena, &.{ "links", "add", from_ref, to_ref, "--relationship", "blocks", "--json" });
    try std.testing.expectEqualStrings("blocks", link.relationship);
    const lid = std.fmt.allocPrint(arena, "{d}", .{link.id}) catch unreachable;

    // trail — the audit row for the entity_links edge.
    const trail = suite.mustRunJSON(Trail, arena, &.{ "links", "trail", lid, "--json" });
    try std.testing.expectEqualStrings("entity_link", trail.entity_kind);

    // remove, then removing again must fail (idempotency boundary).
    const removed = suite.mustRunJSON(Id, arena, &.{ "links", "remove", lid, "--json" });
    try std.testing.expectEqual(link.id, removed.id);
    const miss = suite.expectFailure(&.{ "links", "remove", lid, "--json" });
    gpa.free(miss);
}

test "scenario: top-level link / unlink external-link lifecycle" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ext-link-lifecycle");

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "--json", "Ext link plan" });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "--json", "--plan", pid, "--next-action", "x", "Linked task" });
    const task_ref = std.fmt.allocPrint(arena, "task:{d}", .{task.id}) catch unreachable;

    // Register a GitHub system to link against (no network — registration
    // is a local row).
    gpa.free(suite.mustRun(&.{ "ext", "register", "github", "gh", "--project", "owner/repo" }));

    // link the task to an external issue.
    const linked = suite.mustRunJSON(ExtLink, arena, &.{ "link", task_ref, "--to", "gh:ISSUE-7", "--json" });
    try std.testing.expectEqualStrings("ISSUE-7", linked.external_id);
    const link_id = std.fmt.allocPrint(arena, "{d}", .{linked.link_id}) catch unreachable;

    // unlink it; unlinking a missing id must fail.
    const removed = suite.mustRunJSON(Id, arena, &.{ "unlink", link_id, "--json" });
    try std.testing.expectEqual(linked.link_id, removed.id);
    const miss = suite.expectFailure(&.{ "unlink", "999999", "--json" });
    gpa.free(miss);
}
