//! integration_tests/tree_audit_activity_test.zig
//!
//! Black-box coverage for the M5 fold-ins (plan 85):
//!
//!   - `planar tree --json` includes `activity_summary` on entity rows
//!     once an `agent_actions` / `agent_work_claims` row exists for the
//!     entity (task:tree-activity-summary).
//!   - `planar tree` and `planar tree --json` produce CLEAN output (no
//!     empty activity placeholders) when no activity has been recorded
//!     (task:activity-summary-empty-degrade).
//!   - `planar audit trail <kind:id>` appends an "Agent activity" text
//!     section + an `agent_activity` JSON sub-object when activity exists
//!     (task:audit-trail-agent-activity).
//!   - `planar audit trail <kind:id>` does NOT print "Agent activity:"
//!     when nothing is recorded (task:activity-summary-empty-degrade).
//!
//! And methodology / vendor-surface namespace purity. The 5 role agents
//! (orchestrator, coder, reviewer, test-coder, janitor), their companion
//! docs (methodology, doctrine, models, cross-scope-writes), and the
//! `pl-` role skills were raised to armarium at plan 929 M3 and returned
//! at the armarium reintegration, so the checks below cover both the
//! always-retained planar surfaces and the returned orchestration
//! surfaces; the two checks retired during the raise are restored.
//!
//! Planar plan 933 task 5456 (doc-cluster transfer): the doc-system cluster
//! — `agents/documenter.md`, `agents/doc-author.md`, `skills/src/pl-documenter.md`,
//! `skills/src/pl-doc-maintain.md` — was likewise raised, to tabularium
//! (commit 967ed7f), which owns the doc-system tool they drive. 8cf4705 had
//! seeded several of the retained-surface samples below with `documenter` /
//! `doc-author` / `pl-documenter`; those entries are re-pointed here at other
//! retained planar surfaces (the audited namespace/ritual invariants apply
//! generally, so the sample choice is not load-bearing).
//!
//!   - Planar's retained surfaces (`docs/cli-reference.md`, the
//!     retained `agents/*.md` role docs that discuss the claim ritual)
//!     reference `planar-agent pull` / `complete` / `fail` / `release` /
//!     `block` and do NOT reference any `planar agent <verb>` subcommand
//!     (task:methodology-claim-ritual, task:vendor-surfaces-claim-aware).
//!   - The retained surfaces do NOT reference `planar-agent ps` / `log` /
//!     `tail` (those live on `planar-watch`).

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// Helpers
// =========================================================================

const PlanJSON = struct { id: i64 };
const TaskJSON = struct { id: i64 };

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

fn runAgent(suite: *const harness.Suite, args: []const []const u8) harness.Suite.RunResult {
    const gpa = suite.allocator;
    const bin = resolveAgentBin();
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, bin) catch @panic("OOM");
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

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-agent failed (term={any}): {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

fn mustRunAgentInDir(
    suite: *harness.Suite,
    cwd: []const u8,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const agent_bin = resolveAgentBin();

    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, agent_bin) catch @panic("OOM");
    for (args) |arg| argv_list.append(gpa, arg) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.absDbPath()) catch @panic("OOM PLANAR_DB");
    env_map.put("PWD", cwd) catch @panic("OOM PWD");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .cwd = .{ .path = cwd },
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("mustRunAgentInDir spawn failed: {s}", .{@errorName(e)});
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print(
            "planar-agent failed in '{s}' (term={any}): {s}\nstderr: {s}\n",
            .{ cwd, result.term, result.stdout, result.stderr },
        );
        @panic("planar-agent in-dir must-run failed");
    }
    return result.stdout;
}

fn makeFixtureRepo(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    name: []const u8,
) ![]u8 {
    const repo_root = try std.fs.path.join(gpa, &.{ suite.tmpAbsPath(), name });
    errdefer gpa.free(repo_root);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, repo_root);

    try runCommandDiscard(&.{ "git", "init", repo_root });
    try runCommandInDirDiscard(repo_root, &.{ "git", "config", "user.email", "planar-test@example.com" });
    try runCommandInDirDiscard(repo_root, &.{ "git", "config", "user.name", "Planar Test" });

    try writeRepoFile(repo_root, "README.md", "seed\n");
    try runCommandInDirDiscard(repo_root, &.{ "git", "add", "README.md" });
    try runCommandInDirDiscard(repo_root, &.{ "git", "commit", "-m", "seed" });
    return repo_root;
}

