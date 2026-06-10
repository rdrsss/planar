//! Integration tests for `planar-execute` (plan 492 M1 task 3165).
//!
//! Black-box exercises against the compiled `planar-execute` binary:
//!
//! - `planar-execute <workflow.lua>` exits 0 for a well-formed workflow
//!   whose run() does pure Lua (no host fns — those are M2).
//! - Trailing [args…] are threaded into ctx.args[1], ctx.args[2], ...
//!   as a 1-based Lua sequence; the workflow can assert on them.
//! - `planar-execute version` prints a version line and exits 0.
//! - `planar-execute /no/such/file.lua` exits non-zero with a message on stderr.
//! - A workflow whose run() calls error() exits non-zero with the message.
//! - A syntactically invalid workflow exits non-zero (compile error path).
//! - A workflow that returns a non-table exits with code 2 (invalid module shape).
//!
//! ## Database isolation (plan 499 task 3265)
//!
//! Every spawn in this suite goes through `Iso`, which injects a TmpDir-scoped
//! `PLANAR_DB` (plus an isolated `PLANAR_HOME` / config / templates / workbench
//! root) into the child's environment. See the `Iso` doc comment for the
//! root-cause writeup — without this, every `planar-execute` (and, under the
//! live gate, every `planar-agent` it shells) ran against the operator's REAL
//! `~/.planar/planar.db`.

const std = @import("std");

fn resolveExecuteBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_EXECUTE_BIN=")) return s["PLANAR_EXECUTE_BIN=".len..];
    }
    @panic("PLANAR_EXECUTE_BIN is not set. Run via: zig build test-integration");
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

    fn exitCode(self: RunResult) u32 {
        return switch (self.term) {
            .exited => |code| code,
            else => 255,
        };
    }
};

/// Isolation harness for the planar-execute integration suite.
///
/// ## Why this exists (plan 499 task 3265)
///
/// `planar-execute` holds NO DB handle by design — it shells the real
/// `planar` / `planar-agent` binaries, which resolve the ambient database
/// from `PLANAR_DB` (falling back to `$HOME/.planar/planar.db`). The earlier
/// `buildEnvWithoutGate` helper copied the host environ and stripped ONLY
/// `PLANAR_EXECUTE_LIVE_AGENT` — it never set `PLANAR_DB`. So every spawned
/// `planar-execute` (and, under the live gate, every `planar-agent` it shells)
/// ran against the OPERATOR'S REAL `~/.planar/planar.db`. Under the live gate
/// that silently completed real todo tasks on real dev plans, and the stray
/// run-locks / cycle worktrees under the real `~/.planar` produced the
/// nondeterministic `--listen` parallel hard-abort.
///
/// The fix mirrors what every SIBLING execute test file already does
/// (`planar_execute_doctor_test.zig`, `_eligible_`, `_quality_spine_`,
/// `_refusal_guard_`, `_agent_live_`): inject a TmpDir-scoped `PLANAR_DB` —
/// and, defensively, an isolated `PLANAR_HOME` plus the config / templates /
/// workbench roots — into every child's environment. Those siblings route
/// through `harness.Suite` (which already injects `PLANAR_DB`); this file's
/// tests are pure CLI-contract checks that never need seeded plan data
/// (no `--plan`; mock / dry-run / arg-threading / sandbox only), so a
/// self-contained `Iso` carrying its own per-test TmpDir is the lighter fit.
///
/// `PLANAR_DB` is the primary isolation knob (`resolveDbPath` honors it
/// directly). `PLANAR_HOME` + the other roots are belt-and-suspenders so any
/// config / templates / workbench read the shelled `planar` performs also
/// lands inside the TmpDir, never the operator's home.
const Iso = struct {
    tmp: std.testing.TmpDir,
    abs: []u8,
    db_path: []u8,
    home_path: []u8,
    config_path: []u8,
    gpa: std.mem.Allocator,

    fn init(gpa: std.mem.Allocator) !Iso {
        var tmp = std.testing.tmpDir(.{});
        errdefer tmp.cleanup();
        var buf: [std.fs.max_path_bytes]u8 = undefined;
        const len = try tmp.dir.realPath(std.testing.io, &buf);
        const abs = try gpa.dupe(u8, buf[0..len]);
        errdefer gpa.free(abs);
        const db_path = try std.fs.path.join(gpa, &.{ abs, "planar.db" });
        errdefer gpa.free(db_path);
        // An isolated home subdir so any shelled `planar` config/templates/
        // workbench read lands here, never `$HOME/.planar`.
        try tmp.dir.createDirPath(std.testing.io, "home");
        const home_path = try std.fs.path.join(gpa, &.{ abs, "home" });
        errdefer gpa.free(home_path);
        // A config path INSIDE the TmpDir that does not exist — the config
        // loader treats a missing file as "use defaults", so this guarantees
        // the operator's real ~/.planar/config.toml is never read.
        const config_path = try std.fs.path.join(gpa, &.{ abs, "home", "config.toml" });
        return .{
            .tmp = tmp,
            .abs = abs,
            .db_path = db_path,
            .home_path = home_path,
            .config_path = config_path,
            .gpa = gpa,
        };
    }

    fn deinit(self: *Iso) void {
        self.gpa.free(self.config_path);
        self.gpa.free(self.home_path);
        self.gpa.free(self.db_path);
        self.gpa.free(self.abs);
        self.tmp.cleanup();
    }

    /// Build an explicit subprocess env from the host environ that:
    ///   - strips PLANAR_EXECUTE_LIVE_AGENT (so non-gated tests are immune to
    ///     the operator running `PLANAR_EXECUTE_LIVE_AGENT=1 make
    ///     test-integration`); callers that WANT the gate re-add it,
    ///   - pins PLANAR_DB to this Iso's TmpDir db file (the load-bearing
    ///     isolation — without it the child resolved the operator's real DB),
    ///   - pins PLANAR_HOME + config / templates / workbench roots into the
    ///     TmpDir so no shelled `planar` read escapes to the real home.
    /// Caller owns the returned map and must call `.deinit()`.
    fn buildEnv(self: *const Iso, gpa: std.mem.Allocator) !std.process.Environ.Map {
        const raw: [*:null]?[*:0]u8 = std.c.environ;
        var env_count: usize = 0;
        while (raw[env_count]) |_| : (env_count += 1) {}
        const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
        const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
        const host_environ: std.process.Environ = .{ .block = posix_block };
        var env_map = try host_environ.createMap(gpa);
        errdefer env_map.deinit();
        _ = env_map.swapRemove("PLANAR_EXECUTE_LIVE_AGENT");
        // Load-bearing: pin the ambient DB into the TmpDir.
        try env_map.put("PLANAR_DB", self.db_path);
        // Defense-in-depth: redirect every home-derived read into the TmpDir.
        try env_map.put("PLANAR_HOME", self.home_path);
        try env_map.put("PLANAR_CONFIG_PATH", self.config_path); // missing file under tmp ⇒ defaults; never the real config
        try env_map.put("PLANAR_TEMPLATES_DIR", self.home_path);
        try env_map.put("PLANAR_WORKBENCH_ROOT", self.home_path);
        return env_map;
    }

    fn run(self: *const Iso, args: []const []const u8) !RunResult {
        return runWithEnv(self, self.gpa, args, false);
    }

    fn runGated(self: *const Iso, args: []const []const u8) !RunResult {
        return runWithEnv(self, self.gpa, args, true);
    }

    fn workflowPath(self: *const Iso, name: []const u8) ![]u8 {
        return std.fs.path.join(self.gpa, &.{ self.abs, name });
    }

    fn writeWorkflow(self: *const Iso, name: []const u8, content: []const u8) !void {
        var f = try self.tmp.dir.createFile(std.testing.io, name, .{});
        defer f.close(std.testing.io);
        try f.writeStreamingAll(std.testing.io, content);
    }
    /// The PLANAR_DB this Iso injects — used by the isolation guard test to
    /// assert the child sees a TmpDir-scoped DB, never the operator's home.
    fn dbPath(self: *const Iso) []const u8 {
        return self.db_path;
    }
};

