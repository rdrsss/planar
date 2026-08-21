//! integration_tests/scenarios/scenario_promote_demote_test.zig
//!
//! Scenario M11 of plan 352. Promote / demote: operator hoists a
//! personal-sandbox entity (global scope) into a registered
//! association, then reverses the move.
//!
//! Verbs exercised:
//!     init, assoc create, task add (global), promote, task show,
//!     demote.
//!
//! Verifies (roadmap slugs):
//!     [pd/promote-personal] — `promote task:<id> --to <assoc>`
//!     flips the task's scope_kind from "global" to "association",
//!     populates scope_id, returns the previous-scope JSON
//!     fields, and the post-state via `task show --json` matches.
//!     [pd/demote] — `demote task:<id>` reverses the promotion;
//!     scope_kind back to "global", scope_id null, previous
//!     fields capture the assoc.

const std = @import("std");
const harness = @import("harness");

const TaskJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    scope_kind: []const u8,
    scope_id: ?i64 = null,
};

const PromoteResult = struct {
    ok: bool,
    kind: []const u8,
    id: i64,
    scope_kind: []const u8,
    scope_id: ?i64 = null,
    previous_scope_kind: []const u8,
    previous_scope_id: ?i64 = null,
};

test "scenario: promote-demote — global task → association → back to global, round-trips" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("pd-flow");

    // ---- 1. Target association.
    const assoc_slug = "pd-target";
    const cr_out = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "project" });
    gpa.free(cr_out);

    // ---- 2. Global task (no --plan, no --scope → default
    // global personal scope).
    const t = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "--next-action", "do", "Personal task",
    });
    try std.testing.expectEqualStrings("global", t.scope_kind);
    try std.testing.expect(t.scope_id == null);

    const ref = std.fmt.allocPrint(arena, "task:{d}", .{t.id}) catch unreachable;
    const task_id_str = std.fmt.allocPrint(arena, "{d}", .{t.id}) catch unreachable;

    // ---- 3. Promote into the association.
    const promote = suite.mustRunJSON(PromoteResult, arena, &.{
        "promote", ref, "--to", assoc_slug, "--json",
    });
    try std.testing.expect(promote.ok);
    try std.testing.expectEqualStrings("task", promote.kind);
    try std.testing.expectEqualStrings("association", promote.scope_kind);
    try std.testing.expect(promote.scope_id != null);
    try std.testing.expectEqualStrings("global", promote.previous_scope_kind);

    // task show round-trips the post-promote state.
    const t_promoted = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "show", task_id_str, "--json",
    });
    try std.testing.expectEqualStrings("association", t_promoted.scope_kind);
    try std.testing.expect(t_promoted.scope_id != null);
    try std.testing.expectEqual(promote.scope_id.?, t_promoted.scope_id.?);

    // ---- 4. Demote back to global.
    const demote = suite.mustRunJSON(PromoteResult, arena, &.{
        "demote", ref, "--json",
    });
    try std.testing.expect(demote.ok);
    try std.testing.expectEqualStrings("global", demote.scope_kind);
    try std.testing.expect(demote.scope_id == null);
    try std.testing.expectEqualStrings("association", demote.previous_scope_kind);
    try std.testing.expect(demote.previous_scope_id != null);

    // task show post-demote returns global.
    const t_demoted = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "show", task_id_str, "--json",
    });
    try std.testing.expectEqualStrings("global", t_demoted.scope_kind);
    try std.testing.expect(t_demoted.scope_id == null);
}
