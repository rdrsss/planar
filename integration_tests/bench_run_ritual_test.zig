//! integration_tests/bench_run_ritual_test.zig — the synthetic end-to-end
//! proof for the clean-slate run ritual (plan 635, M1.4).
//!
//! M1.4 acceptance: "a dummy slice flows end-to-end into clean joined
//! records." This test proves the FULL measurement loop with NO agents and
//! NO LLM:
//!
//!   1. Stand up a synthetic git worktree fixture (real `git init` + a base
//!      commit; capture base_sha).
//!   2. Seed a plan + task with declared touches through the CLI against an
//!      isolated DB.
//!   3. Run the run ritual `setup` phase via `planar-execute run
//!      workflows/bench_run_ritual.lua --phase setup`. This resets the
//!      worktree to base_sha, opens the run, and snapshots declared touches.
//!   4. Simulate the "coder" slice: edit the declared file in the worktree
//!      (the dummy slice — no LLM).
//!   5. Run the `measure` phase: harvest the actual git diff → kind=actual,
//!      and finish the run.
//!   6. Assert the joined records via `bench show <run_uid> --json`:
//!      declared touches present (from setup), actual touches present (from
//!      harvest), status=completed, every touch joined to the run, no
//!      orphaned/unjoinable rows.
//!
//! The engine reaches Planar state ONLY by shelling allowlisted CLI verbs.
//! To make the inner `planar` shell hit the SAME isolated DB the harness
//! uses, the test constructs a child env with PLANAR_DB set and prepends the
//! dir holding the harness `planar` binary to PATH (the engine shells the
//! bare name `planar`).

const std = @import("std");
const harness = @import("harness");

// -------------------------------------------------------------------------
// env / process plumbing (mirrors planar_execute_test.zig)
// -------------------------------------------------------------------------

fn resolveEnv(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key ++ "=")) return s[(key ++ "=").len..];
    }
    @panic(key ++ " is not set. Run via: zig build test-integration");
}

const RunResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,
    fn deinit(self: RunResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }
};

/// runExecute spawns `planar-execute <args...>` with PLANAR_DB pointed at
/// `db_path` and PATH prefixed with the dir holding the harness `planar`
/// binary so the engine's inner `cli.planar` shell resolves to it.
fn runExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveEnv("PLANAR_EXECUTE_BIN"));
    for (args) |a| try argv.append(gpa, a);

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", db_path);
    try env_map.put("PLANAR_CONFIG_PATH", "/nonexistent-planar-config.toml");
    try env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1");

    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .cwd = .{ .path = cwd },
        .environ_map = &env_map,
    });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr, .gpa = gpa };
}

/// repoRootFromBin derives the repo root from PLANAR_BIN (<repo>/zig-out/bin/planar).
fn repoRootFromBin(allocator: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return allocator.dupe(u8, d3);
}

// -------------------------------------------------------------------------
// git fixture (the synthetic worktree — the "dummy slice" lives here)
// -------------------------------------------------------------------------

fn git(gpa: std.mem.Allocator, dir: []const u8, args: []const []const u8) void {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    argv.append(gpa, "git") catch @panic("OOM");
    argv.append(gpa, "-C") catch @panic("OOM");
    argv.append(gpa, dir) catch @panic("OOM");
    for (args) |a| argv.append(gpa, a) catch @panic("OOM");
    const res = std.process.run(gpa, std.testing.io, .{ .argv = argv.items }) catch
        @panic("git spawn failed");
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("git failed (exit {any})\nstderr: {s}\n", .{ res.term, res.stderr });
        @panic("git command failed");
    }
}

