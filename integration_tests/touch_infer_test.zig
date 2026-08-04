//! integration_tests/touch_infer_test.zig — black-box tests for
//! `planar task touches infer` (plan 988, task 5842).
//!
//! The headline test is the operator arc the verb exists to serve, end to
//! end through the compiled binary:
//!
//!   1. Two tasks cite disjoint real files in their bodies, but declare no
//!      touches.
//!   2. `plan recommend-strategy` excludes BOTH under rule 2 — an empty
//!      touch set reads as "touches everything". No fan-out is available.
//!   3. `task touches infer --apply` on each.
//!   4. `plan recommend-strategy` now reports both as parallel-eligible and
//!      fan-out as available.
//!
//! That arc is the whole point: eligibility was blocked by an absent
//! declaration, not by a genuine conflict. A "verb exists and emits JSON"
//! smoke test would not have caught a regression in it (CLAUDE.md §Contribution
//! policy).
//!
//! The remaining tests pin the safety properties of decision 906 — preview
//! writes nothing, ambiguity resolves wide, unplaceable tokens are reported
//! rather than dropped, and re-applying is a no-op.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };

const Candidate = struct {
    token: []const u8 = "",
    evidence: []const u8 = "",
    classification: []const u8 = "",
    paths: []const []const u8 = &.{},
};

const InferJSON = struct {
    task_id: i64 = 0,
    repo_id: i64 = 0,
    repo_slug: []const u8 = "",
    applied: bool = false,
    written: usize = 0,
    review: usize = 0,
    candidates: []const Candidate = &.{},
};

const TouchRow = struct {
    repo: []const u8 = "",
    path: []const u8 = "",
};

const TouchListJSON = struct {
    task_id: i64 = 0,
    paths: []const TouchRow = &.{},
};

// =========================================================================
// Fixture helpers
// =========================================================================

fn writeFixture(suite: *harness.Suite, rel: []const u8, data: []const u8) void {
    const gpa = suite.allocator;
    const root = suite.tmpAbsPath();
    const abs = std.fs.path.join(gpa, &.{ root, rel }) catch @panic("OOM");
    defer gpa.free(abs);
    if (std.fs.path.dirname(abs)) |dir| {
        std.Io.Dir.cwd().createDirPath(std.testing.io, dir) catch |e|
            std.debug.panic("createDirPath {s}: {s}", .{ dir, @errorName(e) });
    }
    std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = abs, .data = data }) catch |e|
        std.debug.panic("writeFile {s}: {s}", .{ abs, @errorName(e) });
}

fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("ti-repo");
    const cr = suite.mustRun(&.{ "assoc", "create", "ti-org", "--kind", "org" });
    suite.allocator.free(cr);
    const root = suite.tmpAbsPath();
    const ad = suite.mustRun(&.{ "assoc", "add", "ti-org", root });
    suite.allocator.free(ad);

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", "ti-org", "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "ti-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found", .{});
}

fn addTask(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    plan_id: i64,
    title: []const u8,
    body: []const u8,
) i64 {
    const gpa = suite.allocator;
    const pid = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch unreachable;
    defer gpa.free(pid);
    const t = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task",   "add",  title,
        "--plan", pid,    "--body",
        body,     "--json", "--editor=false",
    });
    return t.id;
}

fn idStr(gpa: std.mem.Allocator, id: i64) []const u8 {
    return std.fmt.allocPrint(gpa, "{d}", .{id}) catch unreachable;
}

/// Run `recommend-strategy` and return its raw text output.
fn recommend(suite: *harness.Suite, plan_id: i64) []u8 {
    const gpa = suite.allocator;
    const pid = idStr(gpa, plan_id);
    defer gpa.free(pid);
    return suite.mustRun(&.{ "plan", "recommend-strategy", pid });
}

// =========================================================================
// The headline arc
// =========================================================================

