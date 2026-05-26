//! integration_tests/parity_health_test.zig
//!
//! Cluster C-health-content-loss parity tests for plan 351 (4 audit
//! rows). See parity-triage.md §C-health-content-loss and Q235.
//!
//! Q235 operator decision 2026-05-26: match Go. Restore the rich
//! field set (integrity_ok, inflight_tasks with resumable / not-
//! resumable breakdown, pending_handoffs with stale count, overall
//! classification) and the exit-1-on-DEGRADED behavior. Today zig
//! returns a minimal {schema_version, migration_count,
//! handoffs_pending, db_ok} shape and always exits 0 — oncall /
//! CI scrapes for degraded state are silently broken.
//!
//! Three test blocks pin the contract:
//! - Text output carries the labeled rich fields.
//! - JSON output carries the rich field set.
//! - exit-1 fires when a NOT-RESUMABLE in-flight task is present.

const std = @import("std");
const harness = @import("harness");

const TaskJSON = struct {
    id: i64,
};

test "parity: 'planar health' text output carries Go's rich field labels (Cluster C, Q235; red until restored)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const stdout = suite.mustRun(&.{"health"});
    defer gpa.free(stdout);

    // Go's text shape: every labeled line below appears.
    const required_labels = [_][]const u8{
        "db:",
        "schema:",
        "integrity:",
        "in-flight tasks:",
        "pending handoffs:",
        "overall:",
    };

    var missing: std.ArrayList([]const u8) = .empty;
    defer missing.deinit(gpa);
    inline for (required_labels) |label| {
        if (!std.mem.containsAtLeast(u8, stdout, 1, label)) {
            missing.append(gpa, label) catch @panic("OOM");
        }
    }

    if (missing.items.len > 0) {
        std.debug.print("\nhealth text missing {d} label(s):\n", .{missing.items.len});
        for (missing.items) |m| std.debug.print("  - '{s}'\n", .{m});
        std.debug.print("\nraw stdout:\n{s}\n", .{stdout});
        try std.testing.expect(false);
    }
}

test "parity: 'planar health --json' carries Go's rich field set (Cluster C, Q235; red until restored)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const stdout = suite.mustRun(&.{ "health", "--json" });
    defer gpa.free(stdout);

    const trimmed = std.mem.trim(u8, stdout, " \n");
    const parsed = std.json.parseFromSlice(std.json.Value, arena, trimmed, .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nhealth --json parse failed: {s}\nraw: {s}\n", .{ @errorName(e), stdout });
        try std.testing.expect(false);
        unreachable;
    };
    try std.testing.expect(parsed.value == .object);
    const obj = parsed.value.object;

    // Go's JSON field set per parity-triage.md §C.
    const required_keys = [_][]const u8{
        "db_path",
        "db_ok",
        "schema_current",
        "integrity_ok",
        "inflight_tasks",
        "resumable_tasks",
        "not_resumable_tasks",
        "pending_handoffs",
        "stale_handoffs",
        "overall",
    };

    var missing: std.ArrayList([]const u8) = .empty;
    defer missing.deinit(gpa);
    inline for (required_keys) |key| {
        if (obj.get(key) == null) {
            missing.append(gpa, key) catch @panic("OOM");
        }
    }

    if (missing.items.len > 0) {
        std.debug.print("\nhealth --json missing {d} key(s): ", .{missing.items.len});
        for (missing.items, 0..) |k, i| {
            if (i > 0) std.debug.print(", ", .{});
            std.debug.print("'{s}'", .{k});
        }
        std.debug.print("\nraw output: {s}\n", .{stdout});
        try std.testing.expect(false);
    }
}

test "parity: 'planar health' exits 1 when an in-flight NOT-RESUMABLE task exists (Cluster C, Q235; red until DEGRADED classification + exit code)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Create an in-flight task with no next_action → NOT RESUMABLE.
    const t = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--json", "HEALTH_DEGRADED_TASK",
    });
    const id_str = std.fmt.allocPrint(arena, "{d}", .{t.id}) catch unreachable;
    const upd_out = suite.mustRun(&.{ "task", "update", "--status", "doing", id_str });
    gpa.free(upd_out);

    const res = suite.exec(&.{"health"});
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    const actual_exit: i32 = if (res.term == .exited) @intCast(res.term.exited) else -1;
    if (actual_exit != 1) {
        std.debug.print(
            "\nhealth: expected exit 1 (DEGRADED — 1 in-flight not-resumable task), got {d}\nstdout: {s}\n",
            .{ actual_exit, res.stdout },
        );
        try std.testing.expect(false);
    }
}