fn runCommand(argv: []const []const u8) ![]u8 {
    const gpa = std.testing.allocator;
    const result = try std.process.run(gpa, std.testing.io, .{ .argv = argv });
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("command failed: {s}\nstderr: {s}\n", .{ argv[0], result.stderr });
        gpa.free(result.stdout);
        return error.CommandFailed;
    }
    return result.stdout;
}

fn runCommandDiscard(argv: []const []const u8) !void {
    const stdout = try runCommand(argv);
    std.testing.allocator.free(stdout);
}

fn runCommandInDir(cwd: []const u8, argv: []const []const u8) ![]u8 {
    const gpa = std.testing.allocator;
    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv,
        .cwd = .{ .path = cwd },
    });
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("command failed in '{s}': {s}\nstderr: {s}\n", .{ cwd, argv[0], result.stderr });
        gpa.free(result.stdout);
        return error.CommandFailed;
    }
    return result.stdout;
}

fn runCommandInDirDiscard(cwd: []const u8, argv: []const []const u8) !void {
    const stdout = try runCommandInDir(cwd, argv);
    std.testing.allocator.free(stdout);
}

fn writeRepoFile(repo_root: []const u8, rel_path: []const u8, contents: []const u8) !void {
    const gpa = std.testing.allocator;
    const path = try std.fs.path.join(gpa, &.{ repo_root, rel_path });
    defer gpa.free(path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = contents });
}

fn createCommit(
    repo_root: []const u8,
    rel_path: []const u8,
    contents: []const u8,
    subject: []const u8,
) ![]u8 {
    try writeRepoFile(repo_root, rel_path, contents);
    try runCommandInDirDiscard(repo_root, &.{ "git", "add", rel_path });
    try runCommandInDirDiscard(repo_root, &.{ "git", "commit", "-m", subject });
    const raw = try runCommandInDir(repo_root, &.{ "git", "rev-parse", "HEAD" });
    defer std.testing.allocator.free(raw);
    return try std.testing.allocator.dupe(u8, std.mem.trim(u8, raw, " \t\r\n"));
}

fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

fn extractStringField(json: []const u8, key: []const u8) ?[]const u8 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    if (i >= json.len or json[i] != '"') return null;
    i += 1;
    const start = i;
    while (i < json.len and json[i] != '"') i += 1;
    if (i >= json.len) return null;
    return json[start..i];
}

// =========================================================================
// tree — empty degrade
// =========================================================================

test "tree --json on a plan with NO agent activity: no activity_summary keys" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tree-no-act", "--json", "TREE_NO_ACT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "no-act-task",
    });

    const raw = suite.mustRun(&.{ "tree", "--depth", "3", "--json", "--scope", "global" });
    defer gpa.free(raw);
    // No activity has been recorded — the `activity_summary` JSON key
    // MUST NOT appear (silent degrade). This pins the
    // task:activity-summary-empty-degrade contract for tree.
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"activity_summary\"") == null);
}

test "tree text on a plan with NO agent activity: no 'activity:' sub-line" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tree-no-act-text", "--json", "TREE_NO_ACT_TEXT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "no-act-task-text",
    });

    const raw = suite.mustRun(&.{ "tree", "--depth", "3", "--scope", "global" });
    defer gpa.free(raw);
    // No "activity:" sub-line: the text renderer degrades silently when
    // the entity has no agent_actions / agent_work_claims rows.
    try std.testing.expect(std.mem.indexOf(u8, raw, "activity:") == null);
}

// =========================================================================
// tree — happy path: activity_summary present on claimed task
// =========================================================================

test "tree --json on a plan with active claim: activity_summary present on task row" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tree-active", "--json", "TREE_ACT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "claimable-task",
    });

    // Seed an active claim via planar-agent pull. pull writes both an
    // agent_actions row AND an agent_work_claims row.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--role", "coder", "--json" }));

    const raw = suite.mustRun(&.{ "tree", "--depth", "3", "--json", "--scope", "global" });
    defer gpa.free(raw);
    // The key MUST appear at least once now (on the claimed task row).
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"activity_summary\"") != null);
    // Latest action_kind is the role `pull` recorded — `coder`.
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"latest_action_kind\":\"coder\"") != null);
    // Active claim count for the task is 1.
    try std.testing.expect(std.mem.indexOf(u8, raw, "\"active_claim_count\":1") != null);
}