test "infer --apply flips rule-2-excluded tasks to parallel-eligible" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const root = suite.tmpAbsPath();

    // Two genuinely disjoint files.
    writeFixture(&suite, "src/alpha.zig", "pub fn a() void {}\n");
    writeFixture(&suite, "src/beta.zig", "pub fn b() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Touch inference arc", "--status", "active", "--json",
    });

    // Each task NAMES its file in the body but declares no touches.
    const t1 = addTask(&suite, arena, plan.id, "Edit alpha", "Rework src/alpha.zig only.");
    const t2 = addTask(&suite, arena, plan.id, "Edit beta", "Rework src/beta.zig only.");

    // --- before: both excluded by rule 2 ---------------------------------
    {
        const out = recommend(&suite, plan.id);
        defer gpa.free(out);
        try std.testing.expect(std.mem.indexOf(u8, out, "eligible:0") != null);
        try std.testing.expect(std.mem.indexOf(u8, out, "fan_out_available:no") != null);
        try std.testing.expect(std.mem.indexOf(u8, out, "no task_touches declared") != null);
    }

    // --- infer + apply ----------------------------------------------------
    for ([_]i64{ t1, t2 }) |tid| {
        const s = idStr(gpa, tid);
        defer gpa.free(s);
        const res = suite.mustRunJSON(InferJSON, arena, &.{
            "task", "touches", "infer", s, "--repo", repo, "--apply", "--json",
        });
        try std.testing.expect(res.applied);
        try std.testing.expectEqual(@as(usize, 1), res.written);
        try std.testing.expectEqualStrings(repo, res.repo_slug);
    }
    _ = root;

    // --- after: both eligible, fan-out available --------------------------
    {
        const out = recommend(&suite, plan.id);
        defer gpa.free(out);
        try std.testing.expect(std.mem.indexOf(u8, out, "eligible:2") != null);
        try std.testing.expect(std.mem.indexOf(u8, out, "fan_out_available:yes") != null);
        // The rule-2 exclusion reason is gone.
        try std.testing.expect(std.mem.indexOf(u8, out, "no task_touches declared") == null);
    }
}

// =========================================================================
// Decision 906 safety properties
// =========================================================================

test "preview writes nothing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "src/alpha.zig", "pub fn a() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Preview only", "--status", "active", "--json",
    });
    const t1 = addTask(&suite, arena, plan.id, "Edit alpha", "Rework src/alpha.zig.");
    const s = idStr(gpa, t1);
    defer gpa.free(s);

    // Preview reports a proposal...
    const prev = suite.mustRunJSON(InferJSON, arena, &.{
        "task", "touches", "infer", s, "--repo", repo, "--json",
    });
    try std.testing.expect(!prev.applied);
    try std.testing.expectEqual(@as(usize, 0), prev.written);
    try std.testing.expectEqual(@as(usize, 1), prev.candidates.len);

    // ...and the task still has no declared path touches.
    const listed = suite.mustRunJSON(TouchListJSON, arena, &.{
        "task", "touches", "list", s, "--json",
    });
    try std.testing.expectEqual(@as(usize, 0), listed.paths.len);

    // Eligibility is therefore unchanged — omission still serializes.
    const out = recommend(&suite, plan.id);
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "no task_touches declared") != null);
}

test "a directory citation expands to its files rather than being dropped" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "docs/one.md", "x\n");
    writeFixture(&suite, "docs/two.md", "x\n");
    writeFixture(&suite, "docs/nested/three.md", "x\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Directory expansion", "--status", "active", "--json",
    });
    const t1 = addTask(&suite, arena, plan.id, "Refresh docs", "Update everything under docs/ this cycle.");
    const s = idStr(gpa, t1);
    defer gpa.free(s);

    const res = suite.mustRunJSON(InferJSON, arena, &.{
        "task", "touches", "infer", s, "--repo", repo, "--apply", "--json",
    });

    // Wider, not narrower: the nested file counts too (decision 906).
    try std.testing.expectEqual(@as(usize, 3), res.written);
    try std.testing.expectEqual(@as(usize, 1), res.candidates.len);
    try std.testing.expectEqualStrings("directory", res.candidates[0].classification);
}

