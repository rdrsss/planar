//! Integration tests for `planar-execute` (plan 633 P0.2c).
//!
//! Black-box exercises against the compiled `planar-execute` binary — the
//! deterministic, spawn-free Lua workflow engine. These tests prove the engine
//! works end-to-end:
//!
//!   - A trivial workflow whose phase calls `cli.planar_json(...)` and
//!     `flow.result({...})` runs through the sandbox, shells the allowlisted
//!     `planar` binary, parses its JSON, and emits the result on stdout.
//!   - A realistic workflow shells `planar plan show <id> --json` against a
//!     harness-seeded plan and reflects a field of it back through flow.result.
//!   - The sandbox refuses host-reach escape hatches (os/io are nil).
//!   - A bad --phase exits non-zero with a clear diagnostic.
//!
//! The engine reaches Planar state ONLY by shelling allowlisted CLI verbs (the
//! D7 host surface). To make the inner `planar` shell hit the SAME isolated DB
//! the harness uses, these tests construct a child env with PLANAR_DB set and
//! prepend the dir holding the harness `planar` binary to PATH (the engine
//! shells the bare name `planar`, resolved via PATH).

const std = @import("std");
const harness = @import("harness");

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

/// runExecute spawns `planar-execute <args...>` with cwd = `cwd`, PLANAR_DB
/// pointed at `db_path`, and PATH prefixed with the dir holding the harness
/// `planar` binary so the engine's inner `cli.planar` shell resolves to it.
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

    // Build the child env from the current environ, then override PLANAR_DB and
    // PATH so the engine's inner `planar` shell hits the harness binary + DB.
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

fn writeWorkflow(suite: *harness.Suite, name: []const u8, body: []const u8) ![]const u8 {
    const root = suite.tmpAbsPath();
    const path = try std.fs.path.join(suite.allocator, &.{ root, name });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = body });
    return path;
}

test "planar-execute runs a trivial deterministic workflow shelling cli.planar_json" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("execwf");
    suite.addAssoc("execwf", null);

    // A trivial workflow: shell `planar schema --json`-equivalent (the schema
    // verb emits JSON with no DB dependency), confirm a known field, and emit a
    // result table. Proves the full engine path: sandbox → host registry →
    // cli.planar_json shell → JSON parse → flow.result → JSON stdout.
    const wf =
        \\function setup()
        \\  flow.phase("setup")
        \\  local sch = cli.planar_json({"schema"})
        \\  flow.log("schemaVersion=" .. tostring(sch.schemaVersion))
        \\  flow.result({ ok = true, root = sch.root, version = sch.schemaVersion })
        \\end
    ;
    const path = try writeWorkflow(&suite, "trivial.lua", wf);
    defer gpa.free(path);

    const res = try runExecute(gpa, root, suite.absDbPath(), &.{ "run", path, "--phase", "setup" });
    defer res.deinit();

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // stdout is the flow.result payload as JSON.
    const Parsed = struct { ok: bool, root: []const u8, version: i64 };
    const parsed = try std.json.parseFromSlice(Parsed, gpa, std.mem.trim(u8, res.stdout, " \t\r\n"), .{ .ignore_unknown_fields = true });
    defer parsed.deinit();
    try std.testing.expect(parsed.value.ok);
    try std.testing.expectEqualStrings("planar", parsed.value.root);
    try std.testing.expectEqual(@as(i64, 1), parsed.value.version);
}

test "planar-execute shells planar plan show against a seeded plan" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("execplan");
    suite.addAssoc("execplan", null);

    // Seed a real plan through the harness, then reflect its id back through a
    // workflow that shells `planar plan show <id> --json`.
    const plan_json = suite.mustRun(&.{ "plan", "create", "Engine smoke plan", "--json" });
    defer gpa.free(plan_json);
    const PlanAdd = struct { id: u64 };
    const pa = try std.json.parseFromSlice(PlanAdd, gpa, plan_json, .{ .ignore_unknown_fields = true });
    defer pa.deinit();
    const plan_id = pa.value.id;

    const wf =
        \\function run()
        \\  local p = cli.planar_json({"plan", "show", tostring(ctx.args.plan_id), "--json"})
        \\  flow.result({ plan_id = p.id, title = p.title })
        \\end
    ;
    const path = try writeWorkflow(&suite, "planshow.lua", wf);
    defer gpa.free(path);

    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{d}}}", .{plan_id});
    defer gpa.free(args_json);

    const res = try runExecute(gpa, root, suite.absDbPath(), &.{ "run", path, "--phase", "run", "--args", args_json });
    defer res.deinit();

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    const Reflected = struct { plan_id: u64, title: []const u8 };
    const r = try std.json.parseFromSlice(Reflected, gpa, std.mem.trim(u8, res.stdout, " \t\r\n"), .{ .ignore_unknown_fields = true });
    defer r.deinit();
    try std.testing.expectEqual(plan_id, r.value.plan_id);
    try std.testing.expectEqualStrings("Engine smoke plan", r.value.title);
}

test "planar-execute sandbox nils os and io" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("execsbx");
    suite.addAssoc("execsbx", null);

    // The workflow asserts os and io are nil (the sandbox stripped them). If the
    // assert fails the phase errors and the engine exits non-zero.
    const wf =
        \\function probe()
        \\  assert(os == nil, "os should be nil")
        \\  assert(io == nil, "io should be nil")
        \\  assert(load == nil, "load should be nil")
        \\  assert(require == nil, "require should be nil")
        \\  assert(math.random == nil, "math.random should be nil")
        \\  flow.result({ sandboxed = true })
        \\end
    ;
    const path = try writeWorkflow(&suite, "sandbox.lua", wf);
    defer gpa.free(path);

    const res = try runExecute(gpa, root, suite.absDbPath(), &.{ "run", path, "--phase", "probe" });
    defer res.deinit();

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"sandboxed\":true") != null);
}

test "planar-execute exits non-zero on a missing phase" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("execmiss");
    suite.addAssoc("execmiss", null);

    const wf =
        \\function setup() flow.result({}) end
    ;
    const path = try writeWorkflow(&suite, "missing.lua", wf);
    defer gpa.free(path);

    const res = try runExecute(gpa, root, suite.absDbPath(), &.{ "run", path, "--phase", "nonexistent" });
    defer res.deinit();

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "phase function not found") != null);
}
