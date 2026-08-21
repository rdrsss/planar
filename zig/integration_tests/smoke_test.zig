//! integration_tests/smoke_test.zig — small migration/health smoke test.

const std = @import("std");
const harness = @import("harness");

test "health --json returns valid JSON with expected fields after migration" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // The health command's JSON shape (from engine/health.zig). After
    // Cluster C-health-content-loss (plan 351 Q235) the shape carries
    // Go's rich field set; the smoke test only asserts on the subset
    // it directly validates.
    const HealthReport = struct {
        db_ok: bool,
        schema_version: i64,
        migration_count: i64,
        pending_handoffs: i64,
    };

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const args: []const []const u8 = &.{ "--json", "health" };
    const report = suite.mustRunJSON(HealthReport, arena, args);

    // After migration the binary should have applied all embedded migrations.
    // schema_version ≥ 1 confirms at least one migration ran successfully.
    try std.testing.expect(report.schema_version >= 1);

    // migration_count should match schema_version (one row per migration).
    try std.testing.expect(report.migration_count >= 1);
    try std.testing.expectEqual(report.schema_version, report.migration_count);

    // Fresh DB has no pending handoffs.
    try std.testing.expectEqual(@as(i64, 0), report.pending_handoffs);

    // db_ok must be true on a healthy newly-migrated database.
    try std.testing.expect(report.db_ok);
}