test "an unplaceable path is reported for review and never written" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "src/real.zig", "pub fn r() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Unresolved reporting", "--status", "active", "--json",
    });
    const t1 = addTask(
        &suite,
        arena,
        plan.id,
        "Mixed",
        "Edit src/real.zig and also src/does_not_exist.zig here.",
    );
    const s = idStr(gpa, t1);
    defer gpa.free(s);

    const res = suite.mustRunJSON(InferJSON, arena, &.{
        "task", "touches", "infer", s, "--repo", repo, "--apply", "--json",
    });

    // The real file is written; the phantom one is surfaced, not invented.
    try std.testing.expectEqual(@as(usize, 1), res.written);
    try std.testing.expectEqual(@as(usize, 1), res.review);

    var saw_unresolved = false;
    for (res.candidates) |c| {
        if (std.mem.eql(u8, c.classification, "unresolved")) {
            saw_unresolved = true;
            try std.testing.expectEqual(@as(usize, 0), c.paths.len);
        }
    }
    try std.testing.expect(saw_unresolved);
}

test "re-applying infer is a no-op" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "src/alpha.zig", "pub fn a() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Idempotence", "--status", "active", "--json",
    });
    const t1 = addTask(&suite, arena, plan.id, "Edit alpha", "Rework src/alpha.zig.");
    const s = idStr(gpa, t1);
    defer gpa.free(s);

    const first = suite.mustRunJSON(InferJSON, arena, &.{
        "task", "touches", "infer", s, "--repo", repo, "--apply", "--json",
    });
    const second = suite.mustRunJSON(InferJSON, arena, &.{
        "task", "touches", "infer", s, "--repo", repo, "--apply", "--json",
    });
    try std.testing.expectEqual(first.written, second.written);

    // The unique(task_id, repo_id, path) constraint collapses the re-run:
    // one declared path, not two.
    const listed = suite.mustRunJSON(TouchListJSON, arena, &.{
        "task", "touches", "list", s, "--json",
    });
    try std.testing.expectEqual(@as(usize, 1), listed.paths.len);
}

test "a task with no inferable paths writes nothing and stays serialized" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "src/alpha.zig", "pub fn a() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Omission stays safe", "--status", "active", "--json",
    });
    // Prose with no path in it at all.
    const t1 = addTask(
        &suite,
        arena,
        plan.id,
        "Think about the design",
        "Decide whether the approach is right before writing anything.",
    );
    const s = idStr(gpa, t1);
    defer gpa.free(s);

    // Even WITH --apply, inference invents nothing.
    const res = suite.mustRunJSON(InferJSON, arena, &.{
        "task", "touches", "infer", s, "--repo", repo, "--apply", "--json",
    });
    try std.testing.expectEqual(@as(usize, 0), res.written);
    try std.testing.expectEqual(@as(usize, 0), res.candidates.len);

    const listed = suite.mustRunJSON(TouchListJSON, arena, &.{
        "task", "touches", "list", s, "--json",
    });
    try std.testing.expectEqual(@as(usize, 0), listed.paths.len);

    // The invariant that matters: an undeclared task still serializes.
    // Inference must never turn "I found nothing" into "it touches nothing",
    // which would read as trivially disjoint and falsely parallel-eligible.
    const out = recommend(&suite, plan.id);
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "no task_touches declared") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "eligible:0") != null);
}