test "tree text on a plan with active claim: 'activity:' sub-line present" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "tree-active-text", "--json", "TREE_ACT_TEXT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    _ = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "claimable-task-text",
    });
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--role", "coder", "--json" }));

    const raw = suite.mustRun(&.{ "tree", "--depth", "3", "--scope", "global" });
    defer gpa.free(raw);
    try std.testing.expect(std.mem.indexOf(u8, raw, "activity: coder") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "[claims:1]") != null);
}

// =========================================================================
// audit trail — empty degrade
// =========================================================================

test "audit trail on a task with NO agent activity: no 'Agent activity:' section" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "at-no-act", "--json", "AT_NO_ACT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "no-act-trail-task",
    });
    const tid_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    const raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", tid_str });
    defer gpa.free(raw);
    // Section header MUST NOT appear when there's no agent activity.
    try std.testing.expect(std.mem.indexOf(u8, raw, "Agent activity:") == null);

    const json_raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", "--json", tid_str });
    defer gpa.free(json_raw);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"agent_activity\"") == null);
}

// =========================================================================
// audit trail — happy path
// =========================================================================

test "audit trail on a task with agent activity: 'Agent activity:' section + agent_activity JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "at-with-act", "--json", "AT_WITH_ACT",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "trail-with-act-task",
    });
    const tid_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    // pull → claim + action → still active.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--role", "coder", "--json" }));

    const raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", tid_str });
    defer gpa.free(raw);
    try std.testing.expect(std.mem.indexOf(u8, raw, "Agent activity:") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "actions (") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "claims (") != null);

    const json_raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", "--json", tid_str });
    defer gpa.free(json_raw);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"agent_activity\":{\"actions\":[") != null);
    // The vendor recorded by pull (no --vendor-session) is the default
    // `planar` adapter; assert presence rather than exact value to
    // stay resilient to the default-vendor convention.
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"claims\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"action_kind\":\"coder\"") != null);
}

test "audit trail on a task with session commits: emits commits section and commits JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("trail-task-commits");
    const repo_root = try makeFixtureRepo(gpa, &suite, "trail-task-commits-repo");
    defer gpa.free(repo_root);

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "trail-task-commits", "--json", "TRAIL_TASK_COMMITS",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "trail task commits",
    });
    const tid_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

    const claim_json = mustRunAgentInDir(&suite, repo_root, &.{ "pull", pid, "--role", "coder", "--json" });
    defer gpa.free(claim_json);
    const claim_token = try gpa.dupe(u8, extractStringField(claim_json, "\"claim_token\"") orelse @panic("no claim token"));
    defer gpa.free(claim_token);

    const sha = try createCommit(repo_root, "trail-task.txt", "trail task\n", "trail task commit");
    defer gpa.free(sha);
    gpa.free(mustRunAgentInDir(&suite, repo_root, &.{ "complete", "--claim", claim_token, "--summary", "done", "--json" }));

    const raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", tid_str });
    defer gpa.free(raw);
    try std.testing.expect(std.mem.indexOf(u8, raw, "\ncommits:\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, sha) != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "trail task commit") != null);

    const json_raw = suite.mustRun(&.{ "audit", "trail", "--kind", "task", "--json", tid_str });
    defer gpa.free(json_raw);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"commits\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, sha) != null);
}

test "audit trail link form folds in commits and omits the commits key when empty" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("trail-link-commits");
    gpa.free(suite.mustRunExt(&.{ "ext", "register", "github", "gh", "--project", "owner/repo" }));

    const repo_root = try makeFixtureRepo(gpa, &suite, "trail-link-commits-repo");
    defer gpa.free(repo_root);

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "trail-link-commits", "--json", "TRAIL_LINK_COMMITS",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const task_with_commits = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "trail link commits task",
    });
    const task_without_commits = suite.mustRunJSON(TaskJSON, arena, &.{
        "task", "add", "--plan", pid, "--json", "trail link empty task",
    });

    const task_ref = std.fmt.allocPrint(arena, "task:{d}", .{task_with_commits.id}) catch unreachable;
    const task_no_commits_ref = std.fmt.allocPrint(arena, "task:{d}", .{task_without_commits.id}) catch unreachable;

    const claim_json = mustRunAgentInDir(&suite, repo_root, &.{ "pull", pid, "--role", "coder", "--json" });
    defer gpa.free(claim_json);
    const claim_token = try gpa.dupe(u8, extractStringField(claim_json, "\"claim_token\"") orelse @panic("no claim token"));
    defer gpa.free(claim_token);

    const sha = try createCommit(repo_root, "trail-link.txt", "trail link\n", "trail link commit");
    defer gpa.free(sha);
    gpa.free(mustRunAgentInDir(&suite, repo_root, &.{ "complete", "--claim", claim_token, "--summary", "done", "--json" }));

    const linked = suite.mustRun(&.{ "link", task_ref, "--to", "gh:ISSUE-7", "--json" });
    defer gpa.free(linked);
    const link_id = extractIntField(linked, "\"link_id\"") orelse @panic("no link id");
    const link_id_arg = std.fmt.allocPrint(arena, "{d}", .{link_id}) catch unreachable;

    const linked_empty = suite.mustRun(&.{ "link", task_no_commits_ref, "--to", "gh:ISSUE-8", "--json" });
    defer gpa.free(linked_empty);
    const empty_link_id = extractIntField(linked_empty, "\"link_id\"") orelse @panic("no empty link id");
    const empty_link_id_arg = std.fmt.allocPrint(arena, "{d}", .{empty_link_id}) catch unreachable;

    const raw = suite.mustRun(&.{ "audit", "trail", "--link", link_id_arg });
    defer gpa.free(raw);
    try std.testing.expect(std.mem.indexOf(u8, raw, "\ncommits:\n") != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, sha) != null);
    try std.testing.expect(std.mem.indexOf(u8, raw, "trail link commit") != null);

    const json_raw = suite.mustRun(&.{ "audit", "trail", "--link", link_id_arg, "--json" });
    defer gpa.free(json_raw);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, "\"commits\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_raw, sha) != null);

    const empty_raw = suite.mustRun(&.{ "audit", "trail", "--link", empty_link_id_arg });
    defer gpa.free(empty_raw);
    try std.testing.expect(std.mem.indexOf(u8, empty_raw, "\ncommits:\n") == null);

    const empty_json = suite.mustRun(&.{ "audit", "trail", "--link", empty_link_id_arg, "--json" });
    defer gpa.free(empty_json);
    try std.testing.expect(std.mem.indexOf(u8, empty_json, "\"commits\"") == null);
}

