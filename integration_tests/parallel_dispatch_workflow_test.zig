//! integration_tests/parallel_dispatch_workflow_test.zig — black-box tests for
//! `workflows/parallel-dispatch.lua` (plan 760 M1, tasks 4620–4624; plan 760
//! M2, tasks 4625–4627 — staged waves, wave barrier).
//!
//! M2 coverage (test-spec artifact 429):
//!
//!   wave-ordering / staged waves (Happy): the plan-753 shape (proto blocks
//!     identity + web) yields proto in wave 1 and identity + web in wave 2,
//!     each labeled blocked_by proto. The `blocks` graph orders/labels; the
//!     engine's recommend-strategy still gates eligibility.
//!
//!   wave-ordering (Edge): a strict A→B→C blocks-chain collapses to three
//!     sequential one-lane waves.
//!
//!   wave-ordering (Error): a task the engine serialized for a NON-blocks
//!     reason (rule 3 migration) is never placed in a wave — carried in the
//!     serialized set instead.
//!
//!   wave-ordering (Determinism): the `waves` phase is byte-stable across runs.
//!
//!   wave-barrier (Edge): the barrier holds when a lane completed but its
//!     fan-in has not run — barrier_check proceeds only when every lane is BOTH
//!     landed AND fanned in; a landed-but-not-fanned lane blocks the barrier.
//!
//! These tests exercise the deterministic, spawn-free wave/lane computation
//! seam through the planar-execute engine against a seeded isolated PLANAR_DB.
//! The seam computes waves/paths/branch-names/merge-order and HANDS BACK; it
//! never spawns coders and never runs git worktree/branch/merge. The model does
//! that. So these tests assert on the seam's JSON `flow.result` payload only.
//!
//! Covered scenarios (test-spec artifact 429):
//!
//!   single-wave-compute / seam-plan-phase (Happy): a plan whose
//!     recommend-strategy returns two disjoint eligible tasks yields ONE wave of
//!     two lanes, each with a distinct worktree path + lane branch, and carries
//!     the serialized set through untouched.
//!
//!   worktree-create (naming): epic branch is `epic/p<plan>-<slug>`, each lane
//!     branch is `cycle/p<plan>/<task-slug>` and worktree path mirrors it.
//!
//!   single-wave-compute (Empty-null): a plan with fewer than two eligible
//!     tasks (fan_out_available:false) yields NO fan-out wave — an empty lane
//!     list with fan_out=false, not an error.
//!
//!   manual-fan-in: the fan_in phase emits the stable (task-id-ordered) merge
//!     order and teardown list even when the input lanes are out of order.
//!
//!   Determinism: the seam is byte-stable across runs — running the same phase
//!     against the same DB twice yields identical stdout (a resume must
//!     recompute identical waves). Guaranteed by array-ordered lanes + sorted
//!     object keys in planar-execute's JSON serializer.
//!
//!   Spawn-free contract: the seam source contains no model-spawning primitive.
//!
//! The tests isolate PLANAR_DB via the harness Suite tmp dir and prepend the
//! harness binary dirs to PATH so planar-execute's inner `cli.planar` shells hit
//! the same binary + DB.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// resolveEnv — same pattern as dispatch_workflow_test.zig
// ---------------------------------------------------------------------------

fn resolveEnv(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key ++ "=")) return s[(key ++ "=").len..];
    }
    @panic(key ++ " is not set. Run via: make test-integration");
}

// ---------------------------------------------------------------------------
// RunResult / runExecute / mustExecute — mirror dispatch_workflow_test.zig
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
};

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

fn mustExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    args: []const []const u8,
) !RunResult {
    const res = try runExecute(gpa, cwd, db_path, args);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-execute failed (term={any})\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("mustExecute: planar-execute exited non-zero");
    }
    return res;
}

/// repoRootFromBin — derive repo root from PLANAR_BIN (three dirname levels:
/// bin/planar → bin → repo-root; the harness build lays the binary at
/// <root>/bin/planar so two dirname levels reach the root, but the dispatch
/// test uses three because PLANAR_BIN there is under zig-out. We resolve the
/// workflow relative to the binary's grand-grandparent, matching the dispatch
/// test's proven path, then fall back if the workflow is not there.
fn repoRootFromBin(allocator: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return allocator.dupe(u8, d3);
}

/// workflowPath resolves the seam path relative to the repo root, trying the
/// three-dirname root first (matches dispatch_workflow_test.zig) and the
/// two-dirname root as a fallback (when PLANAR_BIN is <root>/bin/planar).
fn workflowPath(gpa: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;

    // Candidate 1: three-dirname root (zig-out/bin/planar layout).
    if (std.fs.path.dirname(d2)) |d3| {
        const p = try std.fs.path.join(gpa, &.{ d3, "workflows", "parallel-dispatch.lua" });
        if (std.Io.Dir.cwd().access(std.testing.io, p, .{})) |_| {
            return p;
        } else |_| {
            gpa.free(p);
        }
    }
    // Candidate 2: two-dirname root (bin/planar layout).
    const p2 = try std.fs.path.join(gpa, &.{ d2, "workflows", "parallel-dispatch.lua" });
    return p2;
}

// ---------------------------------------------------------------------------
// JSON shapes for the seam result payloads.
// ---------------------------------------------------------------------------

const Lane = struct {
    task_id: i64,
    slug: []const u8,
    title: []const u8,
    branch: []const u8,
    worktree: []const u8,
};

const PlanResult = struct {
    plan_id: i64,
    fan_out: bool,
    epic_branch: []const u8,
    base: []const u8,
    lane_count: i64,
    lanes: []Lane = &.{},
};

const EmptyPlanResult = struct {
    plan_id: i64,
    fan_out: bool,
    lane_count: i64,
    reason: []const u8 = "",
};

const FanInResult = struct {
    plan_id: i64,
    lane_count: i64,
    merge_order: []const []const u8,
    teardown_worktrees: []const []const u8,
};

// ---------------------------------------------------------------------------
// M2 shapes: the `waves` (wave-ordering) and `barrier_check` (wave-barrier)
// phase result payloads.
// ---------------------------------------------------------------------------

const WaveLane = struct {
    task_id: i64,
    slug: []const u8,
    title: []const u8,
    branch: []const u8,
    worktree: []const u8,
    // `blocked_by` is parsed as a raw JSON value because the planar-execute
    // serializer emits an EMPTY Lua table as `{}` (a JSON object) and a
    // NON-empty one as `[…]` (a JSON array) — the same established empty-table
    // policy the M1 `serialized:{}` payload relies on. A typed `[]const i64`
    // slice cannot parse the `{}` form, so we take the Value and normalize via
    // blockedByIds below.
    blocked_by: std.json.Value = .null,
};

/// blockedByIds normalizes a WaveLane.blocked_by Value into a list of blocker
/// task ids. An empty blocked-by serializes as `{}` (object) → no ids; a
/// non-empty one serializes as an array of integers.
fn blockedByIds(gpa: std.mem.Allocator, v: std.json.Value) []i64 {
    switch (v) {
        .array => |arr| {
            var out = gpa.alloc(i64, arr.items.len) catch @panic("oom");
            for (arr.items, 0..) |item, i| {
                out[i] = switch (item) {
                    .integer => |n| n,
                    else => std.debug.panic("blocked_by element is not an integer", .{}),
                };
            }
            return out;
        },
        // `{}` (empty object) or null → no blockers.
        .object, .null => return &.{},
        else => std.debug.panic("blocked_by is neither an array nor an empty object", .{}),
    }
}