/// Shared spawn path for `Iso.run` / `Iso.runGated`. Builds the isolated env,
/// optionally re-adds the live gate, and execs the binary.
fn runWithEnv(iso: *const Iso, gpa: std.mem.Allocator, args: []const []const u8, gated: bool) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

    var env_map = try iso.buildEnv(gpa);
    defer env_map.deinit();
    if (gated) env_map.put("PLANAR_EXECUTE_LIVE_AGENT", "1") catch @panic("OOM injecting gate var");

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
// Isolation regression guard (plan 499 task 3265)
// ---------------------------------------------------------------------------

test "planar-execute isolation: the test env pins PLANAR_DB inside the TmpDir, never the operator's real DB (task 3265)" {
    // Regression guard for the real-DB-mutation root cause. We assert TWO things:
    //
    //   1. The env this suite builds for EVERY child carries a PLANAR_DB that
    //      lives inside the per-test TmpDir (`abs`) and is NOT the operator's
    //      `~/.planar/planar.db`. This is verified directly on the env map so
    //      the assertion holds even for spawn shapes that never touch the DB.
    //
    //   2. A workflow that drives the FULL agent() claim ritual under
    //      --mock-worker runs cleanly AND, because the FakeSpawner pipeline
    //      shells nothing against the real DB, leaves the operator's home
    //      untouched. The TmpDir DB is what any shelled `planar`/`planar-agent`
    //      would resolve — proven by assertion (1).
    //
    // The point is that a future maintainer who reintroduces a host-environ
    // copy without PLANAR_DB (the original bug) gets a red test here.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    // (1) The injected PLANAR_DB must be inside the TmpDir and must not be the
    //     real home DB.
    var env_map = try iso.buildEnv(gpa);
    defer env_map.deinit();
    const injected_db = env_map.get("PLANAR_DB") orelse {
        std.debug.print("\nPLANAR_DB was not injected into the test env\n", .{});
        return error.TestUnexpectedResult;
    };
    try std.testing.expect(std.mem.startsWith(u8, injected_db, iso.abs));
    try std.testing.expect(std.mem.indexOf(u8, injected_db, "/.planar/planar.db") == null);
    // PLANAR_HOME must also point inside the TmpDir.
    const injected_home = env_map.get("PLANAR_HOME") orelse {
        std.debug.print("\nPLANAR_HOME was not injected into the test env\n", .{});
        return error.TestUnexpectedResult;
    };
    try std.testing.expect(std.mem.startsWith(u8, injected_home, iso.abs));

    // (2) Drive a full agent() ritual under --mock-worker and confirm it runs
    //     cleanly against the isolated env.
    const wf_src =
        \\return {
        \\  meta = { name = "iso-guard", description = "claim ritual under mock", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/iso-wt",
        \\      claim_token = "tok-iso",
        \\      task_slug = "ts-iso",
        \\    })
        \\    assert(r.status ~= "stub", "agent must run the real pipeline under --mock-worker")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("iso_guard.lua", wf_src);
    const wf_path = try iso.workflowPath("iso_guard.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-worker", wf_path });
    defer res.deinit();
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    // The TmpDir DB may or may not have been created (mock mode shells
    // nothing), but the operator's real DB must be irrelevant: assertion (1)
    // already proved the child could only ever resolve `injected_db`.
    _ = iso.dbPath();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "planar-execute version prints a version line and exits 0" {
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const res = try iso.run(&.{"version"});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // Output must contain "planar-execute" and a Lua version marker.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "planar-execute") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "lua") != null or
        std.mem.indexOf(u8, res.stdout, "Lua") != null);
}