// =========================================================================
// Namespace purity — methodology + vendor surfaces
// =========================================================================

/// Resolve a path relative to the repository root by walking upward
/// from `PLANAR_BIN`'s parent until a `.git` entry is found. `.git` is
/// planar's own unambiguous repo-root marker: a directory for a plain
/// checkout, or a FILE (containing `gitdir: ...`) for a submodule
/// checkout such as this one under armarium's `stack/planar/`. The
/// existence probe (`std.Io.Dir.accessAbsolute`) is deliberately
/// type-agnostic so it matches both shapes.
///
/// This walk MUST stop at the first `.git` entry and must NOT continue
/// probing upward into a superproject: armarium (the superproject that
/// pins this repo as a submodule) carries its own top-level `agents/`
/// tree post-raise, so a walk that kept climbing past planar's boundary
/// could silently resolve into armarium instead of failing when a
/// requested `rel` no longer exists in planar. Once the boundary is
/// found, the caller's subsequent file open surfaces `FileNotFound`
/// directly for a `rel` that doesn't exist here — a clean failure
/// instead of a wrong-repo resolution.
///
/// `make test-integration` exports `PLANAR_BIN=<repo>/bin/planar` so the
/// repo root is normally two levels above the binary file; the search
/// loop is defensive in case the binary moves. Returns an absolute path
/// the caller owns.
fn resolveRepoPath(gpa: std.mem.Allocator, rel: []const u8) ![]u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var bin_path: ?[]const u8 = null;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_BIN=")) {
            bin_path = s["PLANAR_BIN=".len..];
            break;
        }
    }
    if (bin_path == null) return error.NoPlanarBin;

    var cwd: []const u8 = bin_path.?;
    var depth: usize = 0;
    while (depth < 12) : (depth += 1) {
        cwd = std.fs.path.dirname(cwd) orelse return error.RepoRootNotFound;
        const git_marker = try std.fs.path.join(gpa, &.{ cwd, ".git" });
        defer gpa.free(git_marker);
        std.Io.Dir.accessAbsolute(std.testing.io, git_marker, .{}) catch continue;
        // Found the repo boundary. Stop unconditionally — do not keep
        // climbing even if `rel` turns out not to exist at this level.
        return try std.fs.path.join(gpa, &.{ cwd, rel });
    }
    return error.RepoRootNotFound;
}

