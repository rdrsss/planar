//! integration_tests/opaque_model_string_test.zig — the `--model` flag on
//! `planar-agent claim` records the model actually used as an OPAQUE STRING.
//!
//! Per armarium decision 884: Planar is told which vendor and model were used
//! and records them verbatim. It does not decide, validate, or publish what is
//! "supported" — Armarium holds the catalog. These tests pin that contract, so
//! a future change that adds a membership check fails loudly here.
//!
//! Covered scenarios:
//!
//!   - Scenario A: an ID Planar has never heard of is stored verbatim.
//!   - Scenario B: --model omitted leaves the column null; reporting is
//!     optional, not required.
//!   - Scenario C: a vendor/model pair that disagrees is ACCEPTED, not
//!     rejected. Catching a cross-host mistake is Armarium's spawn
//!     verification, not Planar's job.
//!   - Scenario D: punctuation and mixed case survive unchanged — no
//!     normalization, no lowercasing, no aliasing.

const std = @import("std");
const harness = @import("harness");

/// Resolve the planar-agent binary from PLANAR_AGENT_BIN env var.
fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_AGENT_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

/// Run planar-agent with the suite's PLANAR_DB injected.
fn runAgent(suite: *const harness.Suite, args: []const []const u8) harness.Suite.RunResult {
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
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

/// Assert exit 0, return stdout (caller owns).
fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-agent failed (term={any}): {s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

/// Seed an init + plan, return the plan id string (caller frees).
fn seedPlan(suite: *const harness.Suite, slug: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    return std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
}

/// Extract the first integer value for `key` from a JSON string.
fn extractIntField(s: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, s, key) orelse return null;
    var i = idx + key.len;
    while (i < s.len and (s[i] == ' ' or s[i] == ':' or s[i] == '\t')) i += 1;
    var end = i;
    while (end < s.len and s[end] >= '0' and s[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, s[i..end], 10) catch null;
}

/// Return true if `haystack` contains `needle`.
fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

// =========================================================================
// Scenario A — an unknown ID is stored verbatim
// =========================================================================

test "scenario A: claim --model stores an unrecognized id verbatim" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "om-verbatim");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Opaque model task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const entity = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(entity);

    const out = mustRunAgent(&suite, &.{
        "claim",               "--entity", entity,
        "--role",              "coder",    "--vendor",
        "codex",               "--model",  "some-model-planar-has-never-heard-of-v7",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(out);

    try std.testing.expect(contains(out, "\"model\":\"some-model-planar-has-never-heard-of-v7\""));
}

// =========================================================================
// Scenario B — omitted --model leaves null
// =========================================================================

test "scenario B: claim without --model leaves the column null" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "om-null");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "No model task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const entity = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(entity);

    const out = mustRunAgent(&suite, &.{
        "claim",               "--entity", entity, "--role", "coder",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(out);

    try std.testing.expect(contains(out, "\"model\":null"));
}

// =========================================================================
// Scenario C — a disagreeing vendor/model pair is accepted, not rejected
// =========================================================================

test "scenario C: a cross-vendor model is recorded, not rejected" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "om-cross");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Cross vendor task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const entity = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(entity);

    // Deliberately incoherent: a Claude model reported on a Codex vendor.
    // Planar must record what it is told. Detecting that this is wrong is
    // Armarium's spawn verification, not a Planar membership check.
    const out = mustRunAgent(&suite, &.{
        "claim",               "--entity", entity,
        "--role",              "coder",    "--vendor",
        "codex",               "--model",  "claude-opus-5",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(out);

    try std.testing.expect(contains(out, "\"vendor\":\"codex\""));
    try std.testing.expect(contains(out, "\"model\":\"claude-opus-5\""));
}

// =========================================================================
// Scenario D — no normalization
// =========================================================================

test "scenario D: punctuation and case survive unchanged" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const plan_arg = seedPlan(&suite, "om-raw");
    defer gpa.free(plan_arg);

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_arg, "--json", "Raw string task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const entity = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(entity);

    const out = mustRunAgent(&suite, &.{
        "claim",               "--entity", entity,
        "--role",              "coder",    "--vendor",
        "claude",              "--model",  "Vendor.Model_9000-RC2",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(out);

    try std.testing.expect(contains(out, "\"model\":\"Vendor.Model_9000-RC2\""));
}
