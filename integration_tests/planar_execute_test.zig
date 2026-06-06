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

/// buildEnvWithoutGate builds an explicit subprocess environment that strips
/// PLANAR_EXECUTE_LIVE_AGENT from the host environ. This makes the non-gated
/// tests immune to the operator running `PLANAR_EXECUTE_LIVE_AGENT=1 make
/// test-integration` — the gate must be injected explicitly via runExecuteWithGate.
fn buildEnvWithoutGate(gpa: std.mem.Allocator) !std.process.Environ.Map {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count]) |_| : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const host_environ: std.process.Environ = .{ .block = posix_block };
    var env_map = try host_environ.createMap(gpa);
    _ = env_map.swapRemove("PLANAR_EXECUTE_LIVE_AGENT");
    return env_map;
}

fn runExecute(gpa: std.mem.Allocator, args: []const []const u8) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

    // Strip PLANAR_EXECUTE_LIVE_AGENT so non-gated tests are immune to the
    // operator running `PLANAR_EXECUTE_LIVE_AGENT=1 make test-integration`.
    var env_map = try buildEnvWithoutGate(gpa);
    defer env_map.deinit();

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

fn tmpAbsPath(tmp: *std.testing.TmpDir, gpa: std.mem.Allocator) ![]u8 {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const len = try tmp.dir.realPath(std.testing.io, &buf);
    return gpa.dupe(u8, buf[0..len]);
}

fn writeWorkflow(tmp: *std.testing.TmpDir, name: []const u8, content: []const u8) !void {
    var f = try tmp.dir.createFile(std.testing.io, name, .{});
    defer f.close(std.testing.io);
    try f.writeStreamingAll(std.testing.io, content);
}

fn workflowPath(tmp_abs: []const u8, name: []const u8, gpa: std.mem.Allocator) ![]u8 {
    return std.fs.path.join(gpa, &.{ tmp_abs, name });
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "planar-execute version prints a version line and exits 0" {
    const gpa = std.testing.allocator;

    const res = try runExecute(gpa, &.{"version"});
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "trivial.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "trivial.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute: trailing args reach ctx.args as 1-based sequence" {
    // Verify the CLI arg-threading contract: [args…] after the workflow path
    // are threaded into run(ctx) as ctx.args[1], ctx.args[2], ...
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "args_check.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "args_check.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ wf_path, "alpha", "beta" });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute: missing file exits non-zero with message" {
    // A file path that does not exist must produce a non-zero exit and a
    // human-readable message on stderr.
    const gpa = std.testing.allocator;

    const res = try runExecute(gpa, &.{"/no/such/planar-execute-test-file.lua"});
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return {
        \\  meta = { name = "boom", description = "errors", phases = {} },
        \\  run = function(ctx)
        \\    error("intentional test error")
        \\  end,
        \\}
    ;
    try writeWorkflow(&tmp, "boom.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "boom.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "intentional test error") != null);
}

test "planar-execute: compile error exits 3 with message" {
    // A syntactically invalid workflow must exit with code 3 (compile error).
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "bad_syntax.lua", "this is not valid lua @@@@\n");
    const wf_path = try workflowPath(tmp_abs, "bad_syntax.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 3), res.exitCode());
    try std.testing.expect(res.stderr.len > 0);
}

test "planar-execute: non-table return exits 2 (invalid module shape)" {
    // A workflow that returns a number instead of a table must exit with
    // code 2 (invalid module structure).
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "not_table.lua", "return 42\n");
    const wf_path = try workflowPath(tmp_abs, "not_table.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 2), res.exitCode());
}

test "planar-execute: help flag exits 0" {
    // `planar-execute --help` must exit 0 and produce help text.
    const gpa = std.testing.allocator;

    const res = try runExecute(gpa, &.{"--help"});
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "preview.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "preview.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--dry-run", wf_path });
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "error_run.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "error_run.lua", gpa);
    defer gpa.free(wf_path);

    // ARM 1: --dry-run → exit 0, run never entered.
    const dry = try runExecute(gpa, &.{ "run", "--dry-run", wf_path });
    defer dry.deinit();
    try std.testing.expectEqual(@as(u32, 0), dry.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, dry.stdout, "error-in-run") != null);

    // ARM 2: no --dry-run → exit non-zero, error message on stderr.
    const live = try runExecute(gpa, &.{wf_path});
    defer live.deinit();
    try std.testing.expect(live.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, live.stderr, "must not run under dry-run") != null);
}

test "planar-execute --dry-run: malformed module still exits non-zero (load/validate shared)" {
    // --dry-run does NOT bypass load+validate. A workflow that returns a
    // non-table must still exit with code 2 under --dry-run.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "not_table_dr.lua", "return 42\n");
    const wf_path = try workflowPath(tmp_abs, "not_table_dr.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--dry-run", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 2), res.exitCode());
}

