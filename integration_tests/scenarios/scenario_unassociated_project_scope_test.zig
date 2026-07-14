//! Fresh-project scope guardrails: initialization tells the operator how to
//! establish an association, and plan creation never silently converts an
//! unassociated cwd-derived project into global scope.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    scope_kind: []const u8,
};

test "scenario: fresh project init explains how to create and add an association" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.tmpAbsPath();
    const init_out = suite.mustRunInDir(root, &.{
        "init", "--allow-no-repo", "--name", "fresh-project",
    });
    defer gpa.free(init_out);

    try std.testing.expect(std.mem.containsAtLeast(u8, init_out, 1, "planar assoc create"));
    try std.testing.expect(std.mem.containsAtLeast(u8, init_out, 1, "planar assoc add"));
}

test "scenario: unassociated project refuses implicit global plan but permits explicit global" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const root = suite.registerProject("fresh-project");

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = root },
    };
    const implicit = suite.execWithInDir(root, &.{
        "plan", "create", "Implicit global must be refused",
    }, &env);
    defer implicit.deinit(gpa);
    try std.testing.expect(implicit.term == .exited);
    try std.testing.expectEqual(@as(u8, 5), implicit.term.exited);
    try std.testing.expectEqual(@as(usize, 0), implicit.stdout.len);
    try std.testing.expect(std.mem.containsAtLeast(u8, implicit.stderr, 1, "project has no association"));
    try std.testing.expect(std.mem.containsAtLeast(u8, implicit.stderr, 1, "planar assoc create"));
    try std.testing.expect(std.mem.containsAtLeast(u8, implicit.stderr, 1, "planar assoc add"));
    try std.testing.expect(std.mem.containsAtLeast(u8, implicit.stderr, 1, "--scope global"));

    const after_refusal_raw = suite.mustRunInDir(root, &.{
        "plan", "list", "--scope", "global", "--json",
    });
    defer gpa.free(after_refusal_raw);
    const after_refusal = std.json.parseFromSlice([]PlanJSON, arena, after_refusal_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expectEqual(@as(usize, 0), after_refusal.value.len);

    const explicit_raw = suite.mustRunInDir(root, &.{
        "plan", "create", "--scope", "global", "--json", "Explicit global remains supported",
    });
    defer gpa.free(explicit_raw);
    const explicit = std.json.parseFromSlice(PlanJSON, arena, explicit_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expectEqualStrings("global", explicit.value.scope_kind);
}
