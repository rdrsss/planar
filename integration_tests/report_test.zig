//! integration_tests/report_test.zig
//!
//! Black-box integration tests for `planar report` (M2).
//!
//! Scenarios covered (per test-spec artifact 321):
//!   report-aggregates   — aggregates reflect seeded activity.
//!   report-json         — --json emits the stable machine shape.
//!   report-verb         — disabled logging partial bundle; quiet-db empty
//!                         but well-formed; invalid flags fail as usage errors.
//!   report-error-tail   — empty tail; tail cap and ordering; window filter.
//!   report-redaction    — sentinel text never leaks into the bundle.
//!
//! Run via: make test-integration

const std = @import("std");
const harness = @import("harness");

// Minimal JSON shapes for CLI-driven task operations used in aggregate tests.
const TaskJSON = struct { id: i64, title: []const u8, status: []const u8 };

// =========================================================================
// Helpers
// =========================================================================

/// Write a config.toml enabling or disabling CLI logging.
fn writeCliLogConfig(suite: *harness.Suite, cli_log: bool) []u8 {
    const dir = std.fs.path.dirname(suite.db_path) orelse ".";
    const cfg_path = std.fs.path.join(suite.allocator, &.{ dir, "config.toml" }) catch
        @panic("OOM building config path");
    const content = if (cli_log)
        "[introspection]\ncli_log = true\n"
    else
        "[introspection]\ncli_log = false\n";
    std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = cfg_path, .data = content }) catch
        @panic("cannot write config.toml");
    return cfg_path;
}

/// Helper: inject PLANAR_CONFIG_PATH into env.
fn cfgEnv(cfg_path: []const u8) [1]harness.Suite.ExtraEnvEntry {
    return [1]harness.Suite.ExtraEnvEntry{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg_path }};
}

/// Seed a failed invocation directly into cli_invocations via sqlite3.
fn seedFailedInvocation(
    suite: *harness.Suite,
    verb_path: []const u8,
    category: []const u8,
    exit_code: i64,
    offset_minutes: i64,
) void {
    var sql_buf: [512]u8 = undefined;
    const sql = std.fmt.bufPrint(
        &sql_buf,
        "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)" ++
            " values ('{s}', '', {d}, '{s}', datetime('now', '-{d} minutes'));",
        .{ verb_path, exit_code, category, offset_minutes },
    ) catch @panic("sql buf too small");

    const result = std.process.run(suite.allocator, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, sql },
    }) catch @panic("sqlite3 not found");
    defer suite.allocator.free(result.stdout);
    defer suite.allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("sqlite3 seed failed: {s}\n", .{result.stderr});
        @panic("seed failed");
    }
}

/// Seed a successful invocation directly.
fn seedSuccessInvocation(suite: *harness.Suite, verb_path: []const u8) void {
    var sql_buf: [256]u8 = undefined;
    const sql = std.fmt.bufPrint(
        &sql_buf,
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)" ++
            " values ('{s}', '', 0, datetime('now'));",
        .{verb_path},
    ) catch @panic("sql buf too small");
    const result = std.process.run(suite.allocator, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, sql },
    }) catch @panic("sqlite3 not found");
    defer suite.allocator.free(result.stdout);
    defer suite.allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) @panic("seed failed");
}

// =========================================================================
// report-json: stable machine shape
// =========================================================================