test "touches remove --path withdraws one declaration and restores serialization" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "src/alpha.zig", "pub fn a() void {}\n");
    writeFixture(&suite, "src/beta.zig", "pub fn b() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Withdrawal", "--status", "active", "--json",
    });
    const t1 = addTask(&suite, arena, plan.id, "Edit alpha", "Rework src/alpha.zig only.");
    const t2 = addTask(&suite, arena, plan.id, "Edit beta", "Rework src/beta.zig only.");

    for ([_]i64{ t1, t2 }) |tid| {
        const s = idStr(gpa, tid);
        defer gpa.free(s);
        const out = suite.mustRun(&.{
            "task", "touches", "infer", s, "--repo", repo, "--apply",
        });
        gpa.free(out);
    }

    const s1 = idStr(gpa, t1);
    defer gpa.free(s1);

    // Both eligible to begin with.
    {
        const out = recommend(&suite, plan.id);
        defer gpa.free(out);
        try std.testing.expect(std.mem.indexOf(u8, out, "eligible:2") != null);
    }

    // Withdraw t1's only path declaration.
    const rm = suite.mustRunJSON(struct {
        ok: bool = false,
        path: []const u8 = "",
    }, arena, &.{
        "task", "touches", "remove", s1, repo, "--path", "src/alpha.zig", "--json",
    });
    try std.testing.expect(rm.ok);
    try std.testing.expectEqualStrings("src/alpha.zig", rm.path);

    const listed = suite.mustRunJSON(TouchListJSON, arena, &.{
        "task", "touches", "list", s1, "--json",
    });
    try std.testing.expectEqual(@as(usize, 0), listed.paths.len);

    // Withdrawal takes effect — but note WHERE it lands. `--path` does not
    // cascade to the repo edge, and `infer --apply` wrote one. So t1 is not
    // back to "undeclared"; it now holds a WHOLE-REPO claim, which collides
    // with any same-repo touch. t2's path touch is on that same repo, so
    // both drop (drop-both-on-tie) and nothing is eligible.
    //
    // That is stricter than the undeclared state, not looser, which is the
    // right direction for a withdrawal to err in (decision 906). Pinned
    // because the intuitive expectation — "removing a path returns the task
    // to undeclared" — is wrong, and a future cascade change would silently
    // relax this.
    {
        const out = recommend(&suite, plan.id);
        defer gpa.free(out);
        try std.testing.expect(std.mem.indexOf(u8, out, "eligible:0") != null);
        try std.testing.expect(std.mem.indexOf(u8, out, "fan_out_available:no") != null);
    }
}

test "removing the repo edge does NOT silently drop path declarations" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "src/alpha.zig", "pub fn a() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Edge vs path", "--status", "active", "--json",
    });
    const t1 = addTask(&suite, arena, plan.id, "Edit alpha", "Rework src/alpha.zig.");
    const s = idStr(gpa, t1);
    defer gpa.free(s);

    const applied = suite.mustRun(&.{
        "task", "touches", "infer", s, "--repo", repo, "--apply",
    });
    gpa.free(applied);

    // Drop the coarse repo edge only.
    const out = suite.mustRun(&.{ "task", "touches", "remove", s, repo });
    gpa.free(out);

    // The path row survives — the two granularities are independent, which
    // is exactly why --path had to exist. Pinning it so nobody "fixes" the
    // repo-edge removal into a cascade without deciding to.
    const listed = suite.mustRunJSON(TouchListJSON, arena, &.{
        "task", "touches", "list", s, "--json",
    });
    try std.testing.expectEqual(@as(usize, 1), listed.paths.len);
}

test "withdrawing a path that was never declared is an error, not a no-op" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "src/alpha.zig", "pub fn a() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Typo guard", "--status", "active", "--json",
    });
    const t1 = addTask(&suite, arena, plan.id, "Edit alpha", "Rework src/alpha.zig.");
    const s = idStr(gpa, t1);
    defer gpa.free(s);

    const applied = suite.mustRun(&.{
        "task", "touches", "infer", s, "--repo", repo, "--apply",
    });
    gpa.free(applied);

    // A mistyped path must fail loudly. Silently succeeding would let an
    // operator believe a declaration was withdrawn while it still drives
    // eligibility.
    const stderr = suite.expectFailure(&.{
        "task", "touches", "remove", s, repo, "--path", "src/alhpa.zig",
    });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "no declared path touch") != null);

    // The real declaration is untouched.
    const listed = suite.mustRunJSON(TouchListJSON, arena, &.{
        "task", "touches", "list", s, "--json",
    });
    try std.testing.expectEqual(@as(usize, 1), listed.paths.len);
}

test "a path cited with a line number still resolves" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    writeFixture(&suite, "src/alpha.zig", "pub fn a() void {}\n");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "Line citations", "--status", "active", "--json",
    });
    // `path:line` is how this codebase cites locations; if inference misses
    // it, the most common declaration style silently under-declares.
    const t1 = addTask(&suite, arena, plan.id, "Fix", "The bug is at src/alpha.zig:12-40.");
    const s = idStr(gpa, t1);
    defer gpa.free(s);

    const res = suite.mustRunJSON(InferJSON, arena, &.{
        "task", "touches", "infer", s, "--repo", repo, "--apply", "--json",
    });
    try std.testing.expectEqual(@as(usize, 1), res.written);
    try std.testing.expectEqual(@as(usize, 0), res.review);
}
