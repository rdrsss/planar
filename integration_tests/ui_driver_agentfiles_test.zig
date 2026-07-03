//! integration_tests/ui_driver_agentfiles_test.zig — structural guard for the
//! ui-driver worker role files.
//!
//! Asserts that the four files comprising the ui-driver role exist in the repo
//! and contain the substrings required by the role contract:
//!
//!   - agents/ui-driver.md     (canonical role spec)
//!   - skills/src/pl-ui-driver.md  (skill template; renders to all vendor surfaces)
//!
//! Both files must contain `ui-driver`. The files that reference the driver
//! scripts (canonical + skill template) must also contain `run.mjs` and
//! `synthesize.mjs`.
//!
//! The "four files" framing in the brief maps to this repo's actual structure:
//! the render system produces agents/claude/<role>.md, agents/codex/<role>.toml,
//! and agents/copilot/<role>.agent.md from the single skill template at build
//! time. Those rendered outputs are not hand-written files and are not asserted
//! here — agents_render_test.zig covers the render pipeline end-to-end.

const std = @import("std");
const harness = @import("harness");

/// Resolve the repo root from the compiled binary path.
/// Binary is at <repo>/zig-out/bin/planar; repo root is three dirs up.
fn repoRootFromBin(allocator: std.mem.Allocator, bin_path: []const u8) ![]u8 {
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return allocator.dupe(u8, d3);
}

fn readRepoFile(allocator: std.mem.Allocator, root: []const u8, rel: []const u8) ![]u8 {
    const full = try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(full);
    return std.Io.Dir.cwd().readFileAlloc(
        std.testing.io,
        full,
        allocator,
        std.Io.Limit.limited(1024 * 1024),
    ) catch |e| {
        std.debug.print(
            "\nui_driver_agentfiles: could not read '{s}': {s}\n",
            .{ full, @errorName(e) },
        );
        return e;
    };
}

fn assertContains(content: []const u8, needle: []const u8, file_rel: []const u8) !void {
    if (std.mem.indexOf(u8, content, needle) == null) {
        std.debug.print(
            "\nui_driver_agentfiles: '{s}' does not contain expected substring '{s}'\n",
            .{ file_rel, needle },
        );
        return error.SubstringNotFound;
    }
}

test "ui-driver canonical file exists and contains required substrings" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = try repoRootFromBin(gpa, suite.bin);
    defer gpa.free(root);

    const rel = "agents/ui-driver.md";
    const content = try readRepoFile(gpa, root, rel);
    defer gpa.free(content);

    try assertContains(content, "ui-driver", rel);
    try assertContains(content, "run.mjs", rel);
    try assertContains(content, "synthesize.mjs", rel);
}

test "ui-driver skill template exists and contains required substrings" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = try repoRootFromBin(gpa, suite.bin);
    defer gpa.free(root);

    const rel = "skills/src/pl-ui-driver.md";
    const content = try readRepoFile(gpa, root, rel);
    defer gpa.free(content);

    try assertContains(content, "ui-driver", rel);
    try assertContains(content, "run.mjs", rel);
    try assertContains(content, "synthesize.mjs", rel);
}

test "orchestrator + methodology document the Phase 3.6 ui-driver dispatch (P3c)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = try repoRootFromBin(gpa, suite.bin);
    defer gpa.free(root);

    // The orchestrator must document the phase, the role it dispatches, the
    // detection hook, and that a FAIL routes like a reviewer request-changes.
    const orch_rel = "agents/orchestrator.md";
    const orch = try readRepoFile(gpa, root, orch_rel);
    defer gpa.free(orch);
    try assertContains(orch, "Phase 3.6", orch_rel);
    try assertContains(orch, "ui-driver", orch_rel);
    try assertContains(orch, "pl-ui-driver-hook", orch_rel);
    try assertContains(orch, "request-changes", orch_rel);

    // The methodology carries the full-rules section the orchestrator links to.
    const meth_rel = "agents/methodology.md";
    const meth = try readRepoFile(gpa, root, meth_rel);
    defer gpa.free(meth);
    try assertContains(meth, "Phase 3.6", meth_rel);
    try assertContains(meth, "ui-driver", meth_rel);
    try assertContains(meth, "request-changes", meth_rel);
}