test "planar-execute: trivial workflow exits 0" {
    // A well-formed workflow whose run() does only pure Lua (no host fns)
    // must succeed. This is the M1 end-to-end smoke test.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "trivial", description = "M1 smoke", phases = {} },
        \\  run = function(ctx)
        \\    -- pure Lua, no host fns; must succeed.
        \\    local x = 1 + 1
        \\    _ = x
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("trivial.lua", wf_src);
    const wf_path = try iso.workflowPath("trivial.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute: trailing args reach ctx.args as 1-based sequence" {
    // Verify the CLI arg-threading contract: [args…] after the workflow path
    // are threaded into run(ctx) as ctx.args[1], ctx.args[2], ...
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "args-check", description = "threading", phases = {} },
        \\  run = function(ctx)
        \\    assert(ctx.args[1] == "alpha", "expected ctx.args[1]='alpha'")
        \\    assert(ctx.args[2] == "beta",  "expected ctx.args[2]='beta'")
        \\    assert(ctx.args[3] == nil,     "expected ctx.args[3]=nil")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("args_check.lua", wf_src);
    const wf_path = try iso.workflowPath("args_check.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ wf_path, "alpha", "beta" });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute: missing file exits non-zero with message" {
    // A file path that does not exist must produce a non-zero exit and a
    // human-readable message on stderr.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const res = try iso.run(&.{"/no/such/planar-execute-test-file.lua"});
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    // stderr must mention the file path or a meaningful error keyword.
    try std.testing.expect(
        std.mem.indexOf(u8, res.stderr, "cannot read") != null or
            std.mem.indexOf(u8, res.stderr, "FileNotFound") != null or
            std.mem.indexOf(u8, res.stderr, "no such") != null,
    );
}

test "planar-execute: runtime error in run() exits non-zero with message" {
    // A workflow whose run() calls error() must exit non-zero and put
    // the Lua error message on stderr.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "boom", description = "errors", phases = {} },
        \\  run = function(ctx)
        \\    error("intentional test error")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("boom.lua", wf_src);
    const wf_path = try iso.workflowPath("boom.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "intentional test error") != null);
}

test "planar-execute: compile error exits 3 with message" {
    // A syntactically invalid workflow must exit with code 3 (compile error).
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("bad_syntax.lua", "this is not valid lua @@@@\n");
    const wf_path = try iso.workflowPath("bad_syntax.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 3), res.exitCode());
    try std.testing.expect(res.stderr.len > 0);
}

test "planar-execute: non-table return exits 2 (invalid module shape)" {
    // A workflow that returns a number instead of a table must exit with
    // code 2 (invalid module structure).
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("not_table.lua", "return 42\n");
    const wf_path = try iso.workflowPath("not_table.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 2), res.exitCode());
}

test "planar-execute: help flag exits 0" {
    // `planar-execute --help` must exit 0 and produce help text.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const res = try iso.run(&.{"--help"});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(
        std.mem.indexOf(u8, res.stdout, "planar-execute") != null,
    );
}

// ---------------------------------------------------------------------------
// task 3166 — --dry-run integration tests
// ---------------------------------------------------------------------------

test "planar-execute --dry-run: well-formed workflow prints meta and phases, exits 0" {
    // --dry-run on a well-formed workflow with ≥2 phases must print the
    // workflow name, description, and each phase title, then exit 0.
    // run(ctx) must NOT be called (the run body errors; that error must be silent).
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = {
        \\    name = "preview-workflow",
        \\    description = "A workflow to preview",
        \\    phases = {
        \\      { title = "Initialize", detail = "set up the env" },
        \\      { title = "Execute", detail = "run the tasks" },
        \\      { title = "Finalize", detail = "clean up" },
        \\    },
        \\  },
        \\  run = function(ctx)
        \\    error("run must not be called under --dry-run")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("preview.lua", wf_src);
    const wf_path = try iso.workflowPath("preview.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--dry-run", wf_path });
    defer res.deinit();

    // Must exit 0 — run() was never called despite its error body.
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    // Output must contain the workflow name.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "preview-workflow") != null);
    // Output must contain the description.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "A workflow to preview") != null);
    // Output must contain each phase title.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "Initialize") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "Execute") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "Finalize") != null);
}

test "planar-execute --dry-run: run body that would error still exits 0 (run not entered)" {
    // Load-bearing test: a workflow whose run body calls error() MUST still
    // exit 0 under --dry-run. Without --dry-run the same workflow exits non-zero.
    // Both arms are asserted to pin the flag's effect.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = {
        \\    name = "error-in-run",
        \\    description = "run would fail",
        \\    phases = {
        \\      { title = "Only phase", detail = "" },
        \\    },
        \\  },
        \\  run = function(ctx)
        \\    error("must not run under dry-run")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("error_run.lua", wf_src);
    const wf_path = try iso.workflowPath("error_run.lua");
    defer gpa.free(wf_path);

    // ARM 1: --dry-run → exit 0, run never entered.
    const dry = try iso.run(&.{ "run", "--dry-run", wf_path });
    defer dry.deinit();
    try std.testing.expectEqual(@as(u32, 0), dry.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, dry.stdout, "error-in-run") != null);

    // ARM 2: no --dry-run → exit non-zero, error message on stderr.
    const live = try iso.run(&.{wf_path});
    defer live.deinit();
    try std.testing.expect(live.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, live.stderr, "must not run under dry-run") != null);
}

test "planar-execute --dry-run: malformed module still exits non-zero (load/validate shared)" {
    // --dry-run does NOT bypass load+validate. A workflow that returns a
    // non-table must still exit with code 2 under --dry-run.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("not_table_dr.lua", "return 42\n");
    const wf_path = try iso.workflowPath("not_table_dr.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--dry-run", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 2), res.exitCode());
}

test "planar-execute --dry-run: compile error still exits 3 (load/validate shared)" {
    // A syntactically invalid workflow must exit 3 under --dry-run, same as
    // without the flag — the load+validate path is shared.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("bad_syntax_dr.lua", "this is not valid lua @@@@\n");
    const wf_path = try iso.workflowPath("bad_syntax_dr.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--dry-run", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 3), res.exitCode());
}

// ---------------------------------------------------------------------------
// task 3168 (m2-host-fns) + 3169 (m2-sandbox) — host functions on ctx + sandbox
//
// These exercise the user-visible contract through the compiled binary: the
// host functions are recording stubs carried on ctx, and the sandbox closes the
// os/io/os.time/math.random holes while exposing host-injected determinism.
// In-process observability of the recorded calls is covered by the unit tests;
// here we assert the run-success / run-failure contract the operator sees.
// ---------------------------------------------------------------------------