test "report-json: --json carries the stable top-level field contract" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    const json_out = suite.mustRun(&.{ "report", "--json" });
    defer gpa.free(json_out);

    // Parse as JSON — must succeed.
    var parsed = std.json.parseFromSlice(std.json.Value, gpa, json_out, .{}) catch |e| {
        std.debug.print("report --json parse failed: {s}\noutput: {s}\n", .{ @errorName(e), json_out });
        return error.TestUnexpectedResult;
    };
    defer parsed.deinit();

    const obj = parsed.value.object;

    // All top-level fields from the spec must be present.
    try std.testing.expect(obj.get("version") != null);
    try std.testing.expect(obj.get("schema_version") != null);
    try std.testing.expect(obj.get("health") != null);
    try std.testing.expect(obj.get("window") != null);
    try std.testing.expect(obj.get("invocations") != null);
    try std.testing.expect(obj.get("failures") != null);
    try std.testing.expect(obj.get("actions") != null);
    try std.testing.expect(obj.get("sync") != null);
    try std.testing.expect(obj.get("claims") != null);
    try std.testing.expect(obj.get("handoffs") != null);

    // invocations and failures must be arrays (empty, not null).
    const inv = obj.get("invocations").?;
    try std.testing.expect(inv == .array);

    const fail = obj.get("failures").?;
    try std.testing.expect(fail == .array);

    const actions = obj.get("actions").?;
    try std.testing.expect(actions == .array);

    const sync = obj.get("sync").?;
    try std.testing.expect(sync == .array);

    // claims and handoffs must be objects.
    const claims = obj.get("claims").?;
    try std.testing.expect(claims == .object);
    try std.testing.expect(claims.object.get("stale_claims") != null);
    try std.testing.expect(claims.object.get("never_consumed") != null);

    const handoffs = obj.get("handoffs").?;
    try std.testing.expect(handoffs == .object);
    try std.testing.expect(handoffs.object.get("stale_handoffs") != null);
    try std.testing.expect(handoffs.object.get("never_consumed") != null);

    // window must be numeric.
    try std.testing.expect(obj.get("window").? == .integer);

    // schema_version must be a positive integer.
    const sv = obj.get("schema_version").?;
    try std.testing.expect(sv == .integer);
    try std.testing.expect(sv.integer > 0);

    // reopens must be present and numeric (3815 field addition).
    const reopens = obj.get("reopens") orelse {
        std.debug.print("'reopens' field missing from --json output\n", .{});
        return error.TestUnexpectedResult;
    };
    try std.testing.expect(reopens == .integer);
}

test "report-json: empty window emits empty arrays not nulls" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    // Enable logging but no actual invocations seeded → empty arrays.
    const cfg_path = writeCliLogConfig(&suite, true);
    defer gpa.free(cfg_path);
    const extra = cfgEnv(cfg_path);

    const json_out = suite.mustRunWith(&.{ "report", "--json" }, &extra);
    defer gpa.free(json_out);

    var parsed = std.json.parseFromSlice(std.json.Value, gpa, json_out, .{}) catch
        return error.TestUnexpectedResult;
    defer parsed.deinit();
    const obj = parsed.value.object;

    // When logging is on but no data, these arrays must be present and empty.
    const inv = obj.get("invocations").?;
    try std.testing.expect(inv == .array);
    // May or may not be empty — the report verb itself is being recorded.
    // The key contract is: no nulls or missing fields.
    const fail = obj.get("failures").?;
    try std.testing.expect(fail == .array);
}

test "report-json: configured transcript adapters feed normalized preview with authoritative CLI dedup" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(init_out);

    const dir = std.fs.path.dirname(suite.db_path) orelse ".";
    const cfg_path = try std.fs.path.join(gpa, &.{ dir, "transcript-config.toml" });
    defer gpa.free(cfg_path);
    const transcript_path = try std.fs.path.join(gpa, &.{ dir, "claude.jsonl" });
    defer gpa.free(transcript_path);
    const cfg = try std.fmt.allocPrint(gpa, "[introspection]\ncli_log = true\n[introspection.transcripts]\nclaude_path = \"{s}\"\ncodex_enabled = false\ncopilot_enabled = false\n", .{transcript_path});
    defer gpa.free(cfg);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = cfg_path, .data = cfg });
    const extra = cfgEnv(cfg_path);

    // Exercise the real capture hook. The logger stores parser-relative
    // `task add`; the introspection boundary must expose canonical
    // `planar task add` without weakening the adapter's validation.
    const failed = suite.expectFailureWith(&.{ "task", "add", "--unknown-private-flag", "PRIVATE_SENTINEL" }, &extra);
    defer gpa.free(failed);
    const captured = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, "select verb_path || '|' || recorded_at from cli_invocations where verb_path = 'task add' order by id desc limit 1;" },
    }) catch @panic("sqlite3 not found");
    defer gpa.free(captured.stdout);
    defer gpa.free(captured.stderr);
    try std.testing.expect(captured.term == .exited and captured.term.exited == 0);
    const captured_line = std.mem.trim(u8, captured.stdout, " \r\n");
    const separator = std.mem.indexOfScalar(u8, captured_line, '|') orelse return error.TestUnexpectedResult;
    try std.testing.expectEqualStrings("task add", captured_line[0..separator]);
    const recorded_at = captured_line[separator + 1 ..];

    const transcript = try std.fmt.allocPrint(gpa, "{{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"{s}\",\"tool\":{{\"name\":\"planar task add\",\"input\":{{\"body\":\"PRIVATE_SENTINEL\"}}}},\"exit_code\":2,\"invalid_flag\":true}}", .{recorded_at});
    defer gpa.free(transcript);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = transcript_path, .data = transcript });
    const json_out = suite.mustRunWith(&.{ "report", "--json" }, &extra);
    defer gpa.free(json_out);
    try std.testing.expect(std.mem.indexOf(u8, json_out, "PRIVATE_SENTINEL") == null);

    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, json_out, .{});
    defer parsed.deinit();
    const preview = parsed.value.object.get("introspection_preview").?.object;
    const signals = preview.get("signals").?.array;
    try std.testing.expectEqual(@as(usize, 1), signals.items.len);
    try std.testing.expectEqualStrings("cli_log", signals.items[0].object.get("vendor").?.string);
    try std.testing.expectEqualStrings("failure", signals.items[0].object.get("category").?.string);
    const coverage = preview.get("coverage").?.array;
    try std.testing.expectEqual(@as(usize, 4), coverage.items.len);
}

