//! integration_tests/cwd_scope_test.zig
//!
//! Sentinel test for the cwd-derive harness primitives
//! (`Suite.registerProject` + `Suite.addAssoc` + `Suite.mustRunInDir`).
//!
//! Also serves as the first Bucket-1 parity test for the tree-scope
//! regression that shipped on M20 (commit bfa3abc): when `planar tree`
//! is run from a registered-project cwd with no `--scope` flag, it must
//! derive the scope from cwd rather than falling back to global-only
//! output. See Planar artifact 190 (Phase 1) for the rationale.
//!
//! Until this test existed, every integration test ran from a bare
//! `std.testing.tmpDir()` — cwd-derive returned null → global, and the
//! tree handler's missing scope-resolve call was indistinguishable from
//! correct behavior. This test fails (with the buggy tree.zig from M20)
//! because it asserts the assoc slug appears in the rendered root label
//! — i.e. the tree was scope-derived, not global-defaulted.

const std = @import("std");
const harness = @import("harness");

test "tree from registered-project cwd derives scope (would catch M20 bfa3abc regression)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Fixture: register the tmp dir as a Planar project, bind it to a
    // freshly-created association. After this, cwd-derive run from
    // inside the tmp must resolve to `cwd-scope-sentinel`.
    const root = suite.registerProject("cwd-scope-sentinel-project");
    suite.addAssoc("cwd-scope-sentinel", null);

    // Run `planar tree` from inside the registered tmp — NO --scope flag.
    // The fix from commit bfa3abc routes through `scope_mod.resolve(...)`
    // when --scope is absent; the regression skipped that and emitted the
    // global stub instead.
    const stdout = suite.mustRunInDir(root, &.{"tree"});
    defer gpa.free(stdout);

    // The buggy code path emitted a two-line `global` stub. The correct
    // code path emits a tree whose root label is `assoc:<slug>`.
    try std.testing.expect(std.mem.containsAtLeast(u8, stdout, 1, "assoc:cwd-scope-sentinel"));

    // Negative assertion: the global-only stub would contain "global" as
    // the root label and NOT the assoc slug. Guard against false positives
    // where the slug happens to leak in via some debug noise but the root
    // is still global.
    const first_line_end = std.mem.indexOfScalar(u8, stdout, '\n') orelse stdout.len;
    const first_line = stdout[0..first_line_end];
    try std.testing.expect(!std.mem.eql(u8, std.mem.trim(u8, first_line, " \t\r"), "global"));
}