test "planar-execute: workflow calling ctx host fns runs successfully (task 3168)" {
    // A workflow whose run(ctx) drives phase/log/agent/parallel/pipeline/
    // workflow/budget must exit 0 — the stubs record and return, never erroring.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "host-fns", description = "drive every host fn", phases = {} },
        \\  run = function(ctx)
        \\    ctx.phase("Build")
        \\    ctx.log("working")
        \\    local r = ctx.agent("do the thing", { role = "coder" })
        \\    assert(r.status == "stub", "agent must return a stub result table")
        \\    ctx.parallel({ function() end, function() end })
        \\    ctx.pipeline({ "a", "b" }, function() end)
        \\    ctx.workflow("sub", {})
        \\    assert(ctx.budget.total == 100, "budget.total injected")
        \\    assert(ctx.budget:remaining() == 100, "remaining = total - spent")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("host_fns.lua", wf_src);
    const wf_path = try iso.workflowPath("host_fns.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute sandbox: os.execute call fails (absent) (task 3169)" {
    // os is never opened → os is nil → os.execute is an index-on-nil runtime
    // error. The workflow must exit non-zero.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "os-exec", description = "must fail", phases = {} },
        \\  run = function(ctx) os.execute("echo pwned") end,
        \\}
    ;
    try iso.writeWorkflow("os_exec.lua", wf_src);
    const wf_path = try iso.workflowPath("os_exec.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(res.stderr.len > 0);
}

test "planar-execute sandbox: io / os.time / math.random absent, ctx.now/seed present (task 3169)" {
    // Single workflow that asserts the full sandbox contract: io and os nil,
    // math.random / randomseed nil, math/string/table still work, and the
    // host-injected ctx.now / ctx.seed are present. Exit 0 proves every assert
    // held.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "sandbox", description = "holes closed", phases = {} },
        \\  run = function(ctx)
        \\    assert(io == nil, "io must be absent")
        \\    assert(os == nil, "os must be absent (os.time unreachable)")
        \\    assert(math.random == nil, "math.random stripped")
        \\    assert(math.randomseed == nil, "math.randomseed stripped")
        \\    assert(load == nil, "load stripped")
        \\    assert(math.floor(2.9) == 2, "math.floor kept")
        \\    assert(string.upper("x") == "X", "string lib kept")
        \\    assert(type(ctx.now) == "number", "ctx.now injected")
        \\    assert(type(ctx.seed) == "number", "ctx.seed injected")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("sandbox.lua", wf_src);
    const wf_path = try iso.workflowPath("sandbox.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute sandbox: debug and package globals are absent (task 3232)" {
    // Regression guard: debug.getupvalue could pierce the HostState
    // light-userdata upvalue; debug.getregistry reaches LUA_LOADED_TABLE.
    // package exposes module-loader internals. Neither is opened in
    // openSandboxedLibs, so both globals must be nil. A future maintainer
    // adding luaopen_debug "for diagnostics" must get a red test here.
    // (extended task 3232)
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "no-debug-pkg", description = "absent", phases = {} },
        \\  run = function(ctx)
        \\    assert(debug == nil, "debug must be absent (task 3232)")
        \\    assert(package == nil, "package must be absent (task 3232)")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("no_debug_pkg.lua", wf_src);
    const wf_path = try iso.workflowPath("no_debug_pkg.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute sandbox: string.dump present, load nil — bytecode out, no re-execution path (task 3233)" {
    // string.dump IS present (full string lib is opened) and CAN serialize
    // function bytecode. But load/loadstring/dofile/loadfile/require are all
    // nil'd, closing every re-execution path. Both halves are pinned here:
    // bytecode serialization works, bytecode re-execution is impossible.
    // A future maintainer re-opening load must get a red test here. (task 3233)
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "dump-no-load", description = "bytecode guard", phases = {} },
        \\  run = function(ctx)
        \\    local fn = function(x) return x + 1 end
        \\    local bytecode = string.dump(fn)
        \\    assert(type(bytecode) == "string" and #bytecode > 0,
        \\           "string.dump must return non-empty bytecode (task 3233)")
        \\    assert(load == nil,       "load must be nil (task 3233)")
        \\    assert(loadstring == nil, "loadstring must be nil (task 3233)")
        \\    assert(loadfile == nil,   "loadfile must be nil (task 3233)")
        \\    assert(dofile == nil,     "dofile must be nil (task 3233)")
        \\    assert(require == nil,    "require must be nil (task 3233)")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("dump_no_load.lua", wf_src);
    const wf_path = try iso.workflowPath("dump_no_load.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute --dry-run: host-fn workflow still never enters run (task 3168/3169)" {
    // The dry-run guarantee is unchanged by the host surface: a workflow whose
    // run body would error must still exit 0 under --dry-run (run not entered),
    // and exit non-zero without it (run entered, error raised).
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = {
        \\    name = "dry-host",
        \\    description = "run errors; dry-run must skip it",
        \\    phases = { { title = "P", detail = "" } },
        \\  },
        \\  run = function(ctx)
        \\    ctx.phase("Build")
        \\    error("run body must not execute under --dry-run")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("dry_host.lua", wf_src);
    const wf_path = try iso.workflowPath("dry_host.lua");
    defer gpa.free(wf_path);

    // ARM 1: --dry-run → exit 0, run never entered.
    const dry = try iso.run(&.{ "run", "--dry-run", wf_path });
    defer dry.deinit();
    try std.testing.expectEqual(@as(u32, 0), dry.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, dry.stdout, "dry-host") != null);

    // ARM 2: live run → exit non-zero, error surfaced.
    const live = try iso.run(&.{wf_path});
    defer live.deinit();
    try std.testing.expect(live.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, live.stderr, "must not execute under --dry-run") != null);
}

// ---------------------------------------------------------------------------
// task 3264 — agent-free workflows must not require --plan under the live gate
// ---------------------------------------------------------------------------

test "planar-execute: agent-free workflow runs cleanly under PLANAR_EXECUTE_LIVE_AGENT=1 without --plan (task 3264)" {
    // Regression guard for task 3264: a pure-Lua workflow that never calls
    // agent() must exit 0 when PLANAR_EXECUTE_LIVE_AGENT=1 is set, even
    // without --plan. Before the fix, the gate block eagerly required --plan
    // and the process exited 1 before the workflow ran.
    //
    // This test injects PLANAR_EXECUTE_LIVE_AGENT=1 for this one invocation
    // only (via iso.runGated) so it runs in CI without a real claude spawn.
    // The DB it would resolve is the TmpDir-scoped PLANAR_DB (task 3265), so
    // even though the gate attaches a live driver, nothing touches the real DB.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "agent-free-gated", description = "pure Lua, no agent() call", phases = {} },
        \\  run = function(ctx)
        \\    -- No agent() call. Must exit 0 even under PLANAR_EXECUTE_LIVE_AGENT=1.
        \\    local x = 1 + 1
        \\    assert(x == 2, "basic Lua must work")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("agent_free_gated.lua", wf_src);
    const wf_path = try iso.workflowPath("agent_free_gated.lua");
    defer gpa.free(wf_path);

    const res = try iso.runGated(&.{wf_path});
    defer res.deinit();

    // Must exit 0 — no agent() call, so binary resolution and --plan absence
    // are irrelevant. The driver is attached in degraded mode but never used.
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

// ---------------------------------------------------------------------------
// task 3202 — `--mock-worker` mode: exercise workflow control flow without
// spawning a real `claude -p`. The mode wires an in-process FakeSpawner-backed
// AgentDriver into `handleRun`, so the FULL agent() pipeline (claim → brief →
// spawn → wait → terminal decision → result table → fan-in) runs end-to-end
// against canned outcomes (exit_code=0, stdout="ok", stderr=""). Mutually
// exclusive with --dry-run and PLANAR_EXECUTE_LIVE_AGENT=1.
// ---------------------------------------------------------------------------

test "planar-execute --mock-worker: flag advertised in `run --help` (task 3202)" {
    // Discoverability is part of the contract: a workflow author looking at
    // `planar-execute run --help` must see --mock-worker described.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();
    const res = try iso.run(&.{ "run", "--help" });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "--mock-worker") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "FakeSpawner") != null);
}

test "planar-execute --mock-worker conflict: --dry-run AND --mock-worker exits non-zero (task 3202)" {
    // The two modes are mutually exclusive. Combining them is a wiring error
    // and must surface a clear, distinct message — operator picks one.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    ;
    try iso.writeWorkflow("x.lua", wf_src);
    const wf_path = try iso.workflowPath("x.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-worker", "--dry-run", wf_path });
    defer res.deinit();
    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "--mock-worker") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "--dry-run") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mutually exclusive") != null);
}