/// initGitFixture stands up a real git repo under `dir` with one committed
/// file and returns the base commit sha (owned by `arena`). This is the
/// clean-slate point the run ritual resets to.
fn initGitFixture(gpa: std.mem.Allocator, arena: std.mem.Allocator, dir: []const u8, filename: []const u8) []const u8 {
    git(gpa, dir, &.{"init"});
    git(gpa, dir, &.{ "checkout", "-b", "main" });
    git(gpa, dir, &.{ "config", "user.email", "planar@example.com" });
    git(gpa, dir, &.{ "config", "user.name", "Planar Test" });

    const filepath = std.fs.path.join(gpa, &.{ dir, filename }) catch @panic("OOM");
    defer gpa.free(filepath);
    std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = filepath, .data = "base contents\n" }) catch
        @panic("writeFile base failed");
    git(gpa, dir, &.{ "add", "." });
    git(gpa, dir, &.{ "commit", "-m", "base commit" });

    const sha_res = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "git", "-C", dir, "rev-parse", "HEAD" },
    }) catch @panic("rev-parse spawn failed");
    defer gpa.free(sha_res.stderr);
    const raw_sha = std.mem.trim(u8, sha_res.stdout, " \t\r\n");
    const sha = arena.dupe(u8, raw_sha) catch @panic("OOM sha");
    gpa.free(sha_res.stdout);
    return sha;
}

// -------------------------------------------------------------------------
// bench show --json shapes (mirrors bench_lifecycle_test.zig)
// -------------------------------------------------------------------------

const ShowTouch = struct {
    id: i64 = 0,
    task_id: i64 = 0,
    path: []const u8 = "",
    kind: []const u8 = "",
    created_at: []const u8 = "",
};

const ShowRun = struct {
    id: i64 = 0,
    run_uid: []const u8 = "",
    plan_id: i64 = 0,
    arm: []const u8 = "",
    base_sha: []const u8 = "",
    config_hash: []const u8 = "",
    status: []const u8 = "",
    touches: []const ShowTouch = &.{},
};

const PlanId = struct { id: i64 };
const TaskId = struct { id: i64 };

/// Register a project at the suite tmp dir and return its repo slug (read
/// back via `assoc members --json` — the slug is auto-generated, not the
/// project name, and `task touches add` resolves the repo by that slug).
fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("ritual-repo");
    const assoc_slug = "ritual-org";
    const cr = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" });
    suite.allocator.free(cr);
    const root = suite.tmpAbsPath();
    const ad = suite.mustRun(&.{ "assoc", "add", assoc_slug, root });
    suite.allocator.free(ad);

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", assoc_slug, "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "ritual-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found in assoc members", .{});
}

// -------------------------------------------------------------------------
// The synthetic end-to-end test.
// -------------------------------------------------------------------------

