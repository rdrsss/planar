//! integration_tests/parity_bare_parent_help_test.zig
//!
//! Cluster A parity tests for plan 351. See parity-triage.md
//! §A-bare-parent-verb (84 audit rows across 21 parent verbs × 4 bare
//! invocations) and question 234 for the operator decision.
//!
//! Operator decision 2026-05-26 (Q234): match Go. When a parent verb
//! is invoked with no subcommand (e.g. `planar task`), zig must print
//! the parent's help to stdout and exit 0 — matching the Cobra
//! convention Go inherits. Today zig's parser returns
//! UnknownSubcommand to stderr and exits 1.
//!
//! Table-driven over the 21 parent verbs (per task 2370's layout
//! resolution: table-driven for clusters A and B). The test collects
//! per-verb failures and reports them all in one shot rather than
//! short-circuiting on the first failure.
//!
//! Today: every verb fails the contract → 21 failures recorded, test
//! reports `expect(false)`. Phase 4 lands a parser change in
//! the CLI parser (then in-tree under src/cli/parser.zig, now in
//! vendor/etc-cli/src/cli/parser.zig) that turns this test green.

const std = @import("std");
const harness = @import("harness");

// 21 parent verbs per parity-triage.md §A-bare-parent-verb member list.
// `annotate` (Cluster D, zig-only) and leaf verbs (Cluster I — handoff,
// health, demote, link, unlink, promote, import, synthesize,
// resume, search, tree) are excluded.
const PARENT_VERBS = [_][]const u8{
    "plan",   "task",      "artifact",  "audit",     "capture",
    "config", "decision",  "doc",       "ext",       "links",
    "local",  "question",  "scenario",  "scope",     "skills",
    "spec",   "templates", "test-spec", "workbench", "workspace",
    "assoc",
};

test "parity: bare parent-verb prints help and exits 0 (Cluster A, Q234; red until parser change)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var failures: std.ArrayList([]const u8) = .empty;
    defer {
        for (failures.items) |f| gpa.free(f);
        failures.deinit(gpa);
    }

    inline for (PARENT_VERBS) |verb| {
        const res = suite.exec(&.{verb});
        defer gpa.free(res.stdout);
        defer gpa.free(res.stderr);

        const exit_ok = res.term == .exited and res.term.exited == 0;
        const stdout_ok = std.mem.containsAtLeast(u8, res.stdout, 1, "USAGE:") or
            std.mem.containsAtLeast(u8, res.stdout, 1, "COMMANDS:");
        const no_unknown = !std.mem.containsAtLeast(u8, res.stderr, 1, "UnknownSubcommand");

        if (!exit_ok or !stdout_ok or !no_unknown) {
            const reason = std.fmt.allocPrint(gpa, "{s}: exit_ok={} stdout_ok={} no_unknown={} (stderr: {s})", .{
                verb, exit_ok, stdout_ok, no_unknown, std.mem.trim(u8, res.stderr, " \t\r\n"),
            }) catch @panic("OOM building failure msg");
            failures.append(gpa, reason) catch @panic("OOM appending failure");
        }
    }

    if (failures.items.len > 0) {
        std.debug.print("\n{d} parent verb(s) failed the Q234 bare-help+exit0 contract:\n", .{failures.items.len});
        for (failures.items) |f| std.debug.print("  - {s}\n", .{f});
        try std.testing.expect(false);
    }
}