test "planar-execute --mock-worker conflict: PLANAR_EXECUTE_LIVE_AGENT + --mock-worker exits non-zero (task 3202)" {
    // The live gate spawns real `claude -p`; the mock attaches a FakeSpawner.
    // Combining them is incoherent — must reject loudly with both names in
    // the message so the operator knows what to unset/drop.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    ;
    try iso.writeWorkflow("x.lua", wf_src);
    const wf_path = try iso.workflowPath("x.lua");
    defer gpa.free(wf_path);

    const res = try iso.runGated(&.{ "run", "--mock-worker", wf_path });
    defer res.deinit();
    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "--mock-worker") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "PLANAR_EXECUTE_LIVE_AGENT") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mutually exclusive") != null);
}

test "planar-execute --mock-worker: single agent() call drives the full pipeline against the FakeSpawner (task 3202)" {
    // The deliverable. A workflow that calls ctx.agent(...) once under
    // --mock-worker must:
    //   1. exit 0,
    //   2. see a non-stub result (status is the natural decision-matrix
    //      outcome — "released" with no repo/commit) and exit_code=0,
    //   3. see the MOCK MODE notice on stderr,
    //   4. NOT spawn a real `claude -p` (proven by: the test does not put
    //      claude on PATH, and a real spawn would fail loudly; exit 0 + a
    //      sane status string proves the FakeSpawner served the call).
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "mock-single", description = "one agent call under mock", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/mock-wt",
        \\      claim_token = "tok-mock",
        \\      role_spec = "you are a coder",
        \\      task_slug = "ts-mock-1",
        \\    })
        \\    assert(r.status ~= "stub", "agent must NOT return stub under --mock-worker, got " .. tostring(r.status))
        \\    assert(r.status == "released", "expected released, got " .. tostring(r.status))
        \\    assert(r.exit_code == 0, "exit_code: " .. tostring(r.exit_code))
        \\    assert(r.commit_present == false, "commit_present should be false in mock (no repo)")
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("mock_single.lua", wf_src);
    const wf_path = try iso.workflowPath("mock_single.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-worker", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // MOCK MODE notice was printed.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "no real `claude -p` spawned") != null);
}

