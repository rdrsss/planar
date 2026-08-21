//! integration_tests/scenarios/scenario_worktree_resume_test.zig
//!
//! Plan 297 M6 scenario: `planar resume` surfaces the
//! `worktree_path` recorded on the active `agent_work_claims` row so a
//! cold-start resumer can prepend `cd <path>` before continuing the
//! prior session's work.
//!
//! Contract pinned here:
//!   - `planar resume <task> --json` includes an `active_claim` object
//!     when an exclusive claim is held on the task; the object carries
//!     `worktree_path` (plus claim_id, claim_token, vendor, branch,
//!     repo_root for context).
//!   - `planar resume <task>` text surfaces a `worktree:` and `cd:`
//!     line in the audit footer (section 8) when the claim row has a
//!     non-null worktree_path.
//!   - `planar resume <task> --json` omits / nulls `active_claim` when
//!     no active claim is held on the task.
//!   - Releasing the claim flips the field back to null on the next
//!     resume invocation.
//!
//! Handoff-side persistence (capture of worktree_path on the
//! handoff record itself) IS exercised in the
//! "resume falls back to handoff worktree_path" test below — that
//! path closes plan 297 followup t#2947 (handoffs.worktree_path
//! column landed in migration 00017), so a resumer can recover
//! `cd <path>` even after the originating claim has been released.

const std = @import("std");
const harness = @import("harness");

// ============================================================================
// Cross-binary helper: invoke planar-agent for claim acquisition.
// ============================================================================

fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic("PLANAR_AGENT_BIN not set; run via `make test-integration`");
}

fn runAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;
    const agent_bin = resolveAgentBin();
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, agent_bin) catch @panic("OOM");
    for (args) |a| argv_list.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-agent failed (term={any}): stdout={s} stderr={s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

// ============================================================================
// JSON shapes for partial decode (ignore_unknown_fields is on).
// ============================================================================

const PlanIdJSON = struct { id: i64 };
const TaskAddJSON = struct { id: i64 };

const PullClaimJSON = struct {
    claim_token: []const u8,
    worktree_path: ?[]const u8 = null,
};

const PullJSON = struct {
    ok: bool,
    no_work: bool = false,
    claim_token: ?[]const u8 = null,
    claim: ?PullClaimJSON = null,
};

const ActiveClaimJSON = struct {
    claim_id: i64,
    claim_token: []const u8,
    vendor: []const u8,
    worktree_path: []const u8 = "",
    repo_root: []const u8 = "",
    branch: []const u8 = "",
};

const FromHandoffJSON = struct {
    handoff_id: i64,
    worktree_path: []const u8 = "",
    repo_root: []const u8 = "",
    branch: []const u8 = "",
};

const ResumeJSON = struct {
    identity: struct { task_id: i64 },
    active_claim: ?ActiveClaimJSON = null,
    from_handoff: ?FromHandoffJSON = null,
};

const HandoffShowJSON = struct {
    id: i64,
    status: []const u8,
    worktree_path: []const u8 = "",
    repo_root: []const u8 = "",
    branch: []const u8 = "",
};

const HandoffCaptureJSON = struct {
    ok: bool,
    handoff_id: i64,
};

// ============================================================================
// Test 1 — claim with --worktree → resume surfaces the path.
// ============================================================================

test "scenario: resume surfaces worktree_path from active claim (json + text)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const plan = suite.mustRunJSON(PlanIdJSON, arena, &.{
        "plan", "create", "--slug", "m6-resume-wt", "--json", "M6 resume worktree",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task",   "add",           "--plan",                    pid,
        "--json", "--next-action", "verify worktree surfacing", "wt-target",
    });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    // Acquire a claim with --worktree pointing at a fake path. The
    // pull path stores `worktree_path` verbatim on agent_work_claims
    // when the value isn't a bare integer.
    const fake_wt = "/tmp/planar-m6-fake-worktree/feature-x";
    const pull_raw = mustRunAgent(&suite, &.{
        "pull", pid, "--no-locality-probe", "--worktree", fake_wt, "--json",
    });
    defer gpa.free(pull_raw);

    const pull_parsed = std.json.parseFromSlice(PullJSON, arena, pull_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\npull JSON decode failed: {s}\nraw: {s}\n", .{ @errorName(e), pull_raw });
        @panic("pull JSON");
    };
    try std.testing.expect(pull_parsed.value.ok);
    try std.testing.expect(!pull_parsed.value.no_work);
    const claim_token = pull_parsed.value.claim_token orelse @panic("pull lacked claim_token");

    // ---- Assertion 1a — resume --json surfaces active_claim.worktree_path.
    const resume_pkt = suite.mustRunJSON(ResumeJSON, arena, &.{
        "resume", "--json", tid,
    });

    const ac = resume_pkt.active_claim orelse {
        std.debug.print("\nresume --json had no active_claim despite live claim\n", .{});
        try std.testing.expect(false);
        unreachable;
    };
    try std.testing.expectEqualStrings(fake_wt, ac.worktree_path);
    try std.testing.expectEqualStrings(claim_token, ac.claim_token);
    try std.testing.expect(ac.claim_id > 0);

    // ---- Assertion 1b — resume text surfaces both `worktree:` and
    // `cd:` lines in the audit footer.
    const resume_text = suite.mustRun(&.{ "resume", tid });
    defer gpa.free(resume_text);

    try std.testing.expect(std.mem.indexOf(u8, resume_text, "worktree:") != null);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "cd:") != null);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, fake_wt) != null);
}