const Wave = struct {
    wave: i64,
    lane_count: i64,
    integration_pass: bool = false,
    // Raw Value: a functional wave's lanes serialize as an array of objects, but
    // the M3 terminal integration-pass wave carries NO lanes, which serializes as
    // `{}` (empty object) — a `[]WaveLane` slice cannot parse the `{}` form.
    // Normalize functional-wave lanes via waveLanes below.
    lanes: std.json.Value = .null,
};

/// waveLanes normalizes a Wave.lanes Value into a typed `[]WaveLane`. An empty
/// `{}` (integration-pass wave) or null → no lanes. Functional-wave lanes are
/// parsed field-by-field from the raw object.
fn waveLanes(gpa: std.mem.Allocator, v: std.json.Value) []WaveLane {
    switch (v) {
        .array => |arr| {
            var out = gpa.alloc(WaveLane, arr.items.len) catch @panic("oom");
            for (arr.items, 0..) |item, i| {
                const obj = switch (item) {
                    .object => |o| o,
                    else => std.debug.panic("wave lane is not an object", .{}),
                };
                out[i] = .{
                    .task_id = switch (obj.get("task_id") orelse std.debug.panic("lane missing task_id", .{})) {
                        .integer => |n| n,
                        else => std.debug.panic("lane task_id is not an integer", .{}),
                    },
                    .slug = strField(obj, "slug"),
                    .title = strField(obj, "title"),
                    .branch = strField(obj, "branch"),
                    .worktree = strField(obj, "worktree"),
                    .blocked_by = obj.get("blocked_by") orelse .null,
                };
            }
            return out;
        },
        .object, .null => return &.{},
        else => std.debug.panic("wave lanes is neither an array nor an empty object", .{}),
    }
}

/// strField reads a string field from a JSON object map (panics if missing or
/// not a string).
fn strField(obj: std.json.ObjectMap, key: []const u8) []const u8 {
    const v = obj.get(key) orelse std.debug.panic("object missing string field {s}", .{key});
    return switch (v) {
        .string => |s| s,
        else => std.debug.panic("field {s} is not a string", .{key}),
    };
}

const WavesResult = struct {
    plan_id: i64,
    epic_branch: []const u8,
    base: []const u8,
    wave_count: i64,
    integration_pass_wave: i64 = 0,
    waves: []Wave = &.{},
};

const BlockingLane = struct {
    task_id: i64,
    reason: []const u8,
};

const BarrierResult = struct {
    plan_id: i64,
    proceed: bool,
    // Parsed as a raw Value for the same empty-table reason as WaveLane.blocked_by:
    // an empty `blocking` serializes as `{}` (object), a non-empty one as an
    // array of objects. Normalize via blockingLanes below.
    blocking: std.json.Value = .null,
};

/// blockingLanes normalizes a BarrierResult.blocking Value into a list of
/// BlockingLane. Empty `{}`/null → no blocking lanes.
fn blockingLanes(gpa: std.mem.Allocator, v: std.json.Value) []BlockingLane {
    switch (v) {
        .array => |arr| {
            var out = gpa.alloc(BlockingLane, arr.items.len) catch @panic("oom");
            for (arr.items, 0..) |item, i| {
                const obj = switch (item) {
                    .object => |o| o,
                    else => std.debug.panic("blocking element is not an object", .{}),
                };
                const tid = obj.get("task_id") orelse std.debug.panic("blocking element missing task_id", .{});
                const reason = obj.get("reason") orelse std.debug.panic("blocking element missing reason", .{});
                out[i] = .{
                    .task_id = switch (tid) {
                        .integer => |n| n,
                        else => std.debug.panic("task_id is not an integer", .{}),
                    },
                    .reason = switch (reason) {
                        .string => |s| s,
                        else => std.debug.panic("reason is not a string", .{}),
                    },
                };
            }
            return out;
        },
        .object, .null => return &.{},
        else => std.debug.panic("blocking is neither an array nor an empty object", .{}),
    }
}

/// waveByNumber finds a wave by its 1-based number (panics if absent).
fn waveByNumber(res: WavesResult, n: i64) Wave {
    for (res.waves) |w| {
        if (w.wave == n) return w;
    }
    std.debug.panic("wave {d} not present in seam result", .{n});
}

/// waveContainsTask reports whether the given normalized lane set has a lane
/// for task_id.
fn waveContainsTask(lanes: []const WaveLane, task_id: i64) bool {
    for (lanes) |l| {
        if (l.task_id == task_id) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Seeding: register a project + org assoc, read the repo slug back, and add
// disjoint-touch tasks. Mirrors plan_recommend_strategy_test.zig's approach so
// the tasks flow through the SAME eligibility engine the seam consumes.
// ---------------------------------------------------------------------------

const PlanJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };

fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("pd-repo");
    const assoc_slug = "pd-org";
    suite.allocator.free(suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" }));
    const root = suite.tmpAbsPath();
    suite.allocator.free(suite.mustRun(&.{ "assoc", "add", assoc_slug, root }));

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", assoc_slug, "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "pd-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found in assoc members", .{});
}

fn addTaskSlug(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    pid: []const u8,
    slug: []const u8,
    title: []const u8,
) i64 {
    const t = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task", "add", "--plan", pid, "--slug", slug, "--json", title,
    });
    return t.id;
}

fn touchPath(suite: *harness.Suite, repo_slug: []const u8, task_id: i64, path: []const u8) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    gpa.free(suite.mustRun(&.{ "task", "touches", "add", tid, repo_slug, "--path", path }));
}

/// linkBlocks records a `blocks` edge `blocked -[blocks]-> blocker`, i.e.
/// `blocked` is blocked BY `blocker` (matches strategy.zig rule-1 semantics:
/// from_id is blocked BY to_id). This is the dependency edge the M2 `waves`
/// phase walks to ORDER and LABEL waves.
fn linkBlocks(suite: *harness.Suite, blocked_id: i64, blocker_id: i64) void {
    const gpa = suite.allocator;
    const from_ref = std.fmt.allocPrint(gpa, "task:{d}", .{blocked_id}) catch unreachable;
    defer gpa.free(from_ref);
    const to_ref = std.fmt.allocPrint(gpa, "task:{d}", .{blocker_id}) catch unreachable;
    defer gpa.free(to_ref);
    gpa.free(suite.mustRun(&.{ "links", "add", from_ref, to_ref, "--relationship", "blocks" }));
}

/// laneByTaskId finds a lane by task id in a PlanResult (panics if absent).
fn laneByTaskId(res: PlanResult, id: i64) Lane {
    for (res.lanes) |l| {
        if (l.task_id == id) return l;
    }
    std.debug.panic("lane for task {d} not present in seam result", .{id});
}

// ---------------------------------------------------------------------------
// 4620/4621/4622: seam plan phase — two disjoint eligible tasks compute one
// wave of two lanes with the locked naming convention.
// ---------------------------------------------------------------------------