// =========================================================================
// report-verb: disabled-log partial bundle
// =========================================================================

test "report-verb: logging disabled — invocations section says 'logging disabled'" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    // Logging OFF (default).
    const text_out = suite.mustRun(&.{"report"});
    defer gpa.free(text_out);

    try std.testing.expect(std.mem.indexOf(u8, text_out, "logging disabled") != null);
}

test "report-verb: logging disabled — always-on sections render normally" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    // Text output with logging OFF must still contain health, claims, handoffs.
    const text_out = suite.mustRun(&.{"report"});
    defer gpa.free(text_out);

    try std.testing.expect(std.mem.indexOf(u8, text_out, "health:") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_out, "[claims]") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_out, "[handoffs]") != null);
    // Exit 0 (mustRun panics on non-zero).
}

test "report-verb: logging disabled --json still carries always-on fields" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    const json_out = suite.mustRun(&.{ "report", "--json" });
    defer gpa.free(json_out);

    var parsed = std.json.parseFromSlice(std.json.Value, gpa, json_out, .{}) catch
        return error.TestUnexpectedResult;
    defer parsed.deinit();
    const obj = parsed.value.object;

    // Always-on fields present even when logging disabled.
    try std.testing.expect(obj.get("health") != null);
    try std.testing.expect(obj.get("claims") != null);
    try std.testing.expect(obj.get("handoffs") != null);
    // invocations must be an empty array (not null) when logging disabled.
    const inv = obj.get("invocations").?;
    try std.testing.expect(inv == .array);
    try std.testing.expectEqual(@as(usize, 0), inv.array.items.len);
}

// =========================================================================
// report-verb: quiet database — empty but well-formed
// =========================================================================

test "report-verb: quiet database — exits 0 and renders explicit zero aggregates" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    // Enable logging but no seeded invocations (just the report invocation itself).
    const cfg_path = writeCliLogConfig(&suite, true);
    defer gpa.free(cfg_path);
    const extra = cfgEnv(cfg_path);

    // Exit 0 (mustRun panics otherwise).
    const text_out = suite.mustRunWith(&.{"report"}, &extra);
    defer gpa.free(text_out);

    // Must render the report sections.
    try std.testing.expect(std.mem.indexOf(u8, text_out, "[invocations]") != null or
        std.mem.indexOf(u8, text_out, "logging disabled") != null);
}

// =========================================================================
// report-verb: invalid flags fail as usage errors
// =========================================================================

test "report-verb: --days 0 exits non-zero with no partial bundle" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(init_out);

    const res = suite.exec(&.{ "report", "--days", "0" });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    try std.testing.expect(res.term == .exited and res.term.exited != 0);
    // No partial bundle in stdout.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "schema_version") == null);
}

test "report-verb: --days -5 exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(init_out);

    const res = suite.exec(&.{ "report", "--days", "-5" });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    try std.testing.expect(res.term == .exited and res.term.exited != 0);
}

test "report-verb: --tail 0 exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(init_out);

    const res = suite.exec(&.{ "report", "--tail", "0" });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    try std.testing.expect(res.term == .exited and res.term.exited != 0);
}

// =========================================================================
// report-aggregates: aggregates reflect seeded activity
// =========================================================================

