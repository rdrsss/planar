//! REAL end-to-end live-spawn test for `planar-execute`'s gated `agent()`
//! production driver (plan 492 M4 task 3241; builds on tasks 3175/3176/3178/3180).
//!
//! ## What this test does
//!
//! It drives the SHIPPED `planar-execute run` path with the production agent()
//! driver attached, spawning a REAL `claude --print ...` worker, and asserts the
//! worker's commit landed on the cycle branch and the harness drove the claim to
//! a terminal state. This is the one test that proves the FakeSpawner contract
//! (exercised by the always-on unit tests in main.zig) matches what `claude`
//! actually does on a real machine, and that `handleRun` wires the driver
//! correctly end-to-end.
//!
//! ## Gate: PLANAR_EXECUTE_LIVE_AGENT=1 (one gate, two effects)
//!
//! The same env var that this test keys on ALSO controls whether the production
//! `planar-execute run` binary attaches a real spawn driver (see handleRun in
//! src/cmd/planar-execute/main.zig). When the var is ABSENT — the default, every
//! `make test-integration` run, all of CI — this test returns `error.SkipZigTest`
//! immediately and the binary's `agent()` keeps the M2 stub behavior. CI never
//! burns claude credit. When the operator opts in:
//!
//!     PLANAR_EXECUTE_LIVE_AGENT=1 make test-integration
//!
//! the test runs for real. One spawn per run; cost is ~$0.10–$0.50.
//!
//! ## Flow
//!
//!   1. Seed a fixture DB via the real `planar` CLI (harness Suite): register a
//!      project + association, create a plan (capture id + slug), create one task
//!      with slug `m4-smoke` whose body instructs the worker to write + commit a
//!      file and exit.
//!   2. Prepare the front-half by hand (the M5-deferred lifecycle the scheduler
//!      will own): create a throwaway git repo, create the cycle branch
//!      `cycle/<plan-slug>/m4-smoke` and add a worktree for it; acquire a claim
//!      on the task via `planar-agent claim` and capture its claim_token.
//!   3. Write a smoke.lua workflow that forwards the prepared worktree_path +
//!      task_slug + claim_token from ctx.args into `ctx.agent(brief, opts)`.
//!   4. Run `PLANAR_EXECUTE_LIVE_AGENT=1 planar-execute run --plan <id> smoke.lua
//!      <worktree> m4-smoke <token>` with PLANAR_DB = fixture and PATH prepended
//!      with the freshly-built bin dir, cwd = the throwaway git repo.
//!   5. Assert: planar-execute exited 0; the cycle branch HEAD advanced (the
//!      worker's commit landed); `m4-smoke.txt` exists + is committed in the
//!      worktree; the claim is no longer active in `planar-watch ps`.
//!
//! ## Scope fence (M4)
//!
//! `agent()` does NOT create the cycle worktree or acquire the claim — this test
//! does that front-half (step 2) and passes the prepared handles in via opts.
//! The merge/teardown back-half is M5. The driver only runs the existing
//! `driveAgentCall` pipeline (spawn → sample HEAD → read claim → terminal verb).

const std = @import("std");
const harness = @import("harness");

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

fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: zig build test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

fn resolveExecuteBin() []const u8 {
    return envValue("PLANAR_EXECUTE_BIN") orelse
        @panic("PLANAR_EXECUTE_BIN is not set. Run via: zig build test-integration");
}

/// run a child process to completion, capturing stdout/stderr. Panics on spawn
/// failure (a broken test environment), returns the result otherwise.
const Cmd = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,

    fn deinit(self: Cmd) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }
    fn ok(self: Cmd) bool {
        return self.term == .exited and self.term.exited == 0;
    }
    fn code(self: Cmd) u32 {
        return switch (self.term) {
            .exited => |c| c,
            else => 255,
        };
    }
};

const EnvKV = struct { key: []const u8, value: []const u8 };