test "parallel-dispatch plan: two disjoint eligible tasks compute one wave of two lanes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-happy", "--json", "PD_HAPPY",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_a = addTaskSlug(&suite, arena, pid, "lane-alpha", "Alpha lane");
    touchPath(&suite, repo, t_a, "src/alpha.zig");
    const t_b = addTaskSlug(&suite, arena, pid, "lane-bravo", "Bravo lane");
    touchPath(&suite, repo, t_b, "src/bravo.zig");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer res.deinit();

    const out = std.mem.trim(u8, res.stdout, " \t\r\n");
    const parsed = try std.json.parseFromSlice(PlanResult, arena, out, .{ .ignore_unknown_fields = true });

    try std.testing.expectEqual(plan.id, parsed.value.plan_id);
    try std.testing.expect(parsed.value.fan_out);
    try std.testing.expectEqual(@as(i64, 2), parsed.value.lane_count);
    try std.testing.expectEqual(@as(usize, 2), parsed.value.lanes.len);

    // Epic branch: epic/p<plan>-<plan-slug>.
    const want_epic = try std.fmt.allocPrint(arena, "epic/p{d}-pd-happy", .{plan.id});
    try std.testing.expectEqualStrings(want_epic, parsed.value.epic_branch);
    try std.testing.expectEqualStrings("HEAD", parsed.value.base);

    // Lanes are sorted by task id (byte-stable ordering).
    try std.testing.expect(parsed.value.lanes[0].task_id < parsed.value.lanes[1].task_id);

    // Each lane's branch + worktree follow the locked naming convention.
    const la = laneByTaskId(parsed.value, t_a);
    const want_branch_a = try std.fmt.allocPrint(arena, "cycle/p{d}/lane-alpha", .{plan.id});
    const want_wt_a = try std.fmt.allocPrint(arena, ".worktrees/cycle/p{d}/lane-alpha", .{plan.id});
    try std.testing.expectEqualStrings(want_branch_a, la.branch);
    try std.testing.expectEqualStrings(want_wt_a, la.worktree);
    try std.testing.expectEqualStrings("lane-alpha", la.slug);

    const lb = laneByTaskId(parsed.value, t_b);
    const want_branch_b = try std.fmt.allocPrint(arena, "cycle/p{d}/lane-bravo", .{plan.id});
    try std.testing.expectEqualStrings(want_branch_b, lb.branch);

    // Worktree paths are DISTINCT per lane.
    try std.testing.expect(!std.mem.eql(u8, la.worktree, lb.worktree));
}

// ---------------------------------------------------------------------------
// seam-plan-phase: the serialized set is carried through untouched (a
// serialized task is never placed in a lane).
// ---------------------------------------------------------------------------

test "parallel-dispatch plan: serialized task is carried through, never placed in a lane" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-mixed", "--json", "PD_MIXED",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Two eligible + one serialized (touches a migration → rule 3).
    const t_a = addTaskSlug(&suite, arena, pid, "elig-a", "Eligible A");
    touchPath(&suite, repo, t_a, "src/a.zig");
    const t_b = addTaskSlug(&suite, arena, pid, "elig-b", "Eligible B");
    touchPath(&suite, repo, t_b, "src/b.zig");
    const t_mig = addTaskSlug(&suite, arena, pid, "mig", "Migrator");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.sql");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    // The two eligible tasks are lanes; the migrator is NOT.
    const parsed = try std.json.parseFromSlice(PlanResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(@as(i64, 2), parsed.value.lane_count);
    for (parsed.value.lanes) |l| {
        try std.testing.expect(l.task_id != t_mig);
    }

    // The serialized task surfaces in the result (carried through). We assert on
    // the raw JSON so the excluded_by reason is visible in the payload.
    const mig_ref = try std.fmt.allocPrint(arena, "\"task_id\":{d}", .{t_mig});
    try std.testing.expect(std.mem.indexOf(u8, out, "\"serialized\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, mig_ref) != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"excluded_by\"") != null);
}

// ---------------------------------------------------------------------------
// single-wave-compute (Empty-null): fewer than two eligible tasks yields NO
// fan-out wave (empty lane list, fan_out=false), not an error.
// ---------------------------------------------------------------------------

test "parallel-dispatch plan: fewer than two eligible tasks yields no fan-out wave" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-empty", "--json", "PD_EMPTY",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Only one eligible task → fan_out_available:false.
    const t_only = addTaskSlug(&suite, arena, pid, "only-lane", "Only lane");
    touchPath(&suite, repo, t_only, "src/only.zig");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(EmptyPlanResult, arena, out, .{ .ignore_unknown_fields = true });
    // Legitimately empty, not an error: exit 0 (asserted by mustExecute),
    // fan_out=false, lane_count=0, with a human reason.
    try std.testing.expect(!parsed.value.fan_out);
    try std.testing.expectEqual(@as(i64, 0), parsed.value.lane_count);
    try std.testing.expect(parsed.value.reason.len > 0);
}

// ---------------------------------------------------------------------------
// manual-fan-in: the fan_in phase emits a stable (task-id-ordered) merge order
// and teardown list even when the input lanes arrive out of order.
// ---------------------------------------------------------------------------

test "parallel-dispatch fan_in: merge order is stable by task id regardless of input order" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-fanin");
    suite.addAssoc("pd-fanin", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    // Input lanes deliberately OUT of task-id order (7 before 3).
    const args_json =
        \\{"plan_id":42,"lanes":[
        \\{"task_id":7,"branch":"cycle/p42/lane-g","worktree":".worktrees/cycle/p42/lane-g"},
        \\{"task_id":3,"branch":"cycle/p42/lane-c","worktree":".worktrees/cycle/p42/lane-c"}
        \\]}
    ;

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "fan_in", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(FanInResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(@as(i64, 42), parsed.value.plan_id);
    try std.testing.expectEqual(@as(i64, 2), parsed.value.lane_count);

    // Merge order sorted ascending by task id: lane-c (3) before lane-g (7).
    try std.testing.expectEqual(@as(usize, 2), parsed.value.merge_order.len);
    try std.testing.expectEqualStrings("cycle/p42/lane-c", parsed.value.merge_order[0]);
    try std.testing.expectEqualStrings("cycle/p42/lane-g", parsed.value.merge_order[1]);

    // Teardown list follows the same stable order.
    try std.testing.expectEqualStrings(".worktrees/cycle/p42/lane-c", parsed.value.teardown_worktrees[0]);
    try std.testing.expectEqualStrings(".worktrees/cycle/p42/lane-g", parsed.value.teardown_worktrees[1]);
}

// ---------------------------------------------------------------------------
// Determinism: the seam is byte-stable across runs. Running the plan phase
// against the same DB twice yields identical stdout (a resume must recompute
// identical waves). This is the load-bearing reason the computation lives in a
// deterministic seam and not in drift-prone skill prose.
// ---------------------------------------------------------------------------

test "parallel-dispatch plan: output is byte-stable across repeated runs" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-stable", "--json", "PD_STABLE",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Three eligible tasks so the lane array + several object keys exercise both
    // the array ordering and the sorted-object-key path.
    const t1 = addTaskSlug(&suite, arena, pid, "lane-one", "One");
    touchPath(&suite, repo, t1, "src/one.zig");
    const t2 = addTaskSlug(&suite, arena, pid, "lane-two", "Two");
    touchPath(&suite, repo, t2, "src/two.zig");
    const t3 = addTaskSlug(&suite, arena, pid, "lane-three", "Three");
    touchPath(&suite, repo, t3, "src/three.zig");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const r1 = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer r1.deinit();
    const r2 = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer r2.deinit();

    // The whole stdout (including object key order) must be byte-identical.
    try std.testing.expectEqualStrings(r1.stdout, r2.stdout);
    // Sanity: it is a fan-out wave of three lanes (not a trivially-equal empty).
    try std.testing.expect(std.mem.indexOf(u8, r1.stdout, "\"fan_out\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, r1.stdout, "\"lane_count\":3") != null);
}

// ---------------------------------------------------------------------------
// Spawn-free contract: the seam source contains no model-spawning primitive and
// no git worktree/branch/merge (those are the model's job — the seam only
// computes and hands back).
// ---------------------------------------------------------------------------