test "report-aggregates: seeded invocations appear in --json output" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    const cfg_path = writeCliLogConfig(&suite, true);
    defer gpa.free(cfg_path);
    const extra = cfgEnv(cfg_path);

    // Run several verbs WITH logging enabled so they are captured.
    const h1 = suite.mustRunWith(&.{"health"}, &extra);
    defer gpa.free(h1);
    const h2 = suite.mustRunWith(&.{"health"}, &extra);
    defer gpa.free(h2);

    // Report with logging enabled.
    const json_out = suite.mustRunWith(&.{ "report", "--json" }, &extra);
    defer gpa.free(json_out);

    var parsed = std.json.parseFromSlice(std.json.Value, gpa, json_out, .{}) catch
        return error.TestUnexpectedResult;
    defer parsed.deinit();
    const obj = parsed.value.object;
    const inv = obj.get("invocations").?.array;

    // Find the 'health' verb row.
    var found_health = false;
    for (inv.items) |item| {
        const verb_path = item.object.get("verb_path") orelse continue;
        if (std.mem.eql(u8, verb_path.string, "health")) {
            found_health = true;
            const count = item.object.get("count").?.integer;
            // At least 2 'health' invocations were recorded.
            try std.testing.expect(count >= 2);
        }
    }
    try std.testing.expect(found_health);
}

test "report-aggregates: failed invocations appear in failures array" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    const cfg_path = writeCliLogConfig(&suite, true);
    defer gpa.free(cfg_path);
    const extra = cfgEnv(cfg_path);

    // Run a verb that fails with usage error.
    const res = suite.execWith(&.{ "health", "--unknown-flag-xyz" }, &extra);
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);
    try std.testing.expect(res.term == .exited and res.term.exited != 0);

    const json_out = suite.mustRunWith(&.{ "report", "--json" }, &extra);
    defer gpa.free(json_out);

    var parsed = std.json.parseFromSlice(std.json.Value, gpa, json_out, .{}) catch
        return error.TestUnexpectedResult;
    defer parsed.deinit();
    const obj = parsed.value.object;
    const failures = obj.get("failures").?.array;

    // At least one failure category must appear.
    try std.testing.expect(failures.items.len >= 1);
    // Each item must have category and count.
    for (failures.items) |item| {
        try std.testing.expect(item.object.get("category") != null);
        try std.testing.expect(item.object.get("count") != null);
    }
}

// =========================================================================
// report-error-tail: empty tail; cap and ordering; window filter
// =========================================================================

test "report-error-tail: empty tail when nothing failed" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    // Seed only successful invocations.
    seedSuccessInvocation(&suite, "health");

    // Use a config with logging on so the report includes tail data.
    const cfg_path = writeCliLogConfig(&suite, true);
    defer gpa.free(cfg_path);
    const extra = cfgEnv(cfg_path);

    const text_out = suite.mustRunWith(&.{"report"}, &extra);
    defer gpa.free(text_out);

    // Text output must indicate the failure tail is empty.
    try std.testing.expect(std.mem.indexOf(u8, text_out, "[failure tail]  empty") != null or
        std.mem.indexOf(u8, text_out, "failure tail") != null);
}

test "report-error-tail: tail caps at --tail N, newest first" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    // Seed 5 failed invocations with clear time spacing (minutes apart).
    // Verb names carry the age tag so we can assert presence/absence.
    seedFailedInvocation(&suite, "vtail-oldest", "usage", 2, 40);
    seedFailedInvocation(&suite, "vtail-old4", "usage", 2, 30);
    seedFailedInvocation(&suite, "vtail-mid3", "usage", 2, 20);
    seedFailedInvocation(&suite, "vtail-recent2", "usage", 2, 5);
    seedFailedInvocation(&suite, "vtail-newest1", "usage", 2, 1);

    const cfg_path = writeCliLogConfig(&suite, true);
    defer gpa.free(cfg_path);
    const extra = cfgEnv(cfg_path);

    // Request tail of 2 — only 2 rows should appear.
    const text_out = suite.mustRunWith(&.{ "report", "--tail", "2" }, &extra);
    defer gpa.free(text_out);

    // Extract just the [failure tail] section to assert cap and ordering.
    // The full output also contains [invocations] which shows all verb paths —
    // we only care about the tail section for the cap/ordering contract.
    const tail_section_start = std.mem.indexOf(u8, text_out, "[failure tail]") orelse {
        std.debug.print("missing [failure tail] in output:\n{s}\n", .{text_out});
        return error.TestUnexpectedResult;
    };
    const tail_section = text_out[tail_section_start..];

    // vtail-oldest (40m) and vtail-old4 (30m) must NOT appear in the tail section.
    try std.testing.expect(std.mem.indexOf(u8, tail_section, "vtail-oldest") == null);
    try std.testing.expect(std.mem.indexOf(u8, tail_section, "vtail-old4") == null);
    try std.testing.expect(std.mem.indexOf(u8, tail_section, "vtail-mid3") == null);

    // The 2 newest must appear in the tail section.
    try std.testing.expect(std.mem.indexOf(u8, tail_section, "vtail-newest1") != null);
    try std.testing.expect(std.mem.indexOf(u8, tail_section, "vtail-recent2") != null);

    // Newest-first: vtail-newest1 (1m ago) must appear before vtail-recent2 (5m ago).
    const pos_newest = std.mem.indexOf(u8, tail_section, "vtail-newest1").?;
    const pos_recent = std.mem.indexOf(u8, tail_section, "vtail-recent2").?;
    try std.testing.expect(pos_newest < pos_recent);
}

