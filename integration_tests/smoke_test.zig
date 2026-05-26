//! integration_tests/smoke_test.zig — integration test root.
//!
//! This file is the root source for the integration test executable. Each
//! subsystem's integration tests live in a sibling file; import them here so
//! the Zig test runner discovers their `test` blocks.
//!
//! Run via: zig build test-integration

const std = @import("std");
const harness = @import("harness");

// M4 Cycle B: view/edit handler integration tests.
comptime {
    _ = @import("editflow_view_test.zig");
    _ = @import("editflow_diff_review_test.zig");
    _ = @import("editflow_edit_test.zig");
    _ = @import("editor_add_test.zig");
    _ = @import("editor_validation_test.zig");
    _ = @import("plan_update_test.zig");
}

// M11: search (FTS5) handler integration tests.
comptime {
    _ = @import("search_test.zig");
}

// M12: tree hierarchical drill-down integration tests.
comptime {
    _ = @import("tree_test.zig");
}

// Plan 351 (parity-gap-tests), task 2358 — cwd-derive fixture harness
// + sentinel test for the tree-scope regression that shipped on M20
// (commit bfa3abc). See Planar artifact 190 § Phase 1.
comptime {
    _ = @import("cwd_scope_test.zig");
}

// M14: config ~/.planar/config.toml integration tests.
comptime {
    _ = @import("config_test.zig");
}

// M16 Cycle B: plan step, promote/demote, recompute-status handler integration tests.
comptime {
    _ = @import("m16_test.zig");
}

// M15: workspace routing/regenerate handler integration tests.
comptime {
    _ = @import("workspace_test.zig");
    _ = @import("workbench_test.zig");
}

// M17: local sandbox skills/agents handlers.
comptime {
    _ = @import("local_test.zig");
}

// M18: pl-import / pl-synthesize cache-handshake parity tests.
comptime {
    _ = @import("m18_import_synthesize_test.zig");
    _ = @import("scope_test.zig");
    _ = @import("ext_sync_test.zig");
    _ = @import("parent_issue_test.zig");
    _ = @import("spec_ingest_test.zig");
    _ = @import("skills_render_test.zig");
}

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
