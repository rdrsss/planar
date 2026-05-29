//! integration_tests/parity_help_prose_test.zig
//!
//! Cluster B parity tests for plan 351. See parity-triage.md
//! §B-help-text-content-loss (32 audit rows). Zig dropped Go's
//! per-command prose lead-ins from `<verb> --help` output. The prose
//! documents invariants (status lifecycle, allowed kinds, scope
//! resolution rules) that are part of the user-facing contract.
//!
//! Recommendation per triage doc: restore the prose; format-only
//! diffs (USAGE: vs Usage: header style, global-flag enumeration
//! absence) are subordinate Bucket 4 cosmetic and NOT asserted here.
//!
//! Per-verb expected-prose table: each verb carries 2-3 strong
//! substrings extracted from the Go output captured in
//! scripts/parity-data/parity-gap-report.json (the `help` rows). The
//! substrings are deliberately chosen to be specific enough that
//! generic phrasing matches won't yield false positives while
//! lenient enough to survive minor wording changes.
//!
//! Today: every verb fails because zig's --help is the terse
//! one-line summary format. Phase 4 work to restore the prose makes
//! these tests green.

const std = @import("std");
const harness = @import("harness");

const ProseCase = struct {
    verb: []const u8,
    must_contain: []const []const u8,
};

// 32 verbs with prose lead-ins zig dropped, per gap-report.json `help`
// rows. Substrings derived from the Go-only (-) lines in each diff,
// before the "Usage:" header (the load-bearing prose section).
const CASES = [_]ProseCase{
    .{ .verb = "artifact", .must_contain = &.{ "Kinds:", "tech_spec", "Status lifecycle:" } },
    .{ .verb = "assoc", .must_contain = &.{ "many-to-many", "subcommand names" } },
    .{ .verb = "audit", .must_contain = &.{"Cross-plane audit trail"} },
    .{ .verb = "capture", .must_contain = &.{ "explicit session management", "narrative notes" } },
    .{ .verb = "config", .must_contain = &.{ "configuration file", "~/.planar/config.toml" } },
    .{ .verb = "decision", .must_contain = &.{ "rationale for choices", "Status lifecycle: proposed" } },
    .{ .verb = "demote", .must_contain = &.{ "Reverse a promotion", "global personal scope" } },
    .{ .verb = "ext", .must_contain = &.{ "external systems", "operational plane", "Sub-commands:" } },
    .{ .verb = "handoff", .must_contain = &.{ "context snapshot", "context_snapshots" } },
    .{ .verb = "health", .must_contain = &.{ "schema version currency", "Exit codes:" } },
    .{ .verb = "link", .must_contain = &.{ "external_links row", "ext create" } },
    .{ .verb = "links", .must_contain = &.{ "entity_links", "typed relationships" } },
    .{ .verb = "local", .must_contain = &.{ "personal skills and agents", "~/.planar/local/" } },
    .{ .verb = "import", .must_contain = &.{ "ImportPlan", "tech specs, roadmap milestones" } },
    .{ .verb = "synthesize", .must_contain = &.{ "LLM synthesis pass", "git history" } },
    .{ .verb = "plan", .must_contain = &.{ "Status lifecycle:", "Plans may be hierarchical" } },
    .{ .verb = "promote", .must_contain = &.{ "Promote an entity", "Valid entity kinds" } },
    .{ .verb = "question", .must_contain = &.{ "open uncertainties", "Status lifecycle: open" } },
    .{ .verb = "resume", .must_contain = &.{ "8-section resume packet", "Identity" } },
    .{ .verb = "scenario", .must_contain = &.{ "verification artifacts", "Status lifecycle: draft" } },
    .{ .verb = "scope", .must_contain = &.{ "Plan 153", "active scope stack" } },
    .{ .verb = "search", .must_contain = &.{ "FTS5", "unicode61" } },
    .{ .verb = "skills", .must_contain = &.{ "skill source tree", "skills/src/" } },
    .{ .verb = "spec", .must_contain = &.{ "planning pipeline", "spec ingest" } },
    .{ .verb = "task", .must_contain = &.{ "Status lifecycle: todo", "next_action field" } },
    .{ .verb = "templates", .must_contain = &.{ "template plane", "three-level fallback" } },
    .{ .verb = "test-spec", .must_contain = &.{ "test-spec coverage", "per-milestone breakdown" } },
    .{ .verb = "tree", .must_contain = &.{ "hierarchical view", "parent_plan_id" } },
    .{ .verb = "unlink", .must_contain = &.{ "external_links row", "link id", "cascade" } },
    .{ .verb = "workbench", .must_contain = &.{ "bidirectional sync surface", "workbench root" } },
    .{ .verb = "workspace", .must_contain = &.{ "Workspace administration", "kind=org" } },
};

test "parity: `<verb> --help` retains Go prose lead-ins (Cluster B; red until per-verb prose restored)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var failures: std.ArrayList([]const u8) = .empty;
    defer {
        for (failures.items) |f| gpa.free(f);
        failures.deinit(gpa);
    }

    inline for (CASES) |case| {
        const stdout = suite.mustRun(&.{ case.verb, "--help" });
        defer gpa.free(stdout);

        var missing: std.ArrayList([]const u8) = .empty;
        defer missing.deinit(gpa);

        inline for (case.must_contain) |needle| {
            if (!std.mem.containsAtLeast(u8, stdout, 1, needle)) {
                missing.append(gpa, needle) catch @panic("OOM");
            }
        }

        if (missing.items.len > 0) {
            // Build a compact failure description: verb + missing needles.
            var buf: std.ArrayList(u8) = .empty;
            defer buf.deinit(gpa);
            const prefix = std.fmt.allocPrint(gpa, "{s}: missing [", .{case.verb}) catch @panic("OOM");
            defer gpa.free(prefix);
            buf.appendSlice(gpa, prefix) catch @panic("OOM");
            for (missing.items, 0..) |n, i| {
                if (i > 0) buf.appendSlice(gpa, ", ") catch @panic("OOM");
                const quoted = std.fmt.allocPrint(gpa, "\"{s}\"", .{n}) catch @panic("OOM");
                defer gpa.free(quoted);
                buf.appendSlice(gpa, quoted) catch @panic("OOM");
            }
            buf.appendSlice(gpa, "]") catch @panic("OOM");

            const owned = buf.toOwnedSlice(gpa) catch @panic("OOM");
            failures.append(gpa, owned) catch @panic("OOM");
        }
    }

    if (failures.items.len > 0) {
        std.debug.print("\n{d} verb(s) failed the Cluster B prose-lead-in contract:\n", .{failures.items.len});
        for (failures.items) |f| std.debug.print("  - {s}\n", .{f});
        try std.testing.expect(false);
    }
}