test "parallel-dispatch source is spawn-free and computes only (no git worktree/merge)" {
    const gpa = std.testing.allocator;

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    const source = std.Io.Dir.cwd().readFileAlloc(std.testing.io, wf_path, gpa, .limited(256 * 1024)) catch
        @panic("failed to read parallel-dispatch.lua");
    defer gpa.free(source);

    // No model-spawning / general-exec primitives (mirrors host.zig DENIED set).
    const forbidden = [_][]const u8{
        "os.execute", "io.open", "spawn(", "agent(", "claude", "codex", "headless",
    };
    for (forbidden) |bad| {
        if (std.mem.indexOf(u8, source, bad) != null) {
            std.debug.print("parallel-dispatch.lua references a forbidden primitive: {s}\n", .{bad});
            return error.SpawnPrimitiveFound;
        }
    }

    // The seam does NOT itself run worktree/branch/merge git ops — those are the
    // model's job. It calls git host fns for none of these; assert the source
    // never names a git.* mutation the confined host surface does not even
    // expose (git worktree add / git merge). If a later milestone wires these,
    // that is a deliberate change and this assertion is the tripwire.
    if (std.mem.indexOf(u8, source, "git.worktree") != null or
        std.mem.indexOf(u8, source, "git.merge") != null or
        std.mem.indexOf(u8, source, "git.branch") != null)
    {
        std.debug.print("parallel-dispatch.lua runs a git op the seam must hand back to the model\n", .{});
        return error.SeamRunsGitOp;
    }
}

// ===========================================================================
// M2 (plan 760, tasks 4625–4627): staged waves.
// ===========================================================================

// ---------------------------------------------------------------------------
// wave-ordering (4625): staged waves land the contract lane before dependent
// lanes (plan-753 shape — proto blocks identity + web).
//
// The engine's recommend-strategy gates eligibility (proto is the only
// currently-eligible task; identity/web are rule-1 serialized because proto is
// not done). The seam walks the `blocks` graph ONLY to ORDER and LABEL: it must
// place proto in wave 1 and identity + web in wave 2, each labeled `blocked_by`
// proto. The seam never decides eligibility.
// ---------------------------------------------------------------------------

test "parallel-dispatch waves: staged waves land contract lane before dependent lanes" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-staged", "--json", "PD_STAGED",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // proto (contract lane) blocks identity + web.
    const t_proto = addTaskSlug(&suite, arena, pid, "proto", "Proto contract");
    touchPath(&suite, repo, t_proto, "proto/schema.zig");
    const t_ident = addTaskSlug(&suite, arena, pid, "identity", "Identity");
    touchPath(&suite, repo, t_ident, "identity/user.zig");
    const t_web = addTaskSlug(&suite, arena, pid, "web", "Web");
    touchPath(&suite, repo, t_web, "web/handler.zig");
    linkBlocks(&suite, t_ident, t_proto); // identity blocked_by proto
    linkBlocks(&suite, t_web, t_proto); // web blocked_by proto
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "waves", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(WavesResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(plan.id, parsed.value.plan_id);
    // Two functional waves + the M3 terminal integration pass = 3 total.
    try std.testing.expectEqual(@as(i64, 3), parsed.value.wave_count);
    try std.testing.expectEqual(@as(i64, 3), parsed.value.integration_pass_wave);

    // Epic branch name is the locked convention.
    const want_epic = try std.fmt.allocPrint(arena, "epic/p{d}-pd-staged", .{plan.id});
    try std.testing.expectEqualStrings(want_epic, parsed.value.epic_branch);

    // Wave 1 = proto only (the contract lane), unblocked.
    const w1 = waveByNumber(parsed.value, 1);
    const w1_lanes = waveLanes(arena, w1.lanes);
    try std.testing.expectEqual(@as(i64, 1), w1.lane_count);
    try std.testing.expect(waveContainsTask(w1_lanes, t_proto));
    const w1_blk = blockedByIds(arena, w1_lanes[0].blocked_by);
    try std.testing.expectEqual(@as(usize, 0), w1_blk.len);

    // Wave 2 = identity + web, each labeled blocked_by proto.
    const w2 = waveByNumber(parsed.value, 2);
    const w2_lanes = waveLanes(arena, w2.lanes);
    try std.testing.expectEqual(@as(i64, 2), w2.lane_count);
    try std.testing.expect(waveContainsTask(w2_lanes, t_ident));
    try std.testing.expect(waveContainsTask(w2_lanes, t_web));
    for (w2_lanes) |l| {
        const blk = blockedByIds(arena, l.blocked_by);
        try std.testing.expectEqual(@as(usize, 1), blk.len);
        try std.testing.expectEqual(t_proto, blk[0]);
        // Lane branch/worktree follow the locked naming convention.
        const want_branch = try std.fmt.allocPrint(arena, "cycle/p{d}/{s}", .{ plan.id, l.slug });
        try std.testing.expectEqualStrings(want_branch, l.branch);
    }

    // Wave 2 lanes are sorted by task id (byte-stable ordering).
    try std.testing.expect(w2_lanes[0].task_id < w2_lanes[1].task_id);

    // The M3 terminal wave is a cross-lane integration pass carrying no lanes.
    const w3 = waveByNumber(parsed.value, 3);
    try std.testing.expect(w3.integration_pass);
    try std.testing.expectEqual(@as(usize, 0), waveLanes(arena, w3.lanes).len);
}

// ---------------------------------------------------------------------------
// wave-ordering (4625) — Edge: a strict blocks-chain (A blocks B blocks C)
// collapses to three sequential one-lane waves rather than one three-lane wave.
// ---------------------------------------------------------------------------

test "parallel-dispatch waves: strict blocks-chain collapses to sequential one-lane waves" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-chain", "--json", "PD_CHAIN",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_a = addTaskSlug(&suite, arena, pid, "chain-a", "A");
    touchPath(&suite, repo, t_a, "a.zig");
    const t_b = addTaskSlug(&suite, arena, pid, "chain-b", "B");
    touchPath(&suite, repo, t_b, "b.zig");
    const t_c = addTaskSlug(&suite, arena, pid, "chain-c", "C");
    touchPath(&suite, repo, t_c, "c.zig");
    linkBlocks(&suite, t_b, t_a); // B blocked_by A
    linkBlocks(&suite, t_c, t_b); // C blocked_by B
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "waves", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(WavesResult, arena, out, .{ .ignore_unknown_fields = true });
    // Three single-lane functional waves in dependency order + the M3 terminal
    // integration pass = 4 total waves.
    try std.testing.expectEqual(@as(i64, 4), parsed.value.wave_count);
    try std.testing.expectEqual(@as(i64, 4), parsed.value.integration_pass_wave);
    const w1 = waveByNumber(parsed.value, 1);
    const w2 = waveByNumber(parsed.value, 2);
    const w3 = waveByNumber(parsed.value, 3);
    try std.testing.expectEqual(@as(i64, 1), w1.lane_count);
    try std.testing.expectEqual(@as(i64, 1), w2.lane_count);
    try std.testing.expectEqual(@as(i64, 1), w3.lane_count);
    const w1_lanes = waveLanes(arena, w1.lanes);
    const w2_lanes = waveLanes(arena, w2.lanes);
    const w3_lanes = waveLanes(arena, w3.lanes);
    try std.testing.expect(waveContainsTask(w1_lanes, t_a));
    try std.testing.expect(waveContainsTask(w2_lanes, t_b));
    try std.testing.expect(waveContainsTask(w3_lanes, t_c));
    // Each dependent wave labels its gating blocker.
    const w2_blk = blockedByIds(arena, w2_lanes[0].blocked_by);
    const w3_blk = blockedByIds(arena, w3_lanes[0].blocked_by);
    try std.testing.expectEqual(@as(usize, 1), w2_blk.len);
    try std.testing.expectEqual(t_a, w2_blk[0]);
    try std.testing.expectEqual(@as(usize, 1), w3_blk.len);
    try std.testing.expectEqual(t_b, w3_blk[0]);
    // The terminal wave (4) is the integration pass.
    try std.testing.expect(waveByNumber(parsed.value, 4).integration_pass);
}