test "planar-execute --mock-worker: parallel agent calls drive the full scheduler against mocks (task 3202)" {
    // Proves the FULL pipeline (scheduler + parallel + results table) runs
    // against the mock — not just a one-shot agent(). The workflow drives
    // ctx.parallel({thunk1, thunk2}) each calling agent(); under --mock-worker
    // both thunks complete via the FakeSpawner, original-order results are
    // returned, and the run exits 0.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "mock-parallel", description = "parallel agent() under mock", phases = {} },
        \\  run = function(ctx)
        \\    local function mk(tag)
        \\      return function()
        \\        local r = ctx.agent("brief-" .. tag, {
        \\          role = "coder",
        \\          worktree_path = "/tmp/abs/mock-wt-" .. tag,
        \\          claim_token = "tok-" .. tag,
        \\          task_slug = "ts-" .. tag,
        \\        })
        \\        return r.status
        \\      end
        \\    end
        \\    local results = ctx.parallel({ mk("alpha"), mk("beta"), mk("gamma") })
        \\    assert(#results == 3, "expected 3 results, got " .. tostring(#results))
        \\    -- Original-order preserved.
        \\    assert(results[1] == "released", "results[1] = " .. tostring(results[1]))
        \\    assert(results[2] == "released", "results[2] = " .. tostring(results[2]))
        \\    assert(results[3] == "released", "results[3] = " .. tostring(results[3]))
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("mock_parallel.lua", wf_src);
    const wf_path = try iso.workflowPath("mock_parallel.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-worker", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
}

test "planar-execute --mock-worker: ungated/no-driver path is unchanged when --mock-worker is NOT set (task 3202)" {
    // Regression guard: dropping --mock-worker must keep the default M2 stub
    // behavior. agent() returns { status = "stub" } when the gate is off AND
    // --mock-worker is off. The MOCK MODE notice must NOT appear.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    const wf_src =
        \\return {
        \\  meta = { name = "no-mock", description = "default stub", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("b", { role = "coder" })
        \\    assert(r.status == "stub", "expected stub when --mock-worker is off, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("no_mock.lua", wf_src);
    const wf_path = try iso.workflowPath("no_mock.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // The MOCK MODE notice must NOT leak into a non-mock run.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") == null);
}

// ---------------------------------------------------------------------------
// task 3491 — `--mock-outcomes <file>` per-call scripted FakeSpawner outcomes
// ---------------------------------------------------------------------------
//
// Contract being pinned:
//   1. `--mock-outcomes` implies `--mock-worker` (no need to pass both).
//   2. The Nth agent() call returns the Nth scripted outcome in order.
//   3. Extra agent() calls beyond the script fall back to the default canned
//      outcome (exit_code=0, stdout="ok", stderr="").
//   4. A malformed NDJSON file produces a startup error (non-zero exit, clear
//      message) before any Lua runs.
//   5. `--mock-outcomes` is mutually exclusive with `--dry-run` and
//      `PLANAR_EXECUTE_LIVE_AGENT=1`.
//   6. `--mock-outcomes` is advertised in `run --help`.

test "planar-execute --mock-outcomes: flag advertised in `run --help` (task 3491)" {
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();
    const res = try iso.run(&.{ "run", "--help" });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "--mock-outcomes") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "NDJSON") != null);
}

test "planar-execute --mock-outcomes: 3-call scripted outcomes observed in order (task 3491)" {
    // The primary delivery: three scripted outcomes consumed in order.
    // The workflow calls agent() three times and asserts the per-call result.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    // Write the NDJSON outcomes file.
    const outcomes_src =
        \\{"exit_code":0,"stdout":"call-1-out","stderr":""}
        \\{"exit_code":1,"stdout":"","stderr":"call-2-err"}
        \\{"exit_code":0,"stdout":"call-3-out","stderr":""}
        \\
    ;
    try iso.writeWorkflow("outcomes.ndjson", outcomes_src);
    const outcomes_path = try iso.workflowPath("outcomes.ndjson");
    defer gpa.free(outcomes_path);

    // Write a workflow that makes 3 agent() calls and checks each call's
    // exit_code. Status "released" (exit 0 + no commit) or "failed" (exit ≠ 0)
    // both come from the FakeSpawner decision matrix; what matters is the
    // exit_code the outcome carried.
    //
    // We assert on r.exit_code directly: the scripted outcome is what the
    // FakeSpawner returns as the spawn result, and the terminal verb result
    // table always carries exit_code from the spawn outcome.
    const wf_src =
        \\return {
        \\  meta = { name = "outcomes-3", description = "3-call scripted outcomes", phases = {} },
        \\  run = function(ctx)
        \\    local r1 = ctx.agent("brief-1", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/mo-wt-1",
        \\      claim_token = "tok-mo-1",
        \\      task_slug = "ts-mo-1",
        \\    })
        \\    assert(r1.exit_code == 0, "call 1 exit_code: " .. tostring(r1.exit_code))
        \\
        \\    local r2 = ctx.agent("brief-2", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/mo-wt-2",
        \\      claim_token = "tok-mo-2",
        \\      task_slug = "ts-mo-2",
        \\    })
        \\    assert(r2.exit_code == 1, "call 2 exit_code: " .. tostring(r2.exit_code))
        \\
        \\    local r3 = ctx.agent("brief-3", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/mo-wt-3",
        \\      claim_token = "tok-mo-3",
        \\      task_slug = "ts-mo-3",
        \\    })
        \\    assert(r3.exit_code == 0, "call 3 exit_code: " .. tostring(r3.exit_code))
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("outcomes_3.lua", wf_src);
    const wf_path = try iso.workflowPath("outcomes_3.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // MOCK MODE notice must name the outcomes file.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "scripted outcomes") != null);
}

test "planar-execute --mock-outcomes: exhaustion fallback — more calls than outcomes (task 3491)" {
    // When there are MORE agent() calls than scripted outcomes, extra calls
    // fall back to the global canned default (exit_code=0). The workflow makes
    // 2 calls against a 1-line outcomes file and asserts both succeed.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    // One scripted outcome, but we make two calls.
    try iso.writeWorkflow("one.ndjson",
        \\{"exit_code":0,"stdout":"scripted","stderr":""}
        \\
    );
    const outcomes_path = try iso.workflowPath("one.ndjson");
    defer gpa.free(outcomes_path);

    const wf_src =
        \\return {
        \\  meta = { name = "fallback", description = "exhaustion fallback", phases = {} },
        \\  run = function(ctx)
        \\    local r1 = ctx.agent("brief-1", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/fb-wt-1",
        \\      claim_token = "tok-fb-1",
        \\      task_slug = "ts-fb-1",
        \\    })
        \\    -- First call: scripted outcome (exit 0).
        \\    assert(r1.exit_code == 0, "call 1 exit_code: " .. tostring(r1.exit_code))
        \\
        \\    -- Second call: fallback to canned default (exit 0, stdout="ok").
        \\    local r2 = ctx.agent("brief-2", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/fb-wt-2",
        \\      claim_token = "tok-fb-2",
        \\      task_slug = "ts-fb-2",
        \\    })
        \\    assert(r2.exit_code == 0, "call 2 fallback exit_code: " .. tostring(r2.exit_code))
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("fallback.lua", wf_src);
    const wf_path = try iso.workflowPath("fallback.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
}

test "planar-execute --mock-outcomes: malformed NDJSON file → startup error (task 3491)" {
    // A file with a non-JSON line must fail at startup (non-zero exit, clear
    // message) BEFORE any Lua code runs. Workflow correctness is irrelevant.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("bad.ndjson", "this is not json\n");
    const outcomes_path = try iso.workflowPath("bad.ndjson");
    defer gpa.free(outcomes_path);

    try iso.writeWorkflow("any.lua",
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try iso.workflowPath("any.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mock-outcomes") != null);
}