fn readRepoFile(gpa: std.mem.Allocator, rel: []const u8) ![]u8 {
    const abs = try resolveRepoPath(gpa, rel);
    defer gpa.free(abs);
    var file = try std.Io.Dir.openFileAbsolute(std.testing.io, abs, .{});
    defer file.close(std.testing.io);
    var reader = file.reader(std.testing.io, &.{});
    return try reader.interface.allocRemaining(gpa, std.Io.Limit.limited(1 * 1024 * 1024));
}

// Planar's claim-ritual contract — "every code-writing agent dispatch
// follows planar-agent pull -> heartbeat -> exactly one terminal verb" —
// is pinned on `docs/cli-reference.md` (the authoritative CLI surface,
// which documents the full verb set including `reconcile` / `abort`)
// plus a sample of planar agent docs that reference `planar-agent` as
// consumers of the ritual. The returned `agents/methodology.md` carries
// its own restored check below.
test "CLI reference documents `planar-agent pull` and the canonical terminal verbs" {
    const gpa = std.testing.allocator;
    const files = [_][]const u8{
        "docs/cli-reference.md",
        // `agents/documenter.md` raised to tabularium (plan 933 task 5456);
        // re-pointed to `agents/sync-reconciler.md`, another retained agent
        // dispatched through `planar-agent`.
        "agents/sync-reconciler.md",
        "agents/ingestor.md",
        "agents/planner.md",
    };
    // The CLI reference MUST mention `planar-agent pull` and each of
    // complete / fail / release / block / reconcile / abort. The
    // retained agent docs MUST reference `planar-agent` at least once
    // each (they are dispatched through it, even though they aren't the
    // orchestrator itself).
    {
        const body = try readRepoFile(gpa, files[0]);
        defer gpa.free(body);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent pull") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent heartbeat") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent complete") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent fail") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent release") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent block") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent reconcile") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent abort") != null);
    }
    for (files[1..]) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent") != null);
    }
}

// Restored at the armarium reintegration: `agents/methodology.md` is back
// in-tree, so its claim-ritual and durable-boundary contracts are pinned
// directly again.
test "methodology docs reference `planar-agent pull` and the canonical terminal verbs" {
    const gpa = std.testing.allocator;
    const files = [_][]const u8{
        "agents/methodology.md",
        "agents/orchestrator.md",
        "agents/coder.md",
        "agents/reviewer.md",
    };
    // The methodology file MUST mention `planar-agent pull` and each
    // of complete / fail / release / block. The supporting role docs
    // MUST reference `planar-agent` at least once each.
    {
        const body = try readRepoFile(gpa, files[0]);
        defer gpa.free(body);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent pull") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent heartbeat") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent complete") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent fail") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent release") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent block") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent reconcile") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent abort") != null);
    }
    for (files[1..]) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent") != null);
    }
}

test "methodology defines the durable boundary and surviving target contract" {
    const gpa = std.testing.allocator;
    const body = try readRepoFile(gpa, "agents/methodology.md");
    defer gpa.free(body);

    for ([_][]const u8{
        "## Durable orchestration boundary contract",
        "authoritative status is `todo`, `doing`, or",
        "`blocked` is a surviving target",
        "status is `done` or `cancelled`",
        "planar task update <task-id>",
        "orchestration_checkpoint: v1",
        "iteration_scope: <coder-review|test-coder|none>",
        "`resumable:true` makes this target's boundary durable",
        "returns `outcome=partial`",
        "exact recovery command: `planar resume <task-id> --json`",
    }) |needle| {
        if (std.mem.indexOf(u8, body, needle) == null) {
            std.debug.print("durable-boundary methodology contract missing: {s}\n", .{needle});
            return error.TestUnexpectedResult;
        }
    }
}