// ============================================================================
// Test 2 — no active claim → active_claim is null + text omits cd line.
// ============================================================================

test "scenario: resume omits active_claim when no claim is held on the task" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const plan = suite.mustRunJSON(PlanIdJSON, arena, &.{
        "plan", "create", "--slug", "m6-noclaim", "--json", "M6 no-claim",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task",   "add",           "--plan",        pid,
        "--json", "--next-action", "no claim path", "loner",
    });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    // No pull / no claim acquired.

    const resume_pkt = suite.mustRunJSON(ResumeJSON, arena, &.{
        "resume", "--json", tid,
    });
    try std.testing.expect(resume_pkt.active_claim == null);

    // Text rendering: no `cd:` line, no `worktree:` line under audit
    // footer when claim absent.
    const resume_text = suite.mustRun(&.{ "resume", tid });
    defer gpa.free(resume_text);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "\n  cd:") == null);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "\n  worktree:") == null);
}

// ============================================================================
// Test 3 — claim without --worktree → active_claim present but
// worktree_path is empty; text omits the cd/worktree lines.
// ============================================================================

test "scenario: resume active_claim present but worktree_path empty when claim had no --worktree" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const plan = suite.mustRunJSON(PlanIdJSON, arena, &.{
        "plan", "create", "--slug", "m6-bare", "--json", "M6 bare-claim",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task",   "add",           "--plan",     pid,
        "--json", "--next-action", "bare claim", "bare-target",
    });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    // Pull WITHOUT --worktree.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    const resume_pkt = suite.mustRunJSON(ResumeJSON, arena, &.{
        "resume", "--json", tid,
    });
    const ac = resume_pkt.active_claim orelse {
        std.debug.print("\nresume --json missing active_claim for held claim\n", .{});
        try std.testing.expect(false);
        unreachable;
    };
    try std.testing.expectEqualStrings("", ac.worktree_path);
    // claim still surfaces (claim_id, token, vendor populated).
    try std.testing.expect(ac.claim_id > 0);
    try std.testing.expect(ac.claim_token.len > 0);

    const resume_text = suite.mustRun(&.{ "resume", tid });
    defer gpa.free(resume_text);
    // The `active claim:` line IS emitted (presence of claim), but the
    // `worktree:` / `cd:` lines must NOT appear (no path to surface).
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "active claim:") != null);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "\n  cd:") == null);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "\n  worktree:") == null);
}

