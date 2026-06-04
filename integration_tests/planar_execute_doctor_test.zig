//! Integration tests for `planar-execute doctor` (plan 492 task 3236).
//!
//! This is the LIVE-BINARY gate that closes the "subprocess half is untested"
//! gap for planar-execute's read helpers. Until `doctor` existed, the
//! subprocess halves of `state.zig` (planShow / planNext / testSpecStatus),
//! `schema.zig` (loadSchema), and `worktree.zig` (reconcile dry-run read path)
//! were fixture-parse tested ONLY — no test ever drove them against a real
//! `planar` / `planar-agent` / `planar-watch` / `git` process. `doctor` is the
//! first caller that does, and this suite exercises it end-to-end against the
//! freshly-built binaries and a seeded fixture DB.
//!
//! Hermeticity (the load-bearing part):
//!   - The fixture DB is seeded via the real `planar` CLI (harness `Suite`,
//!     which injects `PLANAR_DB` = a temp fixture DB), NOT raw SQL.
//!   - `planar-execute doctor` runs with `PLANAR_DB` = that fixture DB so its
//!     CHILD `planar`/`planar-agent`/`planar-watch` reads hit the fixture, not
//!     the operator's real DB.
//!   - `PATH` is prepended with the directory holding the freshly-built binaries
//!     (derived from `PLANAR_BIN`'s dirname) so doctor's bare-name PATH lookups
//!     resolve to `./bin/...`, not a stale `~/.planar/bin`.
//!   - cwd for the doctor run is the planar repo root (a real git repo) so the
//!     reconcile dry-run's `git worktree list --porcelain` succeeds. The dry-run
//!     finds no worktree matching the fixture plan's slug → `stale_cycles: 0`,
//!     and mutates nothing (dry-run skips teardown + prune).

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Binary resolution: PLANAR_BIN (the planar binary) and PLANAR_EXECUTE_BIN
// (the harness binary under test) are set by `zig build test-integration`.
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

/// binDir returns the directory holding the freshly-built binaries, derived
/// from PLANAR_BIN's dirname. doctor's child processes resolve bare names
/// (`planar`, `planar-agent`, `planar-watch`, `git`) via PATH; prepending this
/// dir guarantees the children are the just-built binaries, not a stale install.
fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: zig build test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

const DoctorResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,

    fn deinit(self: DoctorResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }

    fn exitCode(self: DoctorResult) u32 {
        return switch (self.term) {
            .exited => |code| code,
            else => 255,
        };
    }
};

/// runDoctor spawns `planar-execute doctor <args...>` with the env doctor's
/// child processes need: PLANAR_DB pinned to the suite's fixture DB, and PATH
/// prepended with the freshly-built binaries' directory. cwd is set to
/// `repo_root` (a real git repo) so the reconcile dry-run's git enumeration
/// succeeds.
fn runDoctor(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    repo_root: []const u8,
    args: []const []const u8,
) !DoctorResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

    // Build an env map from the current process env, then override PLANAR_DB
    // and prepend the build-output bin dir onto PATH.
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    // Pin the child reads to the suite fixture DB (absolute path so the child's
    // non-default cwd does not mis-resolve a relative DB path).
    try env_map.put("PLANAR_DB", suite.absDbPath());

    // Prepend the freshly-built bin dir onto PATH so bare-name lookups of
    // planar / planar-agent / planar-watch resolve to the just-built binaries.
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .environ_map = &env_map,
        .cwd = .{ .path = repo_root },
    });
    return .{
        .term = result.term,
        .stdout = result.stdout,
        .stderr = result.stderr,
        .gpa = gpa,
    };
}

/// repoRoot returns the planar checkout root (a real git repo), derived from
/// `PLANAR_BIN`. `zig build` installs the binary at `<repo>/zig-out/bin/planar`,
/// so the repo root is `PLANAR_BIN`'s dirname climbed three times
/// (bin → zig-out → repo). Deriving from `PLANAR_BIN` (rather than the test
/// runner's cwd, which `Dir.realPath` cannot reliably resolve on the AT_FDCWD
/// handle) gives a stable absolute git-repo path for the doctor child's cwd so
/// the reconcile dry-run's `git worktree list` succeeds.
fn repoRoot(gpa: std.mem.Allocator) ![]u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: zig build test-integration");
    const bin = std.fs.path.dirname(planar_bin) orelse return error.NoRepoRoot; // .../zig-out/bin
    const out = std.fs.path.dirname(bin) orelse return error.NoRepoRoot; // .../zig-out
    const root = std.fs.path.dirname(out) orelse return error.NoRepoRoot; // .../<repo>
    return gpa.dupe(u8, root);
}

// ---------------------------------------------------------------------------
// JSON shapes mirroring the approved doctor report contract.
// ---------------------------------------------------------------------------