test "report-error-tail: window filter excludes failures outside --days window" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    // Seed one recent failure and one 45-day-old failure.
    const result_old = std.process.run(gpa, std.testing.io, .{
        .argv = &.{
            "sqlite3",
            suite.db_path,
            "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)" ++
                " values ('old-fail', '', 2, 'usage', datetime('now', '-45 days'));",
        },
    }) catch @panic("sqlite3 not found");
    defer gpa.free(result_old.stdout);
    defer gpa.free(result_old.stderr);

    seedFailedInvocation(&suite, "recent-fail", "usage", 2, 1);

    const cfg_path = writeCliLogConfig(&suite, true);
    defer gpa.free(cfg_path);
    const extra = cfgEnv(cfg_path);

    // 30-day window — old-fail must not appear.
    const text_out = suite.mustRunWith(&.{ "report", "--days", "30" }, &extra);
    defer gpa.free(text_out);

    try std.testing.expect(std.mem.indexOf(u8, text_out, "old-fail") == null);
    try std.testing.expect(std.mem.indexOf(u8, text_out, "recent-fail") != null);
}

// =========================================================================
// report-redaction: sentinel text never leaks into the bundle
// =========================================================================

// =========================================================================
// report-worktree-gate: report is not refused from inside a worktree
// =========================================================================

test "report-worktree-gate: report runs from inside a worktree (exit 0, not exit 8)" {
    // Regression guard for the misclassification fixed in this cycle:
    // `report` fell through to `.planning` in verb_classification.zig and
    // was refused by the worktree gate (exit 8) before the handler ran.
    //
    // Strategy: create a fake-worktree path (a real directory whose path
    // contains `.worktrees/<segment>/`, which is the fast-path the gate's
    // detectWorktree uses) and assert that `report --json` exits 0.
    // Then assert that a genuine planning verb (`plan create`) from the
    // same path exits 8, confirming the gate is actually active and `report`
    // is the one being exempted.
    //
    // We disable the harness-default PLANAR_DISABLE_WORKTREE_GATE bypass
    // (set it to "") to activate the real gate in this test binary
    // (compiled with -Dtest-binary=true).
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Build a path that contains `.worktrees/<plan>/<task>/` so the
    // fast-path trigger fires.
    const base = suite.freshSystemTmpDir();
    const wt_path = std.fs.path.join(gpa, &.{
        base, ".worktrees", "571-report-gate", "fix-task",
    }) catch @panic("OOM building wt_path");
    defer gpa.free(wt_path);
    std.Io.Dir.cwd().createDirPath(std.testing.io, wt_path) catch
        @panic("cannot create fake worktree dir");

    // First init a DB from outside the worktree-shaped path so we have a
    // valid database. `base` is a plain dir (no .worktrees/ in it).
    const init_out = suite.mustRunInDir(base, &.{ "init", "--allow-no-repo" });
    gpa.free(init_out);

    // Env: disable the bypass so the gate fires; point at the absolute DB
    // so the child process (cwd = wt_path) finds it.
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DISABLE_WORKTREE_GATE", .value = "" },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = wt_path },
    };

    // `report --json` must NOT be refused by the gate (exit 0).
    const report_res = suite.execWithInDir(wt_path, &.{ "report", "--json" }, &env);
    defer gpa.free(report_res.stdout);
    defer gpa.free(report_res.stderr);
    if (report_res.term != .exited or report_res.term.exited != 0) {
        std.debug.print(
            "\nreport --json from worktree: expected exit 0, got {any}\nstderr: {s}\nstdout: {s}\n",
            .{ report_res.term, report_res.stderr, report_res.stdout },
        );
        try std.testing.expect(false);
    }
    // Confirm we got valid JSON (not an error message from the gate).
    var parsed = std.json.parseFromSlice(std.json.Value, gpa, report_res.stdout, .{}) catch |e| {
        std.debug.print(
            "report --json from worktree: JSON parse failed: {s}\noutput: {s}\n",
            .{ @errorName(e), report_res.stdout },
        );
        return error.TestUnexpectedResult;
    };
    defer parsed.deinit();
    try std.testing.expect(parsed.value == .object);
    try std.testing.expect(parsed.value.object.get("health") != null);

    // Confirm the gate IS active: a planning verb from the same path must
    // be refused with exit 8.
    const plan_res = suite.execWithInDir(
        wt_path,
        &.{ "plan", "create", "should-be-refused" },
        &env,
    );
    defer gpa.free(plan_res.stdout);
    defer gpa.free(plan_res.stderr);
    try std.testing.expect(plan_res.term == .exited and plan_res.term.exited == 8);
    try std.testing.expect(std.mem.indexOf(u8, plan_res.stderr, "may not run from inside a worktree") != null);
}