// ---------------------------------------------------------------------------
// wave-ordering (4625) — Error: a task the engine serialized for a NON-blocks
// reason (rule 3 migration) is never placed in a wave; it is carried in the
// serialized set with its excluded_by reason. The seam does NOT override the
// engine's eligibility decision even though the task has no blocks edge.
// ---------------------------------------------------------------------------

test "parallel-dispatch waves: engine-serialized non-blocks task is never placed in a wave" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-wmix", "--json", "PD_WMIX",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_proto = addTaskSlug(&suite, arena, pid, "wproto", "Proto");
    touchPath(&suite, repo, t_proto, "proto.zig");
    const t_dep = addTaskSlug(&suite, arena, pid, "wdep", "Dep");
    touchPath(&suite, repo, t_dep, "dep.zig");
    const t_mig = addTaskSlug(&suite, arena, pid, "wmig", "Migrator");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.sql");
    linkBlocks(&suite, t_dep, t_proto); // dep blocked_by proto
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "waves", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(WavesResult, arena, out, .{ .ignore_unknown_fields = true });
    // Two functional waves (proto w1, dep w2) + the M3 terminal integration
    // pass = 3 total. The migrator is in NEITHER functional wave.
    try std.testing.expectEqual(@as(i64, 3), parsed.value.wave_count);
    try std.testing.expectEqual(@as(i64, 3), parsed.value.integration_pass_wave);
    for (parsed.value.waves) |w| {
        try std.testing.expect(!waveContainsTask(waveLanes(arena, w.lanes), t_mig));
    }
    // The migrator surfaces in the serialized set with its excluded_by reason.
    const mig_ref = try std.fmt.allocPrint(arena, "\"task_id\":{d}", .{t_mig});
    try std.testing.expect(std.mem.indexOf(u8, out, "\"serialized\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, mig_ref) != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"excluded_by\"") != null);
}

// ---------------------------------------------------------------------------
// wave-ordering (4625) — Determinism: the `waves` phase is byte-stable across
// runs (a resume must recompute an identical wave ordering).
// ---------------------------------------------------------------------------

test "parallel-dispatch waves: output is byte-stable across repeated runs" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-wstable", "--json", "PD_WSTABLE",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_proto = addTaskSlug(&suite, arena, pid, "sproto", "Proto");
    touchPath(&suite, repo, t_proto, "proto.zig");
    const t_x = addTaskSlug(&suite, arena, pid, "sx", "X");
    touchPath(&suite, repo, t_x, "x.zig");
    const t_y = addTaskSlug(&suite, arena, pid, "sy", "Y");
    touchPath(&suite, repo, t_y, "y.zig");
    linkBlocks(&suite, t_x, t_proto);
    linkBlocks(&suite, t_y, t_proto);
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const r1 = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "waves", "--args", args_json,
    });
    defer r1.deinit();
    const r2 = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "waves", "--args", args_json,
    });
    defer r2.deinit();

    try std.testing.expectEqualStrings(r1.stdout, r2.stdout);
    // Two functional waves + the M3 terminal integration pass = 3.
    try std.testing.expect(std.mem.indexOf(u8, r1.stdout, "\"wave_count\":3") != null);
}

// ---------------------------------------------------------------------------
// wave-barrier (4626): the barrier holds when a lane completes but its fan-in
// has NOT run. barrier_check(proceed) is true only when every lane is BOTH
// landed AND fanned_in; a landed-but-not-fanned lane holds the barrier.
// ---------------------------------------------------------------------------

test "parallel-dispatch barrier_check: barrier holds when a lane is landed but not fanned in" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-barrier");
    suite.addAssoc("pd-barrier", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    // Lane 5 landed but NOT fanned in; lane 2 landed AND fanned in. The barrier
    // must NOT proceed, and lane 5 is named as blocking (not_fanned_in).
    const args_json =
        \\{"plan_id":9,"lanes":[
        \\{"task_id":5,"landed":true,"fanned_in":false},
        \\{"task_id":2,"landed":true,"fanned_in":true}
        \\]}
    ;

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "barrier_check", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(BarrierResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(@as(i64, 9), parsed.value.plan_id);
    try std.testing.expect(!parsed.value.proceed);
    const blk = blockingLanes(arena, parsed.value.blocking);
    try std.testing.expectEqual(@as(usize, 1), blk.len);
    try std.testing.expectEqual(@as(i64, 5), blk[0].task_id);
    try std.testing.expectEqualStrings("not_fanned_in", blk[0].reason);
}

// ---------------------------------------------------------------------------
// wave-barrier (4626): the barrier proceeds only when every lane is landed AND
// fanned in; a not-yet-landed lane blocks with reason `not_landed`.
// ---------------------------------------------------------------------------

test "parallel-dispatch barrier_check: proceeds iff every lane is landed and fanned in" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-barrier2");
    suite.addAssoc("pd-barrier2", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    // All lanes landed AND fanned in → proceed.
    const ok_args =
        \\{"plan_id":1,"lanes":[
        \\{"task_id":3,"landed":true,"fanned_in":true},
        \\{"task_id":1,"landed":true,"fanned_in":true}
        \\]}
    ;
    const ok = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "barrier_check", "--args", ok_args,
    });
    defer ok.deinit();
    const ok_out = std.mem.trim(u8, ok.stdout, " \t\r\n");
    const ok_parsed = try std.json.parseFromSlice(BarrierResult, arena, ok_out, .{ .ignore_unknown_fields = true });
    try std.testing.expect(ok_parsed.value.proceed);
    try std.testing.expectEqual(@as(usize, 0), blockingLanes(arena, ok_parsed.value.blocking).len);

    // A not-yet-landed lane holds the barrier with reason not_landed; blocking
    // is sorted by task id.
    const blocked_args =
        \\{"plan_id":1,"lanes":[
        \\{"task_id":7,"landed":false,"fanned_in":false},
        \\{"task_id":4,"landed":true,"fanned_in":true}
        \\]}
    ;
    const bl = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "barrier_check", "--args", blocked_args,
    });
    defer bl.deinit();
    const bl_out = std.mem.trim(u8, bl.stdout, " \t\r\n");
    const bl_parsed = try std.json.parseFromSlice(BarrierResult, arena, bl_out, .{ .ignore_unknown_fields = true });
    try std.testing.expect(!bl_parsed.value.proceed);
    const bl_blk = blockingLanes(arena, bl_parsed.value.blocking);
    try std.testing.expectEqual(@as(usize, 1), bl_blk.len);
    try std.testing.expectEqual(@as(i64, 7), bl_blk[0].task_id);
    try std.testing.expectEqualStrings("not_landed", bl_blk[0].reason);
}

