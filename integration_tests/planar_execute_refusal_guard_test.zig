//! integration_tests/planar_execute_refusal_guard_test.zig —
//!   bright-line refusal guard (plan 492 M10 task 3206).
//!
//! Black-box integration: seeds a fixture DB with a plan containing tasks
//! that touch (or don't touch) risky surfaces (migrations/*.sql, new top-level
//! verbs under src/cmd/<bin>/handlers/, validate/invariant/methodology code).
//! Drives planar-execute against the fixture and asserts the guard:
//!
//!   - REFUSES (exit 2) when meta.reviewer is omitted/false AND the plan has
//!     a risky-touch task; stderr names the task + predicate.
//!   - PASSES when meta.reviewer = true (author declares reviewer cadence).
//!   - PASSES when the plan has only clean-touch tasks.
//!   - SKIPS under --dry-run (no execution, no refusal).
//!   - PASSES under --bypass-reviewer-guard with a loud stderr warning.
//!   - Is a NO-OP when --plan is absent (no plan ⇒ nothing to inspect).
//!
//! ## Hermeticity
//!
//! Same shape as planar_execute_eligible_test:
//!   - Fixture DB seeded via the real `planar` CLI (harness Suite injects
//!     PLANAR_DB = temp fixture DB).
//!   - planar-execute run with PLANAR_DB pointed at the fixture and PATH
//!     prepended with the freshly-built bin dir so its bare-name `planar`
//!     subprocess calls resolve to ./bin/planar.
//!   - PLANAR_EXECUTE_LIVE_AGENT is intentionally NOT set; the guard fires
//!     for both --mock-worker and the live gate, and --mock-worker is the
//!     simpler way to exercise the executing path under CI.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Binary resolution helpers (mirrors eligible-test pattern)
// ---------------------------------------------------------------------------

fn envValue(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key) and s.len > key.len and s[key.len] == '=') {
            return s[key.len + 1 ..];
        }
    }
    return null;
}

fn resolveExecuteBin() []const u8 {
    return envValue("PLANAR_EXECUTE_BIN") orelse
        @panic("PLANAR_EXECUTE_BIN is not set. Run via: zig build test-integration");
}

fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: zig build test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

// ---------------------------------------------------------------------------
// RunResult + runner
// ---------------------------------------------------------------------------

const RunResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,

    fn deinit(self: RunResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }

    fn exitCode(self: RunResult) u32 {
        return switch (self.term) {
            .exited => |code| code,
            else => 255,
        };
    }
};

/// runExecuteWithFixture spawns `planar-execute <args...>` with PLANAR_DB
/// pointed at the suite's fixture DB and PATH prepended with the freshly-
/// built bin dir. PLANAR_EXECUTE_LIVE_AGENT is stripped.
fn runExecuteWithFixture(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", suite.absDbPath());

    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    _ = env_map.swapRemove("PLANAR_EXECUTE_LIVE_AGENT");

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .environ_map = &env_map,
    });
    return .{
        .term = result.term,
        .stdout = result.stdout,
        .stderr = result.stderr,
        .gpa = gpa,
    };
}

// ---------------------------------------------------------------------------
// Workflow + fixture seeding helpers
// ---------------------------------------------------------------------------

fn writeWorkflow(tmp: *std.testing.TmpDir, name: []const u8, content: []const u8) !void {
    var f = try tmp.dir.createFile(std.testing.io, name, .{});
    defer f.close(std.testing.io);
    try f.writeStreamingAll(std.testing.io, content);
}

fn tmpAbsPath(tmp: *std.testing.TmpDir, gpa: std.mem.Allocator) ![]u8 {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const len = try tmp.dir.realPath(std.testing.io, &buf);
    return gpa.dupe(u8, buf[0..len]);
}

fn workflowPath(tmp_abs: []const u8, name: []const u8, gpa: std.mem.Allocator) ![]u8 {
    return std.fs.path.join(gpa, &.{ tmp_abs, name });
}

const PlanJSON = struct { id: i64 };
const TaskJSON = struct { id: i64 };

fn addTask(suite: *harness.Suite, arena: std.mem.Allocator, pid: []const u8, title: []const u8) i64 {
    const t = suite.mustRunJSON(TaskJSON, arena, &.{ "task", "add", "--plan", pid, "--json", title });
    return t.id;
}

fn touchPath(suite: *harness.Suite, repo_slug: []const u8, task_id: i64, path: []const u8) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    const out = suite.mustRun(&.{ "task", "touches", "add", tid, repo_slug, "--path", path });
    gpa.free(out);
}

fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("guard-repo");
    const assoc_slug = "guard-org";
    const cr = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" });
    suite.allocator.free(cr);
    const root = suite.tmpAbsPath();
    const ad = suite.mustRun(&.{ "assoc", "add", assoc_slug, root });
    suite.allocator.free(ad);

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", assoc_slug, "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "guard-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found in assoc members", .{});
}

// ---------------------------------------------------------------------------
// Workflow sources
// ---------------------------------------------------------------------------

/// A no-reviewer workflow: meta.reviewer is omitted (defaults to false).
const wf_no_reviewer =
    \\return {
    \\  meta = { name = "no-reviewer-wf", description = "omits meta.reviewer", phases = {} },
    \\  run = function(ctx)
    \\    -- pure Lua; the guard fires BEFORE run() is entered for a risky plan.
    \\    print("ran")
    \\  end,
    \\}
;

/// A reviewer-declared workflow: meta.reviewer = true.
const wf_with_reviewer =
    \\return {
    \\  meta = { name = "with-reviewer-wf", description = "declares reviewer", reviewer = true, phases = {} },
    \\  run = function(ctx)
    \\    print("ran")
    \\  end,
    \\}