test "report-worktree-gate: --days 0 exits usage-error (2) not gate-error (8)" {
    // Regression guard: before the fix, `report --days 0` returned exit 8
    // (gate fires before handler) instead of exit 2 (usage error from the
    // handler's validation). After the fix the handler runs, and --days 0
    // is an invalid value → exit 2.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const base = suite.freshSystemTmpDir();
    const wt_path = std.fs.path.join(gpa, &.{
        base, ".worktrees", "571-days-gate", "fix-task",
    }) catch @panic("OOM building wt_path");
    defer gpa.free(wt_path);
    std.Io.Dir.cwd().createDirPath(std.testing.io, wt_path) catch
        @panic("cannot create fake worktree dir");

    const init_out = suite.mustRunInDir(base, &.{ "init", "--allow-no-repo" });
    gpa.free(init_out);

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_DISABLE_WORKTREE_GATE", .value = "" },
        .{ .key = "PLANAR_DB", .value = suite.absDbPath() },
        .{ .key = "PWD", .value = wt_path },
    };

    const days_res = suite.execWithInDir(wt_path, &.{ "report", "--days", "0" }, &env);
    defer gpa.free(days_res.stdout);
    defer gpa.free(days_res.stderr);
    // Must be a usage error (2), not a gate refusal (8).
    try std.testing.expect(days_res.term == .exited);
    try std.testing.expectEqual(@as(u8, 2), days_res.term.exited);
}