// ===========================================================================
// M3 (plan 763, tasks 4628–4632): fan-in conflict protocol + partial-wave
// failure hardening.
//
// M3 coverage (test-spec artifact 429):
//
//   fan-in-merge-order (Happy): the wave-aware fan_in phase orders lane merges
//     contract-lanes-first — an earlier-wave lane before a later-wave lane;
//     within a wave, stable by task id — across the FULL ordered wave list, not
//     just one wave. Seam-testable: it computes the merge order from the input
//     lanes' `wave` + `task_id`. The actual `git merge --no-ff` is the model's
//     runtime op (the confined git host group exposes no merge verb).
//
//   integration-pass (Happy): the `waves` phase marks a terminal cross-lane
//     integration pass as the last wave (integration_pass=true, no lanes),
//     running AFTER every functional lane fans in. Seam-testable: it emits the
//     terminal wave marker + integration_pass_wave number. The build+test of the
//     merged epic branch is the model's runtime op.
//
//   boundary-conflict-escalation (Error): the conflict_escalation phase formats
//     the escalation payload (sorted conflicting paths + the two lane branches,
//     auto_resolved=false always) given the conflict info. Seam-testable: it is
//     pure formatting. The actual merge + `git merge --abort` is the model's
//     runtime op — the seam never auto-resolves (structural: no merge host verb).
//
//   partial-wave-failure (Error): the reconcile_plan phase distinguishes a CLEAN
//     lane failure (atomic `planar-agent fail` → no reconcile, available again)
//     from a DEAD-CODER abandonment (stranded claim → `planar-agent reconcile
//     --stale-after 0`). Seam-testable: it partitions lane outcomes and emits the
//     reconcile argv. The actual `planar-agent reconcile` is the model's runtime
//     op; the barrier-hold is M2's already-tested barrier_check.
//
//   resume-recompute (Empty-null + Error): resume RECOMPUTES waves from CURRENT
//     task state — landed lanes are `done`, drop out of recommend-strategy, and
//     only the remainder re-fans-out. This is a property of the STATELESS seam
//     (it reads current state on each call). Proven by seeding lanes `done` and
//     asserting they are absent from `waves`/`plan` output, incl. the all-landed
//     ⇒ empty-remainder case.
// ===========================================================================

// M3 result shapes.

const MergeOrderFanIn = struct {
    plan_id: i64,
    lane_count: i64,
    merge_order: []const []const u8,
    teardown_worktrees: []const []const u8,
};

const WaveM3 = struct {
    wave: i64,
    lane_count: i64,
    integration_pass: bool = false,
    // Raw Value: a functional wave's lanes serialize as an array of objects, but
    // the terminal integration-pass wave carries NO lanes, which serializes as
    // `{}` (empty object) under the established empty-table policy — a `[]WaveLane`
    // slice cannot parse the `{}` form. Normalize via m3WaveLanes below.
    lanes: std.json.Value = .null,
};

/// M3WaveLane is the normalized per-lane view used by the M3 tests (task id +
/// blocked_by only — the fields the M3 assertions actually touch).
const M3WaveLane = struct {
    task_id: i64,
    blocked_by: std.json.Value = .null,
};

/// m3WaveLanes normalizes a WaveM3.lanes Value into a list of M3WaveLane. An
/// empty `{}` (integration-pass wave) or null → no lanes.
fn m3WaveLanes(gpa: std.mem.Allocator, v: std.json.Value) []M3WaveLane {
    switch (v) {
        .array => |arr| {
            var out = gpa.alloc(M3WaveLane, arr.items.len) catch @panic("oom");
            for (arr.items, 0..) |item, i| {
                const obj = switch (item) {
                    .object => |o| o,
                    else => std.debug.panic("lane element is not an object", .{}),
                };
                const tid = obj.get("task_id") orelse std.debug.panic("lane missing task_id", .{});
                out[i] = .{
                    .task_id = switch (tid) {
                        .integer => |n| n,
                        else => std.debug.panic("lane task_id is not an integer", .{}),
                    },
                    .blocked_by = obj.get("blocked_by") orelse .null,
                };
            }
            return out;
        },
        .object, .null => return &.{},
        else => std.debug.panic("lanes is neither an array nor an empty object", .{}),
    }
}

const WavesResultM3 = struct {
    plan_id: i64,
    epic_branch: []const u8,
    wave_count: i64,
    integration_pass_wave: i64 = 0,
    waves: []WaveM3 = &.{},
};

const ConflictBranches = struct {
    ours: []const u8,
    theirs: []const u8,
};

const ConflictResult = struct {
    plan_id: i64,
    conflict: bool,
    auto_resolved: bool,
    action: []const u8,
    branches: ConflictBranches,
    conflicting_paths: []const []const u8,
};

const ReconcileLane = struct {
    task_id: i64,
    reason: []const u8 = "",
};

const ReconcileResult = struct {
    plan_id: i64,
    needs_reconcile: bool,
    // Empty tables serialize as `{}` (object); non-empty as arrays. Parse as raw
    // Value and normalize, same policy as blocked_by / blocking above.
    reconcile: std.json.Value = .null,
    available: std.json.Value = .null,
    landed: std.json.Value = .null,
};

/// reconcileLaneIds normalizes a reconcile-list Value into task ids.
fn reconcileLaneIds(gpa: std.mem.Allocator, v: std.json.Value) []i64 {
    switch (v) {
        .array => |arr| {
            var out = gpa.alloc(i64, arr.items.len) catch @panic("oom");
            for (arr.items, 0..) |item, i| {
                const obj = switch (item) {
                    .object => |o| o,
                    else => std.debug.panic("reconcile element is not an object", .{}),
                };
                const tid = obj.get("task_id") orelse std.debug.panic("reconcile element missing task_id", .{});
                out[i] = switch (tid) {
                    .integer => |n| n,
                    else => std.debug.panic("task_id is not an integer", .{}),
                };
            }
            return out;
        },
        .object, .null => return &.{},
        else => std.debug.panic("reconcile list is neither array nor empty object", .{}),
    }
}

/// m3WaveByNumber finds a WaveM3 by its 1-based number (panics if absent).
fn m3WaveByNumber(res: WavesResultM3, n: i64) WaveM3 {
    for (res.waves) |w| {
        if (w.wave == n) return w;
    }
    std.debug.panic("wave {d} not present in m3 seam result", .{n});
}

// ---------------------------------------------------------------------------
// fan-in-merge-order (4628) — Happy: the wave-aware fan_in orders lane merges
// contract-lanes-first (earlier wave first; task id within a wave). A wave-2
// lane with a LOWER task id than a wave-1 lane still merges AFTER it, proving
// the order is wave-first, not task-id-first.
// ---------------------------------------------------------------------------