test "planar-execute --mock-outcomes: missing file → startup error (task 3491)" {
    // A path that does not exist must fail at startup with a clear message.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("any.lua",
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try iso.workflowPath("any.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-outcomes", "/no/such/outcomes.ndjson", wf_path });
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mock-outcomes") != null);
}

test "planar-execute --mock-outcomes: implies --mock-worker (no need to pass both) (task 3491)" {
    // Passing only --mock-outcomes (without --mock-worker) must enter MOCK MODE.
    // This is the "implies" contract: less friction for workflow authors.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("one.ndjson",
        \\{"exit_code":0,"stdout":"implied","stderr":""}
        \\
    );
    const outcomes_path = try iso.workflowPath("one.ndjson");
    defer gpa.free(outcomes_path);

    const wf_src =
        \\return {
        \\  meta = { name = "implies", description = "implies mock-worker", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("b", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/abs/impl-wt",
        \\      claim_token = "tok-impl",
        \\      task_slug = "ts-impl",
        \\    })
        \\    assert(r.status ~= "stub", "must not be stub under --mock-outcomes (implied mock mode)")
        \\    assert(r.exit_code == 0, "exit_code: " .. tostring(r.exit_code))
        \\  end,
        \\}
    ;
    try iso.writeWorkflow("implies.lua", wf_src);
    const wf_path = try iso.workflowPath("implies.lua");
    defer gpa.free(wf_path);

    // Pass ONLY --mock-outcomes, NOT --mock-worker.
    const res = try iso.run(&.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
}

test "planar-execute --mock-outcomes conflict: --dry-run exits non-zero (task 3491)" {
    // --mock-outcomes is mutually exclusive with --dry-run.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("x.ndjson", "{}\n");
    const outcomes_path = try iso.workflowPath("x.ndjson");
    defer gpa.free(outcomes_path);

    try iso.writeWorkflow("x.lua",
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try iso.workflowPath("x.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-outcomes", outcomes_path, "--dry-run", wf_path });
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mock-outcomes") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "--dry-run") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mutually exclusive") != null);
}

test "planar-execute --mock-outcomes conflict: PLANAR_EXECUTE_LIVE_AGENT exits non-zero (task 3491)" {
    // --mock-outcomes is mutually exclusive with the live gate.
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("x.ndjson", "{}\n");
    const outcomes_path = try iso.workflowPath("x.ndjson");
    defer gpa.free(outcomes_path);

    try iso.writeWorkflow("x.lua",
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try iso.workflowPath("x.lua");
    defer gpa.free(wf_path);

    const res = try iso.runGated(&.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mock-outcomes") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "PLANAR_EXECUTE_LIVE_AGENT") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mutually exclusive") != null);
}

// ---------------------------------------------------------------------------
// plan 492 tasks 3708 (dry-run model-table visibility) + 3709 (sonnet-coder
// default). Routing now resolves via `planar models routing` (plan 540); when
// `planar` is unreachable here, execute falls back to the compiled defaults,
// which these default-case assertions pin.
// ---------------------------------------------------------------------------

/// Extract the model paired with `role` from a `--dry-run` "dispatch model
/// table" block. Each row renders as `  <role><pad>→ <model>`. Returns the
/// trimmed model string, or null if no row's leading token equals `role`
/// exactly (so "coder" never matches the "test-coder" row).
const Cell = struct { vendor: []const u8, model: []const u8 };

/// Parse the `<vendor> <model>` cell for `role` from the dispatch table. Each
/// row renders as `  <role><pad> → <vendor> <model>`. Returns null if no row's
/// leading token equals `role` exactly (so "coder" never matches "test-coder").
fn rowCell(stdout: []const u8, role: []const u8) ?Cell {
    var lines = std.mem.splitScalar(u8, stdout, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        const arrow = std.mem.indexOf(u8, line, "→") orelse continue;
        const lhs = std.mem.trim(u8, line[0..arrow], " \t");
        if (!std.mem.eql(u8, lhs, role)) continue;
        const rhs = std.mem.trim(u8, line[arrow + "→".len ..], " \t");
        const sp = std.mem.indexOfScalar(u8, rhs, ' ') orelse return Cell{ .vendor = rhs, .model = "" };
        return Cell{
            .vendor = std.mem.trim(u8, rhs[0..sp], " \t"),
            .model = std.mem.trim(u8, rhs[sp + 1 ..], " \t"),
        };
    }
    return null;
}

/// Convenience: the model for `role`, or "<none>" when the row is absent.
fn rowModel(stdout: []const u8, role: []const u8) []const u8 {
    return (rowCell(stdout, role) orelse return "<none>").model;
}