test "bench run ritual: dummy slice flows end-to-end into clean joined records" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // --- Seed a project, plan, and task with a declared touch via the CLI. ---
    // registerRepoSlug registers the project AND returns the auto-generated
    // repo slug that `task touches add` resolves against.
    const repo_slug = registerRepoSlug(&suite, arena);
    const root = suite.tmpAbsPath();

    const plan_json = suite.mustRunInDir(root, &.{ "plan", "create", "--json", "Ritual plan" });
    defer gpa.free(plan_json);
    const plan = std.json.parseFromSlice(PlanId, arena, plan_json, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch @panic("parse PlanId failed");
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.value.id}) catch @panic("OOM");

    const task_json = suite.mustRunInDir(root, &.{ "task", "add", "--plan", plan_id, "--json", "Edit the slice file" });
    defer gpa.free(task_json);
    const task = std.json.parseFromSlice(TaskId, arena, task_json, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch @panic("parse TaskId failed");
    const task_id = std.fmt.allocPrint(arena, "{d}", .{task.value.id}) catch @panic("OOM");

    // The declared touch: the task says it will edit `slice.txt` in the repo.
    const declared_path = "slice.txt";
    const t_out = suite.mustRunInDir(root, &.{ "task", "touches", "add", task_id, repo_slug, "--path", declared_path });
    defer gpa.free(t_out);

    // --- Stand up the synthetic git worktree fixture (the dummy slice). ---
    const worktree = suite.freshSystemTmpDir();
    const base_sha = initGitFixture(gpa, arena, worktree, declared_path);

    const run_uid = "01JRITUAL00000000000000001";

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "bench_run_ritual.lua" });
    defer gpa.free(wf_path);

    // --args carries the run config + the worktree path (for `bench harvest`,
    // a `planar` verb that takes --worktree). --worktree on the engine
    // confines the git.* host calls in the setup phase.
    const args_json = try std.fmt.allocPrint(
        arena,
        "{{\"run_uid\":\"{s}\",\"plan_id\":{s},\"base_sha\":\"{s}\",\"config_hash\":\"cfg-1\",\"arm\":\"strict\",\"tasks\":[{s}],\"worktree\":\"{s}\",\"status\":\"completed\"}}",
        .{ run_uid, plan_id, base_sha, task_id, worktree },
    );

    // --- Phase A: setup (reset → start → snapshot declared touches). ---
    const setup_res = try runExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "setup", "--worktree", worktree, "--args", args_json,
    });
    defer setup_res.deinit();
    if (setup_res.term != .exited or setup_res.term.exited != 0) {
        std.debug.print("setup phase failed\nstdout: {s}\nstderr: {s}\n", .{ setup_res.stdout, setup_res.stderr });
        @panic("setup phase exited non-zero");
    }
    const SetupReflect = struct { ok: bool, declared_touches: i64 };
    const sr = try std.json.parseFromSlice(SetupReflect, gpa, std.mem.trim(u8, setup_res.stdout, " \t\r\n"), .{ .ignore_unknown_fields = true });
    defer sr.deinit();
    try std.testing.expect(sr.value.ok);
    // Exactly one declared touch was snapshotted (one path on one task).
    try std.testing.expectEqual(@as(i64, 1), sr.value.declared_touches);

    // --- Phase B (the caller's LLM step) — SIMULATED here with no LLM. ---
    // The "coder" edits the declared file. This is the dummy slice.
    const filepath = try std.fs.path.join(gpa, &.{ worktree, declared_path });
    defer gpa.free(filepath);
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = filepath,
        .data = "base contents\nedited by the dummy slice\n",
    }) catch @panic("simulated edit failed");

    // --- Phase C: measure (harvest actual → finish). ---
    const measure_res = try runExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "measure", "--worktree", worktree, "--args", args_json,
    });
    defer measure_res.deinit();
    if (measure_res.term != .exited or measure_res.term.exited != 0) {
        std.debug.print("measure phase failed\nstdout: {s}\nstderr: {s}\n", .{ measure_res.stdout, measure_res.stderr });
        @panic("measure phase exited non-zero");
    }
    const MeasureReflect = struct { ok: bool, status: []const u8, actual_touches: i64 };
    const mr = try std.json.parseFromSlice(MeasureReflect, gpa, std.mem.trim(u8, measure_res.stdout, " \t\r\n"), .{ .ignore_unknown_fields = true });
    defer mr.deinit();
    try std.testing.expect(mr.value.ok);
    try std.testing.expectEqualStrings("completed", mr.value.status);
    try std.testing.expect(mr.value.actual_touches >= 1);

    // --- Assert the joined records via bench show --json. ---
    const run = suite.mustRunJSON(ShowRun, arena, &.{ "bench", "show", run_uid, "--json" });

    try std.testing.expectEqualStrings(run_uid, run.run_uid);
    try std.testing.expectEqualStrings("strict", run.arm);
    try std.testing.expectEqualStrings(base_sha, run.base_sha);
    try std.testing.expectEqualStrings("completed", run.status);
    try std.testing.expectEqual(plan.value.id, run.plan_id);

    // Joined records: count declared vs actual touches, and prove every touch
    // is joined to THIS run's task with a non-empty path (no orphaned /
    // unjoinable rows).
    var declared_count: usize = 0;
    var actual_count: usize = 0;
    for (run.touches) |touch| {
        try std.testing.expectEqual(task.value.id, touch.task_id);
        try std.testing.expect(touch.path.len > 0);
        if (std.mem.eql(u8, touch.kind, "declared")) {
            declared_count += 1;
            // The declared snapshot recorded the predicted path.
            try std.testing.expectEqualStrings(declared_path, touch.path);
        } else if (std.mem.eql(u8, touch.kind, "actual")) {
            actual_count += 1;
            // Ground truth: the harvested path is the file the dummy slice edited.
            try std.testing.expectEqualStrings(declared_path, touch.path);
        } else {
            std.debug.print("unexpected touch kind: {s}\n", .{touch.kind});
            return error.UnexpectedTouchKind;
        }
    }
    // One declared (from setup), at least one actual (from harvest). For this
    // dummy slice declared == actual, so the run records a perfect prediction.
    try std.testing.expectEqual(@as(usize, 1), declared_count);
    try std.testing.expect(actual_count >= 1);
}