test "parallel-dispatch fan_in: merge order is wave-aware, contract-lanes-first" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-m3-order");
    suite.addAssoc("pd-m3-order", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    // Wave 1 has a HIGH-task-id lane (99); wave 2 has a LOW-task-id lane (10).
    // Contract-lanes-first: wave 1 (task 99) MUST merge before wave 2 (task 10),
    // even though 10 < 99. Within wave 1, tasks sort by id (30 before 99). Input
    // is deliberately out of order to prove the seam sorts.
    const args_json =
        \\{"plan_id":77,"lanes":[
        \\{"task_id":10,"wave":2,"branch":"cycle/p77/w2-low","worktree":".worktrees/cycle/p77/w2-low"},
        \\{"task_id":99,"wave":1,"branch":"cycle/p77/w1-high","worktree":".worktrees/cycle/p77/w1-high"},
        \\{"task_id":30,"wave":1,"branch":"cycle/p77/w1-mid","worktree":".worktrees/cycle/p77/w1-mid"}
        \\]}
    ;

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "fan_in", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(MergeOrderFanIn, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(@as(i64, 77), parsed.value.plan_id);
    try std.testing.expectEqual(@as(i64, 3), parsed.value.lane_count);

    // Order: wave 1 (task 30, then 99), then wave 2 (task 10). Contract lanes
    // (earlier wave) first; task-id secondary within a wave.
    try std.testing.expectEqual(@as(usize, 3), parsed.value.merge_order.len);
    try std.testing.expectEqualStrings("cycle/p77/w1-mid", parsed.value.merge_order[0]);
    try std.testing.expectEqualStrings("cycle/p77/w1-high", parsed.value.merge_order[1]);
    try std.testing.expectEqualStrings("cycle/p77/w2-low", parsed.value.merge_order[2]);

    // Teardown follows the same wave-aware order.
    try std.testing.expectEqualStrings(".worktrees/cycle/p77/w1-mid", parsed.value.teardown_worktrees[0]);
    try std.testing.expectEqualStrings(".worktrees/cycle/p77/w2-low", parsed.value.teardown_worktrees[2]);
}

// ---------------------------------------------------------------------------
// fan-in-merge-order (4628) — Regression: lanes with NO `wave` field collapse
// to a single wave and reproduce the M1 task-id order exactly (backward compat).
// ---------------------------------------------------------------------------

test "parallel-dispatch fan_in: lanes without a wave field collapse to single-wave task-id order" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-m3-nowave");
    suite.addAssoc("pd-m3-nowave", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    const args_json =
        \\{"plan_id":8,"lanes":[
        \\{"task_id":7,"branch":"cycle/p8/lane-g","worktree":".worktrees/cycle/p8/lane-g"},
        \\{"task_id":3,"branch":"cycle/p8/lane-c","worktree":".worktrees/cycle/p8/lane-c"}
        \\]}
    ;

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "fan_in", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(MergeOrderFanIn, arena, out, .{ .ignore_unknown_fields = true });
    // No `wave` field ⇒ single wave, stable by task id (3 before 7): identical
    // to the M1 manual-fan-in contract.
    try std.testing.expectEqualStrings("cycle/p8/lane-c", parsed.value.merge_order[0]);
    try std.testing.expectEqualStrings("cycle/p8/lane-g", parsed.value.merge_order[1]);
}

// ---------------------------------------------------------------------------
// integration-pass (4630) — Happy: the `waves` phase marks a terminal
// cross-lane integration pass as the last wave (integration_pass=true, no
// lanes), running AFTER every functional lane.
// ---------------------------------------------------------------------------

test "parallel-dispatch waves: terminal wave is a cross-lane integration pass" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-m3-intpass", "--json", "PD_INTPASS",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // proto blocks identity + web → two functional waves, so the integration
    // pass is wave 3.
    const t_proto = addTaskSlug(&suite, arena, pid, "ip-proto", "Proto");
    touchPath(&suite, repo, t_proto, "proto.zig");
    const t_ident = addTaskSlug(&suite, arena, pid, "ip-identity", "Identity");
    touchPath(&suite, repo, t_ident, "identity.zig");
    const t_web = addTaskSlug(&suite, arena, pid, "ip-web", "Web");
    touchPath(&suite, repo, t_web, "web.zig");
    linkBlocks(&suite, t_ident, t_proto);
    linkBlocks(&suite, t_web, t_proto);
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "waves", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(WavesResultM3, arena, out, .{ .ignore_unknown_fields = true });
    // Two functional waves + one integration pass = 3 total; the integration
    // pass is the LAST wave.
    try std.testing.expectEqual(@as(i64, 3), parsed.value.wave_count);
    try std.testing.expectEqual(@as(i64, 3), parsed.value.integration_pass_wave);

    // The two functional waves are NOT integration passes.
    const w1 = m3WaveByNumber(parsed.value, 1);
    const w2 = m3WaveByNumber(parsed.value, 2);
    try std.testing.expect(!w1.integration_pass);
    try std.testing.expect(!w2.integration_pass);
    try std.testing.expect(w1.lane_count > 0);

    // The terminal wave IS the integration pass, and carries no lanes (it is not
    // a fan-out wave; the model builds + tests the merged epic branch).
    const w3 = m3WaveByNumber(parsed.value, 3);
    try std.testing.expect(w3.integration_pass);
    try std.testing.expectEqual(@as(i64, 0), w3.lane_count);
    try std.testing.expectEqual(@as(usize, 0), m3WaveLanes(arena, w3.lanes).len);
}

// ---------------------------------------------------------------------------
// boundary-conflict-escalation (4629) — Error: the conflict_escalation phase
// formats the escalation payload (sorted conflicting paths + the two lane
// branches) and NEVER auto-resolves (auto_resolved=false, action=abort). The
// seam does no merge — that (and the abort) is the model's runtime op.
// ---------------------------------------------------------------------------