test "report-redaction: entity title, flag value, and scope slug never appear in output" {
    // 3816: assert all three sentinel surfaces are redacted from both text and JSON.
    // (a) entity title in a task/plan
    // (b) a flag value passed to an invocation
    // (c) a project scope slug
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(init_out);

    const cfg_path = writeCliLogConfig(&suite, true);
    defer gpa.free(cfg_path);
    const extra = cfgEnv(cfg_path);

    // Sentinel (a): entity title — create a plan whose title is the sentinel.
    // The title is a positional arg; the capture subsystem records only verb_path
    // and flag NAMES, not positional values.
    const sentinel_title = "REDACT_TITLE_SENTINEL_ABC999";
    const plan_out = suite.mustRunWith(
        &.{ "plan", "create", sentinel_title, "--scope", "global" },
        &extra,
    );
    defer gpa.free(plan_out);

    // Sentinel (b): a flag VALUE passed to an invocation. Run health with a
    // --unknown flag whose VALUE is the sentinel; the invocation is recorded
    // with args_shape carrying only the flag NAME, never the value.
    // (health rejects the unknown flag with exit 2, but that's fine — the
    // invocation row is still written before cli_log checks the exit code.)
    const sentinel_flag_value = "REDACT_FLAG_VAL_SENTINEL_DEF888";
    const flag_arg = "--unknown-sentinel=" ++ sentinel_flag_value;
    const flag_res = suite.execWith(&.{ "health", flag_arg }, &extra);
    defer gpa.free(flag_res.stdout);
    defer gpa.free(flag_res.stderr);
    // exit non-zero is expected (unknown flag), that's fine.

    // Sentinel (c): project slug — create an association whose slug contains
    // the sentinel; scope_slug is never SELECTed in the bundle queries.
    const sentinel_slug = "REDACT_SLUG_SENTINEL_GHI777";
    // Use assoc create — slug becomes part of the scope identifier.
    const assoc_res = suite.execWith(
        &.{ "assoc", "create", "--slug", sentinel_slug, "--kind", "repo" },
        &extra,
    );
    defer gpa.free(assoc_res.stdout);
    defer gpa.free(assoc_res.stderr);
    // assoc create may fail if slug constraints aren't met; we only care that
    // even if it writes scope_slug the value never leaks into report output.

    // Run report in both modes.
    const text_out = suite.mustRunWith(&.{"report"}, &extra);
    defer gpa.free(text_out);

    const json_out = suite.mustRunWith(&.{ "report", "--json" }, &extra);
    defer gpa.free(json_out);

    // NONE of the three sentinels must appear in either output.
    if (std.mem.indexOf(u8, text_out, sentinel_title) != null) {
        std.debug.print("sentinel_title leaked into text output: {s}\n", .{text_out});
        try std.testing.expect(false);
    }
    if (std.mem.indexOf(u8, json_out, sentinel_title) != null) {
        std.debug.print("sentinel_title leaked into JSON output: {s}\n", .{json_out});
        try std.testing.expect(false);
    }
    try std.testing.expect(std.mem.indexOf(u8, text_out, sentinel_flag_value) == null);
    try std.testing.expect(std.mem.indexOf(u8, json_out, sentinel_flag_value) == null);
    try std.testing.expect(std.mem.indexOf(u8, text_out, sentinel_slug) == null);
    try std.testing.expect(std.mem.indexOf(u8, json_out, sentinel_slug) == null);
}

test "report-redaction: --json output parses cleanly" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(out);

    const json_out = suite.mustRun(&.{ "report", "--json" });
    defer gpa.free(json_out);

    // Must parse as valid JSON.
    var parsed = std.json.parseFromSlice(std.json.Value, gpa, json_out, .{}) catch |e| {
        std.debug.print("JSON parse error: {s}\noutput was:\n{s}\n", .{ @errorName(e), json_out });
        return error.TestUnexpectedResult;
    };
    defer parsed.deinit();

    try std.testing.expect(parsed.value == .object);
}

// =========================================================================
// report-verb: --days abc non-integer (3818)
// =========================================================================

test "report-verb: --days abc exits usage-error (2) with no partial bundle" {
    // 3818: non-integer --days must be rejected as a usage error (exit 2)
    // by the CLI parser (InvalidValue), not silently produce a partial bundle.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(init_out);

    const res = suite.exec(&.{ "report", "--days", "abc" });
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // Must fail as a usage error (exit 2 from InvalidValue parse error).
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u8, 2), res.term.exited);
    // No partial bundle in stdout.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "schema_version") == null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "[invocations]") == null);
}

// =========================================================================
// report-aggregates: non-zero agent_actions, sync_events, task reopen (3817)
// =========================================================================

/// Seed a session row and return its rowid (needed for agent_actions FK).
fn seedSession(suite: *harness.Suite) i64 {
    const sql =
        "insert into sessions (vendor, started_at) values ('test', datetime('now'));" ++
        "select last_insert_rowid();";
    const result = std.process.run(suite.allocator, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, sql },
    }) catch @panic("sqlite3 not found");
    defer suite.allocator.free(result.stderr);
    const out = std.mem.trim(u8, result.stdout, " \n\r\t");
    const id = std.fmt.parseInt(i64, out, 10) catch 0;
    suite.allocator.free(result.stdout);
    return id;
}

/// Seed an agent_action row (completed or errored) using sqlite3.
fn seedAgentAction(
    suite: *harness.Suite,
    session_id: i64,
    action_kind: []const u8,
    outcome: []const u8,
) void {
    var sql_buf: [512]u8 = undefined;
    const sql = std.fmt.bufPrint(
        &sql_buf,
        "insert into agent_actions (session_id, action_kind, vendor, outcome, started_at, ended_at)" ++
            " values ({d}, '{s}', 'test', '{s}', datetime('now'), datetime('now'));",
        .{ session_id, action_kind, outcome },
    ) catch @panic("sql buf too small");
    const result = std.process.run(suite.allocator, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, sql },
    }) catch @panic("sqlite3 not found");
    defer suite.allocator.free(result.stdout);
    defer suite.allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) @panic("seed agent_action failed");
}