test "methodology + role docs contain ZERO `planar agent <verb>` references" {
    const gpa = std.testing.allocator;
    // The namespace-purity invariant (never the space-separated
    // `planar agent <verb>` form) applies to every authored surface that
    // discusses `planar-agent` verbs: the always-retained planar surfaces
    // plus the orchestration role docs and pl- role skills returned at the
    // armarium reintegration. The doc cluster (`documenter` / `doc-author` /
    // `pl-documenter`) lives in tabularium and is covered there.
    const files = [_][]const u8{
        "agents/methodology.md",
        "agents/doctrine.md",
        "agents/models.md",
        "agents/cross-scope-writes.md",
        "agents/orchestrator.md",
        "agents/coder.md",
        "agents/reviewer.md",
        "agents/test-coder.md",
        "agents/research.md",
        "agents/janitor.md",
        "skills/src/pl-orchestrator.md",
        "skills/src/pl-coder.md",
        "skills/src/pl-reviewer.md",
        "skills/src/pl-test-coder.md",
        "skills/src/pl-research.md",
        "agents/ext-sync.md",
        "agents/feedback-triager.md",
        "agents/ingestor.md",
        "agents/introspector.md",
        "agents/planner.md",
        "agents/spec-reviewer.md",
        "agents/sync-reconciler.md",
        "skills/src/pl-doctor.md",
        "skills/src/pl-knowledge.md",
        "skills/src/pl-health.md",
        "skills/src/pl-observe.md",
        "skills/src/pl-plan.md",
        "skills/src/pl-resume.md",
        "skills/src/pl-spec-draft.md",
        "skills/src/pl-spec-ingest.md",
        "skills/src/pl-status.md",
        "skills/src/pl-task.md",
    };
    // Forbidden literal verb references. Each scan looks for the
    // exact substring; phrases like "no `planar agent` subcommand"
    // intentionally do NOT match these (the verb name follows a
    // space-then-letter in the forbidden patterns).
    const forbidden = [_][]const u8{
        "planar agent claim",
        "planar agent ps",
        "planar agent log",
        "planar agent tail",
        "planar agent reconcile",
        "planar agent abort",
        "planar agent start",
        "planar agent end",
        "planar agent pull",
        "planar agent peek",
        "planar agent complete",
        "planar agent fail",
        "planar agent release",
        "planar agent block",
        "planar agent heartbeat",
    };
    for (files) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        for (forbidden) |needle| {
            if (std.mem.indexOf(u8, body, needle)) |hit| {
                std.debug.print(
                    "namespace-purity violation in {s}: found '{s}' at offset {d}\n",
                    .{ rel, needle, hit },
                );
                return error.NamespaceImpurity;
            }
        }
    }
}

// Restored at the armarium reintegration: the orchestrator surfaces are
// back in-tree.
test "orchestrator surfaces reference `planar-agent peek` for parallelism-aware dispatch" {
    const gpa = std.testing.allocator;
    const files = [_][]const u8{
        "agents/orchestrator.md",
        "skills/src/pl-orchestrator.md",
    };
    for (files) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        try std.testing.expect(std.mem.indexOf(u8, body, "planar-agent peek") != null);
        try std.testing.expect(std.mem.indexOf(u8, body, "parallel") != null);
    }
}

test "retained agent/skill surfaces do NOT reference `planar-agent ps` / log / tail (those live on planar-watch)" {
    const gpa = std.testing.allocator;
    // The binary-boundary purity invariant applies to any authored doc
    // that discusses agent verbs: the always-retained sample plus the
    // orchestration surfaces returned at the armarium reintegration.
    const files = [_][]const u8{
        "agents/methodology.md",
        "agents/orchestrator.md",
        "agents/coder.md",
        "agents/reviewer.md",
        "skills/src/pl-orchestrator.md",
        "skills/src/pl-coder.md",
        "skills/src/pl-reviewer.md",
        "agents/sync-reconciler.md",
        "agents/ingestor.md",
        "agents/planner.md",
        "skills/src/pl-status.md",
        "skills/src/pl-plan.md",
        "skills/src/pl-task.md",
    };
    const forbidden = [_][]const u8{
        "planar-agent ps",
        "planar-agent log",
        "planar-agent tail",
    };
    for (files) |rel| {
        const body = try readRepoFile(gpa, rel);
        defer gpa.free(body);
        for (forbidden) |needle| {
            if (std.mem.indexOf(u8, body, needle)) |hit| {
                std.debug.print(
                    "binary-boundary violation in {s}: found '{s}' at offset {d}\n",
                    .{ rel, needle, hit },
                );
                return error.BinaryBoundaryViolation;
            }
        }
    }
}