const SchemaProbe = struct {
    ok: bool,
    commands: ?u64 = null,
};
const PlanShowProbe = struct {
    ok: bool,
    title: ?[]const u8 = null,
};
const PlanNextProbe = struct {
    ok: bool,
    task_id: ?u64 = null,
};
const TestSpecProbe = struct {
    ok: bool,
    plans: ?u64 = null,
};
const ReconcileProbe = struct {
    ok: bool,
    stale_cycles: ?u64 = null,
};
const DoctorReport = struct {
    schema_reads: struct {
        planar: SchemaProbe,
        @"planar-agent": SchemaProbe,
    },
    plan_reads: struct {
        plan_show: PlanShowProbe,
        plan_next: PlanNextProbe,
        test_spec: TestSpecProbe,
    },
    reconcile_dry_run: ReconcileProbe,
    all_ok: bool,
};

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};
const TaskJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "planar-execute doctor: live-binary read probes against a seeded fixture DB (all-green)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // ---- 1. Seed the fixture DB via the real planar CLI (not raw SQL).
    _ = suite.registerProject("doctorlife");
    suite.addAssoc("doctorlife", "project");

    const plan_title = "Doctor probe target plan";
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--json", "--summary", "Plan that doctor's read probes exercise", plan_title,
    });
    try std.testing.expectEqualStrings(plan_title, plan.title);
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // A couple of tasks so plan next has an available row to report.
    const t1 = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",          "add",                "--json",     "--plan", plan_id_str,
        "--next-action", "do the first thing", "First task",
    });
    try std.testing.expectEqualStrings("todo", t1.status);
    const t2_out = suite.mustRun(&.{
        "task",          "add",       "--plan",             plan_id_str,
        "--next-action", "do second", "Second doctor task",
    });
    gpa.free(t2_out);

    // ---- 2. Run doctor against the fixture DB, with the built bins on PATH and
    //         cwd a real git repo (the planar checkout root).
    const root = try repoRoot(gpa);
    defer gpa.free(root);

    const res = try runDoctor(gpa, &suite, root, &.{ "doctor", "--plan", plan_id_str, "--json" });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\ndoctor stdout: {s}\ndoctor stderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    const parsed = try std.json.parseFromSlice(DoctorReport, arena, res.stdout, .{
        .ignore_unknown_fields = true,
    });
    const report = parsed.value;

    // ---- 3. Positive assertions. Counts use > 0 / presence, never equality,
    //         so CLI growth (schema command count, milestone count) does not
    //         make this test brittle.
    try std.testing.expect(report.schema_reads.planar.ok);
    try std.testing.expect(report.schema_reads.planar.commands != null);
    try std.testing.expect(report.schema_reads.planar.commands.? > 0);

    try std.testing.expect(report.schema_reads.@"planar-agent".ok);
    try std.testing.expect(report.schema_reads.@"planar-agent".commands != null);
    try std.testing.expect(report.schema_reads.@"planar-agent".commands.? > 0);

    try std.testing.expect(report.plan_reads.plan_show.ok);
    try std.testing.expect(report.plan_reads.plan_show.title != null);
    try std.testing.expectEqualStrings(plan_title, report.plan_reads.plan_show.title.?);

    try std.testing.expect(report.plan_reads.plan_next.ok);
    try std.testing.expect(report.plan_reads.test_spec.ok);

    // Reconcile dry-run: succeeds against the real repo, finds no worktree
    // matching the fixture plan's slug, mutates nothing.
    try std.testing.expect(report.reconcile_dry_run.ok);

    try std.testing.expect(report.all_ok);
}

test "planar-execute doctor: a nonexistent plan id drives a read failure (all_ok=false, non-zero exit)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Seed only the scope so the schema probes still pass — only the plan-read
    // probes should fail (proving the per-probe failure path is real, not a
    // blanket bail).
    _ = suite.registerProject("doctorneg");
    suite.addAssoc("doctorneg", "project");

    const root = try repoRoot(gpa);
    defer gpa.free(root);

    // A plan id that does not exist in the fixture DB.
    const res = try runDoctor(gpa, &suite, root, &.{ "doctor", "--plan", "987654321", "--json" });
    defer res.deinit();

    // Negative path: exit non-zero, and the report says all_ok=false.
    try std.testing.expect(res.exitCode() != 0);

    const parsed = try std.json.parseFromSlice(DoctorReport, arena, res.stdout, .{
        .ignore_unknown_fields = true,
    });
    const report = parsed.value;

    // Schema probes do not depend on the plan id — they stay green, proving
    // doctor collected every probe rather than bailing on the first failure.
    try std.testing.expect(report.schema_reads.planar.ok);
    try std.testing.expect(report.schema_reads.@"planar-agent".ok);

    // The plan-read probe failed (nonexistent plan → planar exits non-zero).
    try std.testing.expect(!report.plan_reads.plan_show.ok);

    // Aggregate: at least one probe failed.
    try std.testing.expect(!report.all_ok);
}
