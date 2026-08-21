//! integration_tests/parity_exit_code_not_found_test.zig
//!
//! Cluster F-exit-code-not-found parity test for plan 351 (3 rows).
//! See parity-triage.md §F-exit-code-not-found.
//!
//! Three audit rows show zig returning a non-1 exit code on
//! "entity not found" errors where Go returns 1. Exit-code
//! consistency is part of the script contract; pinning to 1
//! (the canonical Unix not-found convention) keeps scripts stable.
//!
//! Today: all three sub-assertions fail. Phase 4 fix likely lives
//! in src/cmd/planar/exit.zig (uniform error-to-exit mapping).

const std = @import("std");
const harness = @import("harness");

test "parity: 'entity not found' errors return exit code 1 (Cluster F-exit-code-not-found; red until exit mapping fixed)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const Case = struct {
        label: []const u8,
        args: []const []const u8,
    };
    const cases = [_]Case{
        .{ .label = "test-spec status 99999", .args = &.{ "test-spec", "status", "99999" } },
        .{ .label = "resume 99999", .args = &.{ "resume", "99999" } },
        .{ .label = "resume --json 99999", .args = &.{ "resume", "--json", "99999" } },
    };

    var failures: std.ArrayList([]const u8) = .empty;
    defer {
        for (failures.items) |f| gpa.free(f);
        failures.deinit(gpa);
    }

    inline for (cases) |case| {
        const res = suite.exec(case.args);
        defer gpa.free(res.stdout);
        defer gpa.free(res.stderr);

        const actual_exit: i32 = if (res.term == .exited) @intCast(res.term.exited) else -1;
        if (actual_exit != 1) {
            const msg = std.fmt.allocPrint(gpa, "{s}: expected exit 1, got {d}", .{ case.label, actual_exit }) catch @panic("OOM");
            failures.append(gpa, msg) catch @panic("OOM");
        }
    }

    if (failures.items.len > 0) {
        std.debug.print("\n{d} not-found exit-code(s) diverged from canonical 1:\n", .{failures.items.len});
        for (failures.items) |f| std.debug.print("  - {s}\n", .{f});
        try std.testing.expect(false);
    }
}
