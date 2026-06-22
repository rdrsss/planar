//! integration_tests/scenarios/scenario_annotations_test.zig
//!
//! Scenario M6 of plan 352 (no longer deferred — task 2452's engine
//! impl landed). Annotations are line-anchored notes on code with a
//! four-state lifecycle (active → resolved / dismissed / archived);
//! annotation is a zig-only addition relative to the Go archive.
//!
//! Verbs exercised:
//!     init, plan create, annotate add (×N, with anchor + tags),
//!     annotate list (with status filters), annotate show,
//!     annotate update, annotate tag (add + --remove), annotate
//!     resolve / dismiss / archive (terminal transitions), annotate
//!     bulk-resolve / bulk-dismiss / bulk-archive (filtered),
//!     annotate sweep (older-than-days), annotate verify (file
//!     hash check), annotate remove.
//!
//! Verifies (roadmap slugs):
//!     [an/annotate-add] [an/annotate-list-update] [an/annotate-tag]
//!     [an/annotate-resolve-dismiss-archive] [an/annotate-bulk]
//!     [an/annotate-sweep]

const std = @import("std");
const harness = @import("harness");

const AnnotationJSON = struct {
    id: i64,
    title: ?[]const u8 = null,
    body: []const u8,
    status: []const u8,
    tags: []const []const u8 = &.{},
};

const BulkResult = struct {
    ok: bool,
    action: []const u8,
    count: i64,
};

const SweepResult = struct {
    ok: bool,
    swept: i64,
};

// =========================================================================
// Primary lifecycle: add → list filters → resolve / dismiss / archive
// =========================================================================

test "scenario: annotations — add, list-by-status, tag add/remove, lifecycle transitions" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-flow");

    // ---- 1. Add three annotations against different anchor paths.
    const a1 = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate",        "add",                 "--json",
        "--anchor-path",   "src/foo.zig",         "--line-start",
        "10",              "--title",             "foo rename",
        "--body",          "symbol name unclear", "--tags",
        "refactor,review",
    });
    try std.testing.expectEqualStrings("active", a1.status);
    try std.testing.expectEqual(@as(usize, 2), a1.tags.len);

    const a2 = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate",      "add",         "--json",
        "--anchor-path", "src/bar.zig", "--body",
        "TODO: cleanup",
    });
    const a3 = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate",        "add",         "--json",
        "--anchor-path",   "src/baz.zig", "--body",
        "deprecated path",
    });

    const id1_str = std.fmt.allocPrint(arena, "{d}", .{a1.id}) catch unreachable;
    const id2_str = std.fmt.allocPrint(arena, "{d}", .{a2.id}) catch unreachable;
    const id3_str = std.fmt.allocPrint(arena, "{d}", .{a3.id}) catch unreachable;

    // ---- 2. List default = active. All three surface.
    const list_raw = suite.mustRun(&.{ "annotate", "list", "--json" });
    defer gpa.free(list_raw);
    try std.testing.expect(std.mem.containsAtLeast(u8, list_raw, 1, "foo rename"));
    try std.testing.expect(std.mem.containsAtLeast(u8, list_raw, 1, "src/bar.zig"));
    try std.testing.expect(std.mem.containsAtLeast(u8, list_raw, 1, "src/baz.zig"));

    // ---- 3. tag --add "extra", verify, --remove "review".
    const tag_add_out = suite.mustRun(&.{ "annotate", "tag", id1_str, "extra", "--json" });
    gpa.free(tag_add_out);
    const tag_rm_out = suite.mustRun(&.{ "annotate", "tag", id1_str, "review", "--remove", "--json" });
    gpa.free(tag_rm_out);

    const a1_after = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "show", id1_str, "--json",
    });
    var saw_extra = false;
    var saw_review = false;
    for (a1_after.tags) |t| {
        if (std.mem.eql(u8, t, "extra")) saw_extra = true;
        if (std.mem.eql(u8, t, "review")) saw_review = true;
    }
    try std.testing.expect(saw_extra);
    try std.testing.expect(!saw_review);

    // ---- 4. Terminal-state transitions.
    const r1 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "resolve", id1_str, "--json" });
    try std.testing.expectEqualStrings("resolved", r1.status);

    const d2 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "dismiss", id2_str, "--json" });
    try std.testing.expectEqualStrings("dismissed", d2.status);

    const ar3 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "archive", id3_str, "--json" });
    try std.testing.expectEqualStrings("archived", ar3.status);

    // ---- 5. update — change title.
    const updated = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "update",                   id1_str, "--json",
        "--title",  "foo rename (post-review)",
    });
    try std.testing.expect(updated.title != null);
    try std.testing.expectEqualStrings("foo rename (post-review)", updated.title.?);

    // ---- 6. List --status filters work.
    const resolved_list = suite.mustRun(&.{ "annotate", "list", "--status", "resolved", "--json" });
    defer gpa.free(resolved_list);
    try std.testing.expect(std.mem.containsAtLeast(u8, resolved_list, 1, "post-review"));

    const dismissed_list = suite.mustRun(&.{ "annotate", "list", "--status", "dismissed", "--json" });
    defer gpa.free(dismissed_list);
    try std.testing.expect(std.mem.containsAtLeast(u8, dismissed_list, 1, "TODO: cleanup"));

    const archived_list = suite.mustRun(&.{ "annotate", "list", "--status", "archived", "--json" });
    defer gpa.free(archived_list);
    try std.testing.expect(std.mem.containsAtLeast(u8, archived_list, 1, "deprecated path"));

    // ---- 7. remove — delete one row.
    const rm_out = suite.mustRun(&.{ "annotate", "remove", id3_str, "--json" });
    defer gpa.free(rm_out);
    try std.testing.expect(std.mem.containsAtLeast(u8, rm_out, 1, "\"ok\":true"));

    // Confirm gone.
    const after_remove = suite.expectFailure(&.{ "annotate", "show", id3_str });
    defer gpa.free(after_remove);
    try std.testing.expect(std.mem.containsAtLeast(u8, after_remove, 1, "no annotation"));
}