test "planar-execute --dry-run: compile error still exits 3 (load/validate shared)" {
    // A syntactically invalid workflow must exit 3 under --dry-run, same as
    // without the flag — the load+validate path is shared.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "bad_syntax_dr.lua", "this is not valid lua @@@@\n");
    const wf_path = try workflowPath(tmp_abs, "bad_syntax_dr.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--dry-run", wf_path });
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "host_fns.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "host_fns.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute sandbox: os.execute call fails (absent) (task 3169)" {
    // os is never opened → os is nil → os.execute is an index-on-nil runtime
    // error. The workflow must exit non-zero.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return {
        \\  meta = { name = "os-exec", description = "must fail", phases = {} },
        \\  run = function(ctx) os.execute("echo pwned") end,
        \\}
    ;
    try writeWorkflow(&tmp, "os_exec.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "os_exec.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "sandbox.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "sandbox.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return {
        \\  meta = { name = "no-debug-pkg", description = "absent", phases = {} },
        \\  run = function(ctx)
        \\    assert(debug == nil, "debug must be absent (task 3232)")
        \\    assert(package == nil, "package must be absent (task 3232)")
        \\  end,
        \\}
    ;
    try writeWorkflow(&tmp, "no_debug_pkg.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "no_debug_pkg.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "dump_no_load.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "dump_no_load.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
}

test "planar-execute --dry-run: host-fn workflow still never enters run (task 3168/3169)" {
    // The dry-run guarantee is unchanged by the host surface: a workflow whose
    // run body would error must still exit 0 under --dry-run (run not entered),
    // and exit non-zero without it (run entered, error raised).
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "dry_host.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "dry_host.lua", gpa);
    defer gpa.free(wf_path);

    // ARM 1: --dry-run → exit 0, run never entered.
    const dry = try runExecute(gpa, &.{ "run", "--dry-run", wf_path });
    defer dry.deinit();
    try std.testing.expectEqual(@as(u32, 0), dry.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, dry.stdout, "dry-host") != null);

    // ARM 2: live run → exit non-zero, error surfaced.
    const live = try runExecute(gpa, &.{wf_path});
    defer live.deinit();
    try std.testing.expect(live.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, live.stderr, "must not execute under --dry-run") != null);
}

// ---------------------------------------------------------------------------
// task 3264 — agent-free workflows must not require --plan under the live gate
// ---------------------------------------------------------------------------

/// runExecuteWithGate runs the planar-execute binary with PLANAR_EXECUTE_LIVE_AGENT=1
/// injected into the subprocess environment. Used to exercise the gated
/// driver-attachment path without spawning a real claude worker.
fn runExecuteWithGate(gpa: std.mem.Allocator, args: []const []const u8) !RunResult {
    // Build the subprocess env from the host environ + inject the gate var.
    var env_map = try buildEnvWithoutGate(gpa);
    defer env_map.deinit();
    env_map.put("PLANAR_EXECUTE_LIVE_AGENT", "1") catch @panic("OOM injecting gate var");

    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

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

test "planar-execute: agent-free workflow runs cleanly under PLANAR_EXECUTE_LIVE_AGENT=1 without --plan (task 3264)" {
    // Regression guard for task 3264: a pure-Lua workflow that never calls
    // agent() must exit 0 when PLANAR_EXECUTE_LIVE_AGENT=1 is set, even
    // without --plan. Before the fix, the gate block eagerly required --plan
    // and the process exited 1 before the workflow ran.
    //
    // This test injects PLANAR_EXECUTE_LIVE_AGENT=1 for this one invocation
    // only (via runExecuteWithGate) so it runs in CI without a real claude spawn.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "agent_free_gated.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "agent_free_gated.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithGate(gpa, &.{wf_path});
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
    const res = try runExecute(gpa, &.{ "run", "--help" });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "--mock-worker") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "FakeSpawner") != null);
}

test "planar-execute --mock-worker conflict: --dry-run AND --mock-worker exits non-zero (task 3202)" {
    // The two modes are mutually exclusive. Combining them is a wiring error
    // and must surface a clear, distinct message — operator picks one.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    ;
    try writeWorkflow(&tmp, "x.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "x.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--mock-worker", "--dry-run", wf_path });
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    ;
    try writeWorkflow(&tmp, "x.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "x.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithGate(gpa, &.{ "run", "--mock-worker", wf_path });
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "mock_single.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "mock_single.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--mock-worker", wf_path });
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

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
    try writeWorkflow(&tmp, "mock_parallel.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "mock_parallel.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--mock-worker", wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
}

test "planar-execute --mock-worker: ungated/no-driver path is unchanged when --mock-worker is NOT set (task 3202)" {
    // Regression guard: dropping --mock-worker must keep the default M2 stub
    // behavior. agent() returns { status = "stub" } when the gate is off AND
    // --mock-worker is off. The MOCK MODE notice must NOT appear.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    const wf_src =
        \\return {
        \\  meta = { name = "no-mock", description = "default stub", phases = {} },
        \\  run = function(ctx)
        \\    local r = ctx.agent("b", { role = "coder" })
        \\    assert(r.status == "stub", "expected stub when --mock-worker is off, got " .. tostring(r.status))
        \\  end,
        \\}
    ;
    try writeWorkflow(&tmp, "no_mock.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "no_mock.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{wf_path});
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
    const res = try runExecute(gpa, &.{ "run", "--help" });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "--mock-outcomes") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "NDJSON") != null);
}