test "planar-execute --dry-run: prints the dispatch model table for all four roles (task 3708)" {
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("tbl.lua",
        \\return {
        \\  meta = { name = "tbl-wf", description = "model table preview", phases = {} },
        \\  run = function(ctx) error("must not run under --dry-run") end,
        \\}
    );
    const wf_path = try iso.workflowPath("tbl.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--dry-run", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // A labelled dispatch table must be present, with one row per known role.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "dispatch model table") != null);
    for ([_][]const u8{ "coder", "reviewer", "test-coder", "documenter" }) |role| {
        if (rowCell(res.stdout, role) == null) {
            std.debug.print("\ndry-run table missing a row for role '{s}'\nstdout:\n{s}\n", .{ role, res.stdout });
            return error.TestUnexpectedResult;
        }
    }
}

test "planar-execute --dry-run: default routing is sonnet-coder / opus-reviewer (task 3709)" {
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    try iso.writeWorkflow("def.lua",
        \\return { meta = { name = "def-wf", description = "defaults", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try iso.workflowPath("def.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--dry-run", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expectEqualStrings("claude-sonnet-4-6", rowModel(res.stdout, "coder"));
    try std.testing.expectEqualStrings("claude-opus-4-8", rowModel(res.stdout, "reviewer"));
    try std.testing.expectEqualStrings("claude-sonnet-4-6", rowModel(res.stdout, "test-coder"));
    try std.testing.expectEqualStrings("claude-sonnet-4-6", rowModel(res.stdout, "documenter"));
    // Default vendor is claude for every role.
    for ([_][]const u8{ "coder", "reviewer", "test-coder", "documenter" }) |role| {
        try std.testing.expectEqualStrings("claude", (rowCell(res.stdout, role) orelse unreachable).vendor);
    }
}
// ---------------------------------------------------------------------------
// plan 492 task 3708 — ctx.dispatch_table() host fn + per-spawn dispatch banner
// ---------------------------------------------------------------------------

test "planar-execute ctx.dispatch_table(): returns the effective role→model mapping (task 3708)" {
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    // Asserts run inside the workflow; a mismatch errors run() → non-zero exit.
    // Default (ungated) mode: agent() is a stub, but dispatch_table() reflects
    // the resolved table on the host.
    try iso.writeWorkflow("dt.lua",
        \\return {
        \\  meta = { name = "dt", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    local t = ctx.dispatch_table()
        \\    assert(t.coder.vendor == "claude", "coder.vendor=" .. tostring(t.coder.vendor))
        \\    assert(t.coder.model == "claude-sonnet-4-6", "coder.model=" .. tostring(t.coder.model))
        \\    assert(t.reviewer.vendor == "claude", "reviewer.vendor=" .. tostring(t.reviewer.vendor))
        \\    assert(t.reviewer.model == "claude-opus-4-8", "reviewer.model=" .. tostring(t.reviewer.model))
        \\    assert(t["test-coder"].model == "claude-sonnet-4-6", "tc=" .. tostring(t["test-coder"].model))
        \\    assert(t.documenter.model == "claude-sonnet-4-6", "doc=" .. tostring(t.documenter.model))
        \\  end,
        \\}
    );
    const wf_path = try iso.workflowPath("dt.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", wf_path });
    defer res.deinit();
    if (res.exitCode() != 0) {
        std.debug.print("\ndispatch_table workflow failed:\nstderr:\n{s}\n", .{res.stderr});
        return error.TestUnexpectedResult;
    }
}
test "planar-execute: per-spawn dispatch banner names role + model on stderr (task 3708)" {
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    // --mock-worker drives the FULL agent() pipeline (no real claude -p), so the
    // banner emitted at spawn time is observable on stderr.
    try iso.writeWorkflow("ban.lua",
        \\return {
        \\  meta = { name = "ban", description = "d", phases = {} },
        \\  run = function(ctx)
        \\    ctx.agent("the brief", {
        \\      role = "coder",
        \\      worktree_path = "/tmp/ban-wt",
        \\      claim_token = "tk",
        \\      task_slug = "ts",
        \\    })
        \\  end,
        \\}
    );
    const wf_path = try iso.workflowPath("ban.lua");
    defer gpa.free(wf_path);

    const res = try iso.run(&.{ "run", "--mock-worker", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    if (std.mem.indexOf(u8, res.stderr, "[dispatch]") == null or
        std.mem.indexOf(u8, res.stderr, "vendor=claude") == null or
        std.mem.indexOf(u8, res.stderr, "role=coder") == null or
        std.mem.indexOf(u8, res.stderr, "model=claude-sonnet-4-6") == null)
    {
        std.debug.print("\ndispatch banner missing/incomplete on stderr:\n{s}\n", .{res.stderr});
        return error.TestUnexpectedResult;
    }
}

// ---------------------------------------------------------------------------
// plan 540 task 3630 — cross-binary: execute consumes `planar models routing`
// ---------------------------------------------------------------------------

/// Resolve the `planar` binary path from PLANAR_BIN (set by make test-integration).
fn resolvePlanarBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_BIN=")) return s["PLANAR_BIN=".len..];
    }
    @panic("PLANAR_BIN not set — run via: make test-integration");
}

test "planar-execute consumes `planar models routing`: [role_vendors] coder=codex → codex dispatch (plan 540 task 3630)" {
    const gpa = std.testing.allocator;
    var iso = try Iso.init(gpa);
    defer iso.deinit();

    // Write a real config (at the path Iso injects as PLANAR_CONFIG_PATH) that
    // routes the coder to codex. execute will shell `planar models routing`,
    // which reads this config via the shared resolver.
    {
        var f = try iso.tmp.dir.createFile(std.testing.io, "home/config.toml", .{});
        defer f.close(std.testing.io);
        try f.writeStreamingAll(std.testing.io,
            \\[role_vendors]
            \\coder = "codex"
        );
    }
    try iso.writeWorkflow("x540.lua",
        \\return { meta = { name = "x540", description = "d", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try iso.workflowPath("x540.lua");
    defer gpa.free(wf_path);

    // Build the isolated env, then put `planar` on PATH so execute can shell
    // `planar models routing`.
    var env_map = try iso.buildEnv(gpa);
    defer env_map.deinit();
    const planar_dir = std.fs.path.dirname(resolvePlanarBin()) orelse ".";
    const old_path = env_map.get("PATH") orelse "/usr/bin:/bin";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for ([_][]const u8{ "run", "--dry-run", wf_path }) |a| try argv.append(gpa, a);

    const res = try std.process.run(gpa, std.testing.io, .{ .argv = argv.items, .environ_map = &env_map });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    const exit_code: u32 = switch (res.term) {
        .exited => |c| c,
        else => 255,
    };
    try std.testing.expectEqual(@as(u32, 0), exit_code);

    // End-to-end: the coder row must route to codex / gpt-5.4 (config →
    // planar models routing → execute dispatch table). reviewer stays claude.
    const coder = rowCell(res.stdout, "coder") orelse {
        std.debug.print("\nno coder row in dispatch table:\n{s}\n", .{res.stdout});
        return error.TestUnexpectedResult;
    };
    try std.testing.expectEqualStrings("codex", coder.vendor);
    try std.testing.expectEqualStrings("gpt-5.4", coder.model);
    const reviewer = rowCell(res.stdout, "reviewer") orelse unreachable;
    try std.testing.expectEqualStrings("claude", reviewer.vendor);
}