// =========================================================================
// Bulk operations + sweep
// =========================================================================

test "scenario: annotations — bulk-resolve filter, bulk-archive any-status, sweep older-than" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-bulk");

    // Seed 5 annotations, three tagged "noise" + two tagged "keeper".
    var i: usize = 0;
    while (i < 3) : (i += 1) {
        const body = std.fmt.allocPrint(arena, "noise body {d}", .{i}) catch unreachable;
        _ = suite.mustRunJSON(AnnotationJSON, arena, &.{
            "annotate",      "add",       "--json",
            "--anchor-path", "src/x.zig", "--body",
            body,            "--tags",    "noise",
        });
    }
    i = 0;
    while (i < 2) : (i += 1) {
        const body = std.fmt.allocPrint(arena, "keeper {d}", .{i}) catch unreachable;
        _ = suite.mustRunJSON(AnnotationJSON, arena, &.{
            "annotate",      "add",       "--json",
            "--anchor-path", "src/x.zig", "--body",
            body,            "--tags",    "keeper",
        });
    }

    // bulk-resolve filtered to tag=noise.
    const br = suite.mustRunJSON(BulkResult, arena, &.{
        "annotate", "bulk-resolve", "--json", "--tag", "noise",
    });
    try std.testing.expect(br.ok);
    try std.testing.expectEqualStrings("resolved", br.action);
    try std.testing.expectEqual(@as(i64, 3), br.count);

    // keepers are still active.
    const active_after = suite.mustRun(&.{ "annotate", "list", "--status", "active", "--tag", "keeper", "--json" });
    defer gpa.free(active_after);
    try std.testing.expect(std.mem.containsAtLeast(u8, active_after, 1, "keeper 0"));
    try std.testing.expect(std.mem.containsAtLeast(u8, active_after, 1, "keeper 1"));

    // bulk-archive across ALL annotations. Under the retention-tier model
    // (plan 692), resolved → archived is legal, so the 3 previously-resolved
    // noise rows ARE included. Together with the 2 still-active keeper rows,
    // bulk-archive archives all 5 rows. (The noise rows go resolved→archived;
    // the keeper rows go active→archived.)
    const ba = suite.mustRunJSON(BulkResult, arena, &.{ "annotate", "bulk-archive", "--json" });
    try std.testing.expect(ba.ok);
    try std.testing.expectEqualStrings("archived", ba.action);
    try std.testing.expectEqual(@as(i64, 5), ba.count);

    // sweep with since-days=0: all resolved/dismissed rows are now archived
    // (we just bulk-archived them above), so the WHERE clause selects 0 rows
    // and swept=0. This confirms sweep is harmlessly idempotent when there is
    // nothing left to archive.
    const sw = suite.mustRunJSON(SweepResult, arena, &.{
        "annotate", "sweep", "--json", "--since-days", "0",
    });
    try std.testing.expect(sw.ok);
    try std.testing.expectEqual(@as(i64, 0), sw.swept);
}

// Retention-tier model (plan 692): sweep archives resolved + dismissed rows.
//
// `archived` is the single final retention state.  `resolved` and `dismissed`
// are outcome states that may progress to `archived`.  This test seeds a
// resolved row and a dismissed row, then confirms sweep (since-days=0)
// archives both and leaves them in the `archived` state.
test "scenario: annotate sweep archives resolved and dismissed rows (retention-tier)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("ann-sweep-retention");

    // Seed one resolved annotation and one dismissed annotation.
    const a1 = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/r.zig", "--body", "resolved note",
    });
    const a2 = suite.mustRunJSON(AnnotationJSON, arena, &.{
        "annotate", "add", "--json", "--anchor-path", "src/d.zig", "--body", "dismissed note",
    });
    const id1 = std.fmt.allocPrint(arena, "{d}", .{a1.id}) catch unreachable;
    const id2 = std.fmt.allocPrint(arena, "{d}", .{a2.id}) catch unreachable;

    gpa.free(suite.mustRun(&.{ "annotate", "resolve", id1 }));
    gpa.free(suite.mustRun(&.{ "annotate", "dismiss", id2 }));

    // Confirm pre-state.
    const pre1 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id1, "--json" });
    try std.testing.expectEqualStrings("resolved", pre1.status);
    const pre2 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id2, "--json" });
    try std.testing.expectEqualStrings("dismissed", pre2.status);

    // Sweep with since-days=0: both rows qualify (resolved + dismissed,
    // updated_at is 0 days old which satisfies > 0 — use 0 to select all).
    // Under the retention-tier model, resolved→archived and dismissed→archived
    // are legal, so sweep should archive both rows (swept=2).
    const sw = suite.mustRunJSON(SweepResult, arena, &.{
        "annotate", "sweep", "--json", "--since-days", "0",
    });
    try std.testing.expect(sw.ok);
    try std.testing.expectEqual(@as(i64, 2), sw.swept);

    // Confirm post-state: both rows are now archived.
    const post1 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id1, "--json" });
    try std.testing.expectEqualStrings("archived", post1.status);
    const post2 = suite.mustRunJSON(AnnotationJSON, arena, &.{ "annotate", "show", id2, "--json" });
    try std.testing.expectEqualStrings("archived", post2.status);
}