test "planar-execute --mock-outcomes: 3-call scripted outcomes observed in order (task 3491)" {
    // The primary delivery: three scripted outcomes consumed in order.
    // The workflow calls agent() three times and asserts the per-call result.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    // Write the NDJSON outcomes file.
    const outcomes_src =
        \\{"exit_code":0,"stdout":"call-1-out","stderr":""}
        \\{"exit_code":1,"stdout":"","stderr":"call-2-err"}
        \\{"exit_code":0,"stdout":"call-3-out","stderr":""}
        \\
    ;
    try writeWorkflow(&tmp, "outcomes.ndjson", outcomes_src);
    const outcomes_path = try workflowPath(tmp_abs, "outcomes.ndjson", gpa);
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
    try writeWorkflow(&tmp, "outcomes_3.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "outcomes_3.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--mock-outcomes", outcomes_path, wf_path });
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
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    // One scripted outcome, but we make two calls.
    try writeWorkflow(&tmp, "one.ndjson",
        \\{"exit_code":0,"stdout":"scripted","stderr":""}
        \\
    );
    const outcomes_path = try workflowPath(tmp_abs, "one.ndjson", gpa);
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
    try writeWorkflow(&tmp, "fallback.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "fallback.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
}

test "planar-execute --mock-outcomes: malformed NDJSON file → startup error (task 3491)" {
    // A file with a non-JSON line must fail at startup (non-zero exit, clear
    // message) BEFORE any Lua code runs. Workflow correctness is irrelevant.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "bad.ndjson", "this is not json\n");
    const outcomes_path = try workflowPath(tmp_abs, "bad.ndjson", gpa);
    defer gpa.free(outcomes_path);

    try writeWorkflow(&tmp, "any.lua",
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try workflowPath(tmp_abs, "any.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mock-outcomes") != null);
}

test "planar-execute --mock-outcomes: missing file → startup error (task 3491)" {
    // A path that does not exist must fail at startup with a clear message.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "any.lua",
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try workflowPath(tmp_abs, "any.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--mock-outcomes", "/no/such/outcomes.ndjson", wf_path });
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mock-outcomes") != null);
}

test "planar-execute --mock-outcomes: implies --mock-worker (no need to pass both) (task 3491)" {
    // Passing only --mock-outcomes (without --mock-worker) must enter MOCK MODE.
    // This is the "implies" contract: less friction for workflow authors.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "one.ndjson",
        \\{"exit_code":0,"stdout":"implied","stderr":""}
        \\
    );
    const outcomes_path = try workflowPath(tmp_abs, "one.ndjson", gpa);
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
    try writeWorkflow(&tmp, "implies.lua", wf_src);
    const wf_path = try workflowPath(tmp_abs, "implies.lua", gpa);
    defer gpa.free(wf_path);

    // Pass ONLY --mock-outcomes, NOT --mock-worker.
    const res = try runExecute(gpa, &.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
}

test "planar-execute --mock-outcomes conflict: --dry-run exits non-zero (task 3491)" {
    // --mock-outcomes is mutually exclusive with --dry-run.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "x.ndjson", "{}\n");
    const outcomes_path = try workflowPath(tmp_abs, "x.ndjson", gpa);
    defer gpa.free(outcomes_path);

    try writeWorkflow(&tmp, "x.lua",
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try workflowPath(tmp_abs, "x.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecute(gpa, &.{ "run", "--mock-outcomes", outcomes_path, "--dry-run", wf_path });
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mock-outcomes") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "--dry-run") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mutually exclusive") != null);
}

test "planar-execute --mock-outcomes conflict: PLANAR_EXECUTE_LIVE_AGENT exits non-zero (task 3491)" {
    // --mock-outcomes is mutually exclusive with the live gate.
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const tmp_abs = try tmpAbsPath(&tmp, gpa);
    defer gpa.free(tmp_abs);

    try writeWorkflow(&tmp, "x.ndjson", "{}\n");
    const outcomes_path = try workflowPath(tmp_abs, "x.ndjson", gpa);
    defer gpa.free(outcomes_path);

    try writeWorkflow(&tmp, "x.lua",
        \\return { meta = { name = "x", description = "x", phases = {} }, run = function(ctx) end }
    );
    const wf_path = try workflowPath(tmp_abs, "x.lua", gpa);
    defer gpa.free(wf_path);

    const res = try runExecuteWithGate(gpa, &.{ "run", "--mock-outcomes", outcomes_path, wf_path });
    defer res.deinit();

    try std.testing.expect(res.exitCode() != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mock-outcomes") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "PLANAR_EXECUTE_LIVE_AGENT") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "mutually exclusive") != null);
}