// ============================================================================
// Test 4 — handoff captures worktree_path, resume recovers it from the
// handoff record after the claim has been released. Plan 297 followup
// t#2947 (handoffs.worktree_path column landed in migration 00017).
// ============================================================================

test "scenario: resume falls back to handoff worktree_path after claim released" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const plan = suite.mustRunJSON(PlanIdJSON, arena, &.{
        "plan", "create", "--slug", "t2947-fallback", "--json", "T2947 handoff fallback",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const task = suite.mustRunJSON(TaskAddJSON, arena, &.{
        "task",   "add",           "--plan",                    pid,
        "--json", "--next-action", "verify handoff carries wt", "wt-handoff-target",
    });
    const tid = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM");

    // Acquire a claim with --worktree pointing at a fake path.
    const fake_wt = "/tmp/planar-t2947-fake-worktree/cycle/feat";
    const pull_raw = mustRunAgent(&suite, &.{
        "pull", pid, "--no-locality-probe", "--worktree", fake_wt, "--json",
    });
    defer gpa.free(pull_raw);

    const pull_parsed = std.json.parseFromSlice(PullJSON, arena, pull_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\npull JSON decode failed: {s}\nraw: {s}\n", .{ @errorName(e), pull_raw });
        @panic("pull JSON");
    };
    const claim_token = pull_parsed.value.claim_token orelse @panic("pull lacked claim_token");

    // Open a capture session bound to the task — required for the
    // `planar handoff <task>` combined ritual.
    gpa.free(suite.mustRunWith(&.{ "capture", "session", "--task", tid }, &.{
        .{ .key = "PLANAR_VENDOR", .value = "claude" },
    }));

    // Run the combined handoff ritual. The handler must copy
    // worktree_path from the active claim onto the new handoff row.
    const handoff_json_raw = suite.mustRunWith(&.{ "handoff", tid, "--json", "--note", "captured" }, &.{
        .{ .key = "PLANAR_VENDOR", .value = "claude" },
    });
    defer gpa.free(handoff_json_raw);
    const hc = std.json.parseFromSlice(HandoffCaptureJSON, arena, handoff_json_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nhandoff JSON decode failed: {s}\nraw: {s}\n", .{ @errorName(e), handoff_json_raw });
        @panic("handoff JSON");
    };
    const hid = std.fmt.allocPrint(arena, "{d}", .{hc.value.handoff_id}) catch @panic("OOM");

    // ---- Assertion 4a — handoff show --json carries worktree_path.
    const show = suite.mustRunJSON(HandoffShowJSON, arena, &.{
        "handoff", "show", hid, "--json",
    });
    try std.testing.expectEqualStrings(fake_wt, show.worktree_path);

    // ---- Release the claim. Active claim row is now gone.
    gpa.free(mustRunAgent(&suite, &.{ "release", "--claim", claim_token }));

    // ---- Assertion 4b — resume --json now has no active_claim,
    // but from_handoff surfaces the worktree_path recovered from
    // the handoff record.
    const resume_pkt = suite.mustRunJSON(ResumeJSON, arena, &.{
        "resume", "--json", tid,
    });
    try std.testing.expect(resume_pkt.active_claim == null);
    const fh = resume_pkt.from_handoff orelse {
        std.debug.print("\nresume --json missing from_handoff fallback after claim release\n", .{});
        try std.testing.expect(false);
        unreachable;
    };
    try std.testing.expectEqualStrings(fake_wt, fh.worktree_path);
    try std.testing.expectEqual(hc.value.handoff_id, fh.handoff_id);

    // ---- Assertion 4c — resume text surfaces the `from handoff:`
    // label and the `cd:` / `worktree:` lines under the audit footer.
    const resume_text = suite.mustRun(&.{ "resume", tid });
    defer gpa.free(resume_text);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "from handoff:") != null);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "worktree:") != null);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, "cd:") != null);
    try std.testing.expect(std.mem.indexOf(u8, resume_text, fake_wt) != null);
}