test "parallel-dispatch conflict_escalation: surfaces conflict without auto-resolving" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-m3-conflict");
    suite.addAssoc("pd-m3-conflict", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    // Conflicting paths deliberately OUT of sorted order to prove the seam sorts
    // for byte-stable output.
    const args_json =
        \\{"plan_id":55,"ours":"cycle/p55/lane-a","theirs":"cycle/p55/lane-b",
        \\"paths":["src/zeta.zig","src/alpha.zig","docs/shared.md"]}
    ;

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "conflict_escalation", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(ConflictResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(@as(i64, 55), parsed.value.plan_id);
    try std.testing.expect(parsed.value.conflict);
    // The load-bearing invariant: NEVER auto-resolved, action is abort.
    try std.testing.expect(!parsed.value.auto_resolved);
    try std.testing.expectEqualStrings("abort", parsed.value.action);

    // Both lane branches are named.
    try std.testing.expectEqualStrings("cycle/p55/lane-a", parsed.value.branches.ours);
    try std.testing.expectEqualStrings("cycle/p55/lane-b", parsed.value.branches.theirs);

    // Conflicting paths are surfaced, SORTED for byte-stability.
    try std.testing.expectEqual(@as(usize, 3), parsed.value.conflicting_paths.len);
    try std.testing.expectEqualStrings("docs/shared.md", parsed.value.conflicting_paths[0]);
    try std.testing.expectEqualStrings("src/alpha.zig", parsed.value.conflicting_paths[1]);
    try std.testing.expectEqualStrings("src/zeta.zig", parsed.value.conflicting_paths[2]);

    // Seam confinement tripwire: the escalation phase must NOT itself have run a
    // merge. The confined git host group exposes no merge verb; assert the seam
    // source never names one (belt-and-suspenders alongside the spawn-free test).
    const source = std.Io.Dir.cwd().readFileAlloc(std.testing.io, wf_path, gpa, .limited(256 * 1024)) catch
        @panic("failed to read parallel-dispatch.lua");
    defer gpa.free(source);
    try std.testing.expect(std.mem.indexOf(u8, source, "git.merge") == null);
}

// ---------------------------------------------------------------------------
// partial-wave-failure (4631) — Error: the reconcile_plan phase distinguishes a
// CLEAN lane failure (no reconcile) from a DEAD-CODER abandonment (reconcile
// --stale-after 0), and drops landed lanes.
// ---------------------------------------------------------------------------

test "parallel-dispatch reconcile_plan: clean-fail needs no reconcile, abandoned needs immediate reclaim" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-m3-reconcile");
    suite.addAssoc("pd-m3-reconcile", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    // Lane 4 landed; lane 2 failed cleanly (atomic fail); lane 6 abandoned (dead
    // coder, stranded claim).
    const args_json =
        \\{"plan_id":12,"lanes":[
        \\{"task_id":4,"outcome":"landed"},
        \\{"task_id":6,"outcome":"abandoned","worktree":".worktrees/cycle/p12/lane-dead"},
        \\{"task_id":2,"outcome":"failed_clean"}
        \\]}
    ;

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "reconcile_plan", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(ReconcileResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expectEqual(@as(i64, 12), parsed.value.plan_id);

    // The abandoned lane needs reconcile; the clean-fail lane does not.
    try std.testing.expect(parsed.value.needs_reconcile);
    const recon = reconcileLaneIds(arena, parsed.value.reconcile);
    try std.testing.expectEqual(@as(usize, 1), recon.len);
    try std.testing.expectEqual(@as(i64, 6), recon[0]);

    const avail = reconcileLaneIds(arena, parsed.value.available);
    try std.testing.expectEqual(@as(usize, 1), avail.len);
    try std.testing.expectEqual(@as(i64, 2), avail[0]);

    const landed = reconcileLaneIds(arena, parsed.value.landed);
    try std.testing.expectEqual(@as(usize, 1), landed.len);
    try std.testing.expectEqual(@as(i64, 4), landed[0]);

    // The exact reclaim argv the model runs is surfaced: `reconcile
    // --stale-after 0` (immediate, not waiting out the lease TTL).
    try std.testing.expect(std.mem.indexOf(u8, out, "\"--stale-after\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "dead_coder_stranded_claim") != null);
}

// ---------------------------------------------------------------------------
// partial-wave-failure (4631) — Edge: an all-clean-failure wave needs NO
// reconcile (every failed lane fired its atomic `planar-agent fail`).
// ---------------------------------------------------------------------------

test "parallel-dispatch reconcile_plan: all-clean-failure wave needs no reconcile" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    _ = suite.registerProject("pd-m3-clean");
    suite.addAssoc("pd-m3-clean", null);

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);

    const args_json =
        \\{"plan_id":13,"lanes":[
        \\{"task_id":1,"outcome":"failed_clean"},
        \\{"task_id":2,"outcome":"failed_clean"}
        \\]}
    ;

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "reconcile_plan", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(ReconcileResult, arena, out, .{ .ignore_unknown_fields = true });
    try std.testing.expect(!parsed.value.needs_reconcile);
    try std.testing.expectEqual(@as(usize, 0), reconcileLaneIds(arena, parsed.value.reconcile).len);
    try std.testing.expectEqual(@as(usize, 2), reconcileLaneIds(arena, parsed.value.available).len);
}

// ---------------------------------------------------------------------------
// resume-recompute (4632) — Error: resume recomputes waves from CURRENT state.
// After the contract lane (proto) lands (done), a resume `waves` run drops proto
// and re-fans only the remainder (identity + web), which are now eligible in
// wave 1. Resume does NOT restart from the original wave 1.
// ---------------------------------------------------------------------------

test "parallel-dispatch waves: resume recomputes and re-fans only the remainder after a lane lands" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-m3-resume", "--json", "PD_RESUME",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_proto = addTaskSlug(&suite, arena, pid, "rc-proto", "Proto");
    touchPath(&suite, repo, t_proto, "proto.zig");
    const t_ident = addTaskSlug(&suite, arena, pid, "rc-identity", "Identity");
    touchPath(&suite, repo, t_ident, "identity.zig");
    const t_web = addTaskSlug(&suite, arena, pid, "rc-web", "Web");
    touchPath(&suite, repo, t_web, "web.zig");
    linkBlocks(&suite, t_ident, t_proto);
    linkBlocks(&suite, t_web, t_proto);
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    // Simulate the contract lane landing: mark proto done. A resume recompute
    // must drop proto and re-fan only identity + web. The status matrix requires
    // todo → doing → done, so advance through `doing` first.
    const proto_id = std.fmt.allocPrint(arena, "{d}", .{t_proto}) catch unreachable;
    suite.allocator.free(suite.mustRun(&.{ "task", "update", proto_id, "--status", "doing" }));
    suite.allocator.free(suite.mustRun(&.{ "task", "done", proto_id }));

    const res = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "waves", "--args", args_json,
    });
    defer res.deinit();
    const out = std.mem.trim(u8, res.stdout, " \t\r\n");

    const parsed = try std.json.parseFromSlice(WavesResultM3, arena, out, .{ .ignore_unknown_fields = true });

    // proto (landed) is absent from EVERY functional wave.
    for (parsed.value.waves) |w| {
        for (m3WaveLanes(arena, w.lanes)) |l| {
            try std.testing.expect(l.task_id != t_proto);
        }
    }

    // identity + web are now the remainder — both eligible in wave 1 (their
    // blocker proto is done, so they are unblocked now), NOT restarted behind
    // proto. Wave 1 is a single functional wave of the two remainder lanes.
    const w1 = m3WaveByNumber(parsed.value, 1);
    try std.testing.expect(!w1.integration_pass);
    try std.testing.expectEqual(@as(i64, 2), w1.lane_count);
    var saw_ident = false;
    var saw_web = false;
    for (m3WaveLanes(arena, w1.lanes)) |l| {
        if (l.task_id == t_ident) saw_ident = true;
        if (l.task_id == t_web) saw_web = true;
        // The remainder lanes are unblocked now (blocked_by proto is gone).
        const blk = blockedByIds(arena, l.blocked_by);
        try std.testing.expectEqual(@as(usize, 0), blk.len);
    }
    try std.testing.expect(saw_ident and saw_web);
}

// ---------------------------------------------------------------------------
// resume-recompute (4632) — Empty-null: after EVERY lane lands (all done), a
// resume recompute yields an empty remainder. The `plan` phase reports
// fan_out=false (fewer than two eligible tasks — zero) and the `waves` phase
// reports zero waves (no candidates, so no integration pass either). The
// orchestrator reports the plan complete rather than re-fanning-out.
// ---------------------------------------------------------------------------

test "parallel-dispatch resume: all lanes landed yields an empty remainder" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pd-m3-alldone", "--json", "PD_ALLDONE",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_a = addTaskSlug(&suite, arena, pid, "ad-a", "A");
    touchPath(&suite, repo, t_a, "a.zig");
    const t_b = addTaskSlug(&suite, arena, pid, "ad-b", "B");
    touchPath(&suite, repo, t_b, "b.zig");
    suite.allocator.free(suite.mustRun(&.{ "plan", "update", pid, "--status", "active" }));

    // Land BOTH lanes. The status matrix requires todo → doing → done.
    const a_id = std.fmt.allocPrint(arena, "{d}", .{t_a}) catch unreachable;
    const b_id = std.fmt.allocPrint(arena, "{d}", .{t_b}) catch unreachable;
    suite.allocator.free(suite.mustRun(&.{ "task", "update", a_id, "--status", "doing" }));
    suite.allocator.free(suite.mustRun(&.{ "task", "done", a_id }));
    suite.allocator.free(suite.mustRun(&.{ "task", "update", b_id, "--status", "doing" }));
    suite.allocator.free(suite.mustRun(&.{ "task", "done", b_id }));

    const wf_path = try workflowPath(gpa);
    defer gpa.free(wf_path);
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{pid});
    defer gpa.free(args_json);

    // `plan` phase: no eligible tasks ⇒ fan_out=false (empty remainder).
    const pres = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "plan", "--args", args_json,
    });
    defer pres.deinit();
    const pout = std.mem.trim(u8, pres.stdout, " \t\r\n");
    const pparsed = try std.json.parseFromSlice(EmptyPlanResult, arena, pout, .{ .ignore_unknown_fields = true });
    try std.testing.expect(!pparsed.value.fan_out);
    try std.testing.expectEqual(@as(i64, 0), pparsed.value.lane_count);

    // `waves` phase: no candidates ⇒ zero waves, and NO integration pass (there
    // is nothing to integrate). integration_pass_wave stays 0. With zero waves
    // the empty `waves` table serializes as `{}` (the established empty-table
    // policy), which will not parse into a `[]WaveM3` slice — so assert on the
    // raw payload for wave_count/integration_pass_wave here.
    const wres = try mustExecute(gpa, suite.tmpAbsPath(), suite.absDbPath(), &.{
        "run", wf_path, "--phase", "waves", "--args", args_json,
    });
    defer wres.deinit();
    const wout = std.mem.trim(u8, wres.stdout, " \t\r\n");
    try std.testing.expect(std.mem.indexOf(u8, wout, "\"wave_count\":0") != null);
    try std.testing.expect(std.mem.indexOf(u8, wout, "\"integration_pass_wave\":0") != null);
    // An empty remainder means no functional waves AND no integration pass.
    try std.testing.expect(std.mem.indexOf(u8, wout, "\"integration_pass\":true") == null);
}