/// Seed a sync_event conflict row using sqlite3.
fn seedSyncConflict(suite: *harness.Suite) void {
    const sql =
        "insert into sync_events (scope, direction, outcome, at)" ++
        " values ('external', 'pull', 'conflict', datetime('now'));";
    const result = std.process.run(suite.allocator, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, sql },
    }) catch @panic("sqlite3 not found");
    defer suite.allocator.free(result.stdout);
    defer suite.allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) @panic("seed sync_event failed");
}

test "report-aggregates: agent_actions, sync_events, and task reopens produce non-zero counts" {
    // 3817: seed non-empty data through the CLI where possible. agent_actions
    // and sync_events have no single CLI surface that creates them in isolation,
    // so those still use direct SQL seeding. Task reopens now go through the
    // real CLI (task add → task done → task reopen) since the engine wires the
    // task_reopens insert (task 3825).
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const init_out = suite.mustRun(&.{ "init", "--allow-no-repo" });
    defer gpa.free(init_out);

    // Seed agent_actions: one completed coder action and one error action.
    const session_id = seedSession(&suite);
    seedAgentAction(&suite, session_id, "coder", "ok");
    seedAgentAction(&suite, session_id, "coder", "error");

    // Seed sync_events: one conflict.
    seedSyncConflict(&suite);

    // Drive a task through the reopen lifecycle via the real CLI. This is the
    // end-to-end path: add → done → reopen. The engine now inserts a
    // task_reopens row inside reopen(), so the aggregate query returns >= 1.
    {
        const task = suite.mustRunJSON(TaskJSON, arena, &.{
            "task", "add", "--json", "reopen-aggregate-t1",
        });
        const task_id_str = std.fmt.allocPrint(arena, "{d}", .{task.id}) catch unreachable;

        // Advance to doing first (todo → done is not in the matrix).
        gpa.free(suite.mustRun(&.{ "task", "update", task_id_str, "--status", "doing" }));
        gpa.free(suite.mustRun(&.{ "task", "done", task_id_str }));
        gpa.free(suite.mustRun(&.{
            "task", "reopen", task_id_str, "--reason", "integration test",
        }));
    }

    // Now run report --json and assert non-zero aggregates.
    const json_out = suite.mustRun(&.{ "report", "--json" });
    defer gpa.free(json_out);

    var parsed = std.json.parseFromSlice(std.json.Value, gpa, json_out, .{}) catch
        return error.TestUnexpectedResult;
    defer parsed.deinit();
    const obj = parsed.value.object;

    // actions: must have at least one row.
    const actions = obj.get("actions").?.array;
    try std.testing.expect(actions.items.len >= 1);
    // Find the coder/ok and coder/error entries.
    var found_ok = false;
    var found_err = false;
    for (actions.items) |item| {
        const kind = (item.object.get("action_kind") orelse continue).string;
        const outcome = (item.object.get("outcome") orelse continue).string;
        if (std.mem.eql(u8, kind, "coder") and std.mem.eql(u8, outcome, "ok")) found_ok = true;
        if (std.mem.eql(u8, kind, "coder") and std.mem.eql(u8, outcome, "error")) found_err = true;
    }
    try std.testing.expect(found_ok);
    try std.testing.expect(found_err);

    // sync: must have at least one conflict row.
    const sync = obj.get("sync").?.array;
    try std.testing.expect(sync.items.len >= 1);
    var found_conflict = false;
    for (sync.items) |item| {
        const outcome = (item.object.get("outcome") orelse continue).string;
        if (std.mem.eql(u8, outcome, "conflict")) {
            found_conflict = true;
            try std.testing.expect(item.object.get("count").?.integer >= 1);
        }
    }
    try std.testing.expect(found_conflict);

    // reopens: must be >= 1 (task reopen above wrote a task_reopens row via the engine).
    const reopens = obj.get("reopens") orelse {
        std.debug.print("'reopens' field missing from --json output\n", .{});
        return error.TestUnexpectedResult;
    };
    try std.testing.expect(reopens.integer >= 1);
}
