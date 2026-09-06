//! integration_tests/parity_bare_parent_help_test.zig
//!
//! Cluster A parity tests for plan 351. See parity-triage.md
//! §A-bare-parent-verb (84 audit rows across 21 parent verbs × 4 bare
//! invocations) and question 234 for the operator decision.
//!
//! Operator decision 2026-05-26 (Q234): match Go. When a parent verb
//! is invoked with no subcommand (e.g. `planar task`), the binary must
//! print the parent's help to stdout and exit 0 — matching the Cobra
//! convention Go inherits.
//!
//! Q234 IS SATISFIED as of the C++/CLI11 binary (task 6480, measured
//! 2026-09-06 against the C++ binary at f63b6a70). CLI11 gives bare-
//! parent-verb help-and-exit-0 for free where the hand-rolled zig parser
//! (etcli-zig) did not — the file previously carried a "red until parser
//! change" header describing that hand-rolled-parser era, which is now
//! obsolete and was actively misleading (the contract has been met for
//! some time; only this file's own verb inventory had gone stale).
//!
//! Table-driven over the 18 parent verbs that still resolve to a genuine
//! `planar` parent verb with subcommands (per task 2370's layout
//! resolution: table-driven for clusters A and B). The test collects
//! per-verb failures and reports them all in one shot rather than
//! short-circuiting on the first failure.
//!
//! Two entries were removed from the original 20-verb list, both stale
//! by sanctioned contract changes rather than by a Q234 regression:
//!
//!   - `ext` — the entire `ext` surface moved to the standalone
//!     `planar-ext` binary under decisions 995-1001 (plan 996, task
//!     6419). `planar ext` is no longer a parent verb at all; it now
//!     exits 2 ("The following argument was not expected") because CLI11
//!     rejects the positional outright. Asserting bare-help-and-exit-0
//!     for a surface that was deliberately removed would be asserting
//!     the wrong contract. Whether an equivalent bare-parent-help
//!     assertion belongs against `planar-ext ext`'s own subcommands is
//!     an open question for the `planar-ext` test suite, not this file
//!     (task 6480 reports rather than assumes this).
//!   - `skills` — plan 918 M5 retired the in-tree skill/vendor-surface
//!     renderer verb and moved rendering to the external `scriptorium`
//!     binary, reducing `skills` to a leaf command with no subcommands.
//!     `planar skills` exits 0 and prints "This command has no
//!     subcommands.", which correctly has no USAGE:/COMMANDS: section to
//!     assert on — a leaf verb's bare invocation is not a "parent verb
//!     with no subcommand given" case, so it does not belong in this
//!     table. The Zig oracle produces the same explanatory text and
//!     fails this assertion identically; this is not a C++-only
//!     divergence.
//!
//! Measured 2026-09-06: all 18 of the verbs below satisfy the contract
//! (exit 0, stdout carries USAGE: or COMMANDS:, stderr has no
//! UnknownSubcommand). No other entry in the original 20-verb list has
//! likewise become a leaf or been removed — every other verb still
//! resolves to a genuine parent with subcommands.

const std = @import("std");
const harness = @import("harness");

// 18 parent verbs per parity-triage.md §A-bare-parent-verb member list,
// minus `ext` (moved to planar-ext, decisions 995-1001) and `skills`
// (reduced to a leaf, plan 918). `annotate` (Cluster D, zig-only) and
// leaf verbs (Cluster I — handoff, health, demote, link, unlink, promote,
// import, synthesize, resume, search, tree) remain excluded as before.
const PARENT_VERBS = [_][]const u8{
    "plan",      "task",      "artifact",  "audit",     "capture",
    "config",    "decision",  "links",     "local",     "question",
    "scenario",  "scope",     "spec",      "templates", "test-spec",
    "workbench", "workspace", "assoc",
};

test "parity: bare parent-verb prints help and exits 0 (Cluster A, Q234; satisfied)" {
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