;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "refusal guard: REFUSES when meta.reviewer omitted and plan has a migration touch (task 3206)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "guard-migration-plan", "--json", "GUARD_MIGRATION",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_mig = addTask(&suite, arena, pid, "add-migration");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.up.sql");

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "no_reviewer.lua", wf_no_reviewer);
    const wf_path = try workflowPath(tmp_abs, "no_reviewer.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--mock-worker", "--plan", pid, wf_path });
    defer res.deinit();

    // exit code 2 = bright-line refusal (distinct from generic "load error" 1).
    try std.testing.expectEqual(@as(u32, 2), res.exitCode());
    // Stderr names the refusal, the task, and the predicate.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "REFUSING TO RUN") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "bright-line refusal guard") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "migrations/00099_widget.up.sql") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "migrations/*.sql") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "meta.reviewer") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "--bypass-reviewer-guard") != null);
    // run() must NOT have run.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "ran") == null);
}

test "refusal guard: REFUSES on a new top-level verb touch (task 3206)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "guard-verb-plan", "--json", "GUARD_VERB",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_verb = addTask(&suite, arena, pid, "add-verb");
    touchPath(&suite, repo, t_verb, "src/cmd/planar/handlers/widget/cmd.zig");

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "no_reviewer.lua", wf_no_reviewer);
    const wf_path = try workflowPath(tmp_abs, "no_reviewer.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--mock-worker", "--plan", pid, wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 2), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "new top-level CLI verb") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "src/cmd/planar/handlers/widget/cmd.zig") != null);
}

test "refusal guard: REFUSES on a methodology singleton touch (task 3206)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "guard-invariant-plan", "--json", "GUARD_INV",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_inv = addTask(&suite, arena, pid, "edit-methodology");
    touchPath(&suite, repo, t_inv, "agents/methodology.md");

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "no_reviewer.lua", wf_no_reviewer);
    const wf_path = try workflowPath(tmp_abs, "no_reviewer.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--mock-worker", "--plan", pid, wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 2), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "validate / invariant / methodology singleton") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "agents/methodology.md") != null);
}

test "refusal guard: PASSES when meta.reviewer = true (author declared) (task 3206)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "guard-reviewer-pass", "--json", "GUARD_REVIEWER",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Same risky touch — the contract is that meta.reviewer = true bypasses
    // the inspection unconditionally (author owns the doctrine assertion).
    const t_mig = addTask(&suite, arena, pid, "add-migration");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.up.sql");

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "with_reviewer.lua", wf_with_reviewer);
    const wf_path = try workflowPath(tmp_abs, "with_reviewer.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--mock-worker", "--plan", pid, wf_path });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nwith_reviewer stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // run() must have entered.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "ran") != null);
    // No refusal message.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "REFUSING TO RUN") == null);
}

test "refusal guard: PASSES when plan has only clean-touch tasks (task 3206)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "guard-clean-plan", "--json", "GUARD_CLEAN",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_a = addTask(&suite, arena, pid, "alpha");
    touchPath(&suite, repo, t_a, "src/engine/planning/strategy.zig");
    const t_b = addTask(&suite, arena, pid, "beta");
    touchPath(&suite, repo, t_b, "src/db/db.zig");

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "no_reviewer.lua", wf_no_reviewer);
    const wf_path = try workflowPath(tmp_abs, "no_reviewer.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--mock-worker", "--plan", pid, wf_path });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nclean stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "ran") != null);
}

test "refusal guard: SKIPPED under --dry-run even with risky plan (task 3206)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "guard-dryrun-plan", "--json", "GUARD_DRYRUN",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_mig = addTask(&suite, arena, pid, "add-migration");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.up.sql");

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "no_reviewer.lua", wf_no_reviewer);
    const wf_path = try workflowPath(tmp_abs, "no_reviewer.lua", gpa);
    defer gpa.free(wf_path);

    // --dry-run validates the workflow and exits 0 without entering run(); the
    // guard MUST NOT fire (no execution to guard).
    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--dry-run", "--plan", pid, wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "REFUSING TO RUN") == null);
    // run() did not enter.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "ran") == null);
}

test "refusal guard: --bypass-reviewer-guard overrides + prints loud warning (task 3206)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "guard-bypass-plan", "--json", "GUARD_BYPASS",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_mig = addTask(&suite, arena, pid, "add-migration");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.up.sql");

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "no_reviewer.lua", wf_no_reviewer);
    const wf_path = try workflowPath(tmp_abs, "no_reviewer.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{
        "run", "--mock-worker", "--plan", pid, "--bypass-reviewer-guard", wf_path,
    });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nbypass stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // Loud warning on stderr.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "WARNING") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "bright-line refusal guard BYPASSED") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "--bypass-reviewer-guard") != null);
    // run() entered.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "ran") != null);
}

test "refusal guard: NO-OP when --plan is absent (task 3206)" {
    // No --plan means no tasks to inspect; the guard cannot fire regardless
    // of meta.reviewer or surface touches. Workflow must run cleanly.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "no_reviewer.lua", wf_no_reviewer);
    const wf_path = try workflowPath(tmp_abs, "no_reviewer.lua", gpa);
    defer gpa.free(wf_path);

    // No --plan, no --mock-worker, no gate — pure-Lua default-stub run.
    const res = try runExecuteWithFixture(gpa, &suite, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "ran") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "REFUSING TO RUN") == null);
}

test "refusal guard: --bypass-reviewer-guard flag advertised in `run --help` (task 3206)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--help" });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "--bypass-reviewer-guard") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "bright-line refusal guard") != null);
}