/// Run `argv` with cwd `cwd` and the host env plus `extra_env` overrides.
fn runCmd(
    gpa: std.mem.Allocator,
    cwd: ?[]const u8,
    argv: []const []const u8,
    extra_env: []const EnvKV,
) Cmd {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM env map");
    defer env_map.deinit();
    for (extra_env) |e| env_map.put(e.key, e.value) catch @panic("OOM put env");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv,
        .environ_map = &env_map,
        .cwd = if (cwd) |c| .{ .path = c } else .inherit,
        .stdout_limit = std.Io.Limit.limited(4 * 1024 * 1024),
        .stderr_limit = std.Io.Limit.limited(1 * 1024 * 1024),
    }) catch |e| std.debug.panic("runCmd spawn failed for '{s}': {s}", .{ argv[0], @errorName(e) });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr, .gpa = gpa };
}

/// Run a git command in `repo`, panicking if it fails (setup must succeed).
fn git(gpa: std.mem.Allocator, repo: []const u8, args: []const []const u8) void {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    argv.append(gpa, "git") catch @panic("OOM");
    argv.append(gpa, "-C") catch @panic("OOM");
    argv.append(gpa, repo) catch @panic("OOM");
    for (args) |a| argv.append(gpa, a) catch @panic("OOM");
    const r = runCmd(gpa, null, argv.items, &.{});
    defer r.deinit();
    if (!r.ok()) std.debug.panic("git {s} failed: {s}", .{ args[0], r.stderr });
}

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    slug: ?[]const u8 = null,
};
const TaskJSON = struct {
    id: i64,
    slug: ?[]const u8 = null,
};
const ClaimJSON = struct {
    ok: bool,
    claim_token: []const u8,
};

test "planar-execute agent() LIVE spawn — real claude worker commits on the cycle branch (gated)" {
    // CI / make test-integration: skip silently. PLANAR_EXECUTE_LIVE_AGENT is
    // the opt-in gate; without it, return SkipZigTest immediately. CI never
    // burns claude credit.
    if (envValue("PLANAR_EXECUTE_LIVE_AGENT") == null) return error.SkipZigTest;

    const gpa = std.testing.allocator;

    // Refuse to run if `claude` is not on PATH (operator misconfiguration).
    {
        const cv = runCmd(gpa, null, &.{ "claude", "--version" }, &.{});
        defer cv.deinit();
        if (!cv.ok()) {
            std.debug.print("\nplanar_execute_agent_live_test: `claude --version` failed; skipping live test.\n", .{});
            return error.SkipZigTest;
        }
    }

    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // ---- 1. Seed the fixture DB via the real planar CLI.
    _ = suite.registerProject("m4smoke");
    suite.addAssoc("m4smoke", "project");

    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan",     "create",    "--json",                              "--slug",
        "m4-smoke", "--summary", "Plan 492 M4 live-spawn smoke target", "M4 smoke plan",
    });
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;
    const plan_slug = plan.slug orelse @panic("plan create did not return a slug");

    const task_body =
        "Create a file named m4-smoke.txt containing exactly the text `ok` in the current directory, " ++
        "then run `git add m4-smoke.txt && git commit -m 'm4 smoke'`. Then exit. Do nothing else.";
    const task = suite.mustRunJSON(TaskJSON, arena, &.{
        "task",                          "add",           "--json", "--plan",  plan_id_str,
        "--slug",                        "m4-smoke",      "--body", task_body, "--next-action",
        "write and commit m4-smoke.txt", "M4 smoke task",
    });
    // ---- 2. Front-half (M5-deferred, done by the test): throwaway git repo,
    //         cycle branch + worktree, claim acquisition.
    const repo = suite.freshSystemTmpDir();
    git(gpa, repo, &.{ "init", "-q", "-b", "main" });
    git(gpa, repo, &.{ "config", "user.email", "m4@example.com" });
    git(gpa, repo, &.{ "config", "user.name", "M4 Smoke" });
    // Seed an initial commit so the repo has a HEAD to branch from.
    {
        const seed_path = std.fmt.allocPrint(arena, "{s}/seed.txt", .{repo}) catch unreachable;
        std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = seed_path, .data = "seed\n" }) catch
            @panic("write seed.txt failed");
    }
    git(gpa, repo, &.{ "add", "seed.txt" });
    git(gpa, repo, &.{ "commit", "-q", "-m", "seed" });

    // Cycle branch name MUST match worktree.cycleBranch(plan_slug, task_slug).
    const cycle_branch = std.fmt.allocPrint(arena, "cycle/{s}/m4-smoke", .{plan_slug}) catch unreachable;
    const worktree_path = std.fmt.allocPrint(arena, "{s}-wt", .{repo}) catch unreachable;
    // git worktree add -b <branch> <path> main
    git(gpa, repo, &.{ "worktree", "add", "-b", cycle_branch, worktree_path, "main" });

    // Acquire the claim on the task (the M5 scheduler will own this).
    const claim = blk: {
        const c = runCmd(gpa, repo, &.{
            resolveBinForName("planar-agent"),                                   "claim",               "--entity",
            std.fmt.allocPrint(arena, "task:{d}", .{task.id}) catch unreachable, "--role",              "coder",
            "--worktree",                                                        worktree_path,         "--ttl",
            "900",                                                               "--no-locality-probe", "--json",
        }, &.{
            .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        });
        defer c.deinit();
        if (!c.ok()) std.debug.panic("planar-agent claim failed ({d}): {s}", .{ c.code(), c.stderr });
        // `.alloc_always` so claim_token OWNS its bytes — without it the field
        // borrows into `c.stdout`, which `c.deinit()` frees on block exit,
        // leaving a dangling pointer (the planShow use-after-free class).
        const parsed = std.json.parseFromSlice(ClaimJSON, arena, c.stdout, .{
            .ignore_unknown_fields = true,
            .allocate = .alloc_always,
        }) catch std.debug.panic("claim json parse failed: {s}", .{c.stdout});
        break :blk parsed.value.claim_token;
    };

    // ---- 3. The smoke.lua workflow: forward ctx.args into agent() opts.
    //         ctx.args[1] = worktree_path, [2] = task_slug, [3] = claim_token.
    const smoke_lua =
        \\return {
        \\  meta = { name = "m4-smoke", description = "live spawn smoke", phases = {} },
        \\  run = function(ctx)
        \\    local brief = "Follow the task instructions exactly: create a file " ..
        \\      "named m4-smoke.txt containing the text ok in the current directory, " ..
        \\      "then run git add m4-smoke.txt && git commit -m 'm4 smoke'. Then exit. " ..
        \\      "Do not run any other commands."
        \\    local r = ctx.agent(brief, {
        \\      role = "coder",
        \\      worktree_path = ctx.args[1],
        \\      task_slug = ctx.args[2],
        \\      claim_token = ctx.args[3],
        \\      role_spec = "",
        \\    })
        \\    assert(r.status ~= "stub", "expected a real spawn, got the M2 stub")
        \\    ctx.log("agent status: " .. tostring(r.status) ..
        \\      " exit=" .. tostring(r.exit_code) ..
        \\      " commit=" .. tostring(r.commit_present) ..
        \\      " verb=" .. tostring(r.terminal_verb))
        \\  end,
        \\}
    ;
    const smoke_path = std.fmt.allocPrint(arena, "{s}/smoke.lua", .{repo}) catch unreachable;
    std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = smoke_path, .data = smoke_lua }) catch
        @panic("write smoke.lua failed");

    // Pre-spawn cycle-branch HEAD.
    const head_before = revParse(gpa, arena, repo, cycle_branch);

    // ---- 4. Run live. PATH prepended with the built bin dir so the worker's
    //         shim (planar-agent + git) and the driver's planar-watch resolve to
    //         the freshly-built binaries. PLANAR_DB = fixture so the driver's
    //         claim read + terminal verb hit the fixture. cwd = the git repo so
    //         `git rev-parse --show-toplevel` resolves to it.
    const old_path = envValue("PATH") orelse "";
    const new_path = std.fmt.allocPrint(arena, "{s}:{s}", .{ binDir(), old_path }) catch unreachable;

    const run = runCmd(gpa, repo, &.{
        resolveExecuteBin(), "run",         "--plan",   plan_id_str,
        "smoke.lua",         worktree_path, "m4-smoke", claim,
    }, &.{
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PATH", .value = new_path },
        .{ .key = "PLANAR_EXECUTE_LIVE_AGENT", .value = "1" },
    });
    defer run.deinit();

    std.debug.print(
        "\nplanar-execute run exit={d}\n--- stdout ---\n{s}\n--- stderr ---\n{s}\n",
        .{ run.code(), run.stdout, run.stderr },
    );

    // ---- 5. Assertions.
    try std.testing.expectEqual(@as(u32, 0), run.code());

    // The cycle branch HEAD advanced (the worker's commit landed).
    const head_after = revParse(gpa, arena, repo, cycle_branch);
    try std.testing.expect(!std.mem.eql(u8, head_before, head_after));

    // m4-smoke.txt exists in the worktree and is tracked by git.
    {
        const tracked = runCmd(gpa, repo, &.{ "git", "-C", worktree_path, "ls-files", "m4-smoke.txt" }, &.{});
        defer tracked.deinit();
        try std.testing.expect(tracked.ok());
        try std.testing.expect(std.mem.indexOf(u8, tracked.stdout, "m4-smoke.txt") != null);
    }

    // The claim is no longer ACTIVE: the driver ran the terminal verb against
    // the fixture DB. `planar-watch ps --plan <id> --json` should not list the
    // token as active.
    {
        const ps = runCmd(gpa, repo, &.{
            resolveBinForName("planar-watch"), "ps", "--plan", plan_id_str, "--json",
        }, &.{
            .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        });
        defer ps.deinit();
        try std.testing.expect(ps.ok());
        // The active-claims view must not still carry our token as live. (A
        // terminal verb flips status off `active`; the token may still appear in
        // a history view, but not as an active lease.)
        const has_active_token = std.mem.indexOf(u8, ps.stdout, claim) != null and
            std.mem.indexOf(u8, ps.stdout, "\"status\":\"active\"") != null;
        if (has_active_token) {
            std.debug.print("\nplanar-watch ps still shows an active claim: {s}\n", .{ps.stdout});
        }
        try std.testing.expect(!has_active_token);
    }
}

/// resolveBinForName returns the absolute path to a sibling binary in the same
/// dir as PLANAR_BIN (the freshly-built bin dir). Used for planar-agent /
/// planar-watch which the harness Suite does not expose directly.
fn resolveBinForName(name: []const u8) []const u8 {
    // Leak into the test arena via a static buffer is unsafe across calls, so
    // build into the gpa-backed page allocator and intern. Tests are short-lived
    // and this runs at most a couple times, so the small leak is acceptable; but
    // we use a fixed buffer cache keyed by name to avoid unbounded growth.
    const S = struct {
        var agent_buf: [std.fs.max_path_bytes]u8 = undefined;
        var watch_buf: [std.fs.max_path_bytes]u8 = undefined;
    };
    const buf: []u8 = if (std.mem.eql(u8, name, "planar-agent")) &S.agent_buf else &S.watch_buf;
    return std.fmt.bufPrint(buf, "{s}/{s}", .{ binDir(), name }) catch @panic("path too long");
}

/// revParse returns the trimmed sha of `branch` in `repo`. Panics on failure
/// (the branch must exist for the assertions to be meaningful).
fn revParse(gpa: std.mem.Allocator, arena: std.mem.Allocator, repo: []const u8, branch: []const u8) []const u8 {
    const r = runCmd(gpa, null, &.{ "git", "-C", repo, "rev-parse", branch }, &.{});
    defer r.deinit();
    if (!r.ok()) std.debug.panic("git rev-parse {s} failed: {s}", .{ branch, r.stderr });
    const trimmed = std.mem.trim(u8, r.stdout, " \t\r\n");
    return arena.dupe(u8, trimmed) catch @panic("OOM");
}
