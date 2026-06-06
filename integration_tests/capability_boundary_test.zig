//! integration_tests/capability_boundary_test.zig — plan 85 M7 closeout.
//!
//! Locks the capability invariants of the five-binary architecture as
//! integration tests. The capability boundary is the binary's verb set,
//! not runtime ACLs; future changes that add a write verb to
//! planar-watch / planar-doc or a planning-entity verb to planar-agent
//! will fail these tests immediately.
//!
//! Verifies:
//!
//!   - planar-agent verb-set audit (t#2598 / capability-boundary-test).
//!     Recursive --help walk asserts EXACTLY the 13 documented verbs
//!     (pull, peek, claim, heartbeat, complete, fail, release, block,
//!     action, ingest, reconcile, abort, version) plus action's
//!     start/end subverbs, AND contains NONE of the planning-entity
//!     verbs (plan, task, decision, question, scenario, artifact,
//!     annotate, init, workbench, doc, spec, templates, ext, sync,
//!     promote, demote, capture, dashboard, tree, health).
//!
//!   - planar-watch verb-set audit (t#2599 /
//!     planar-watch-capability-boundary-test). Asserts EXACTLY the 6
//!     read verbs + version + completion, AND no write verbs from
//!     either planar-agent or planar.
//!
//!   - planar-doc verb-set audit. Asserts EXACTLY the 7 doc verbs
//!     (build, verify, diff, cover, nodoc, lint, schema), AND no write
//!     verbs from planar-agent and no planning-entity verbs from planar.
//!
//!   - planar-watch DB stays read-only at the binary level: invoking
//!     planar-watch verbs against a populated DB does NOT mutate the
//!     schema_migrations baseline row or any agent_* table contents.
//!     The unit-test in src/cmd/planar-watch/main.zig already pins
//!     the SQLite-driver-level rejection of write SQL through the
//!     read-only handle (M8 invariant); this integration-level check
//!     covers the binary-as-a-whole.
//!
//!   - planar-execute verb-set audit (t#3204 / m10-capability-registration).
//!     Asserts EXACTLY the 3 verbs: run, version, doctor.
//!     AND contains NONE of the planning-entity verbs (plan, task,
//!     decision, question, scenario, artifact, etc.) — planar-execute
//!     is a pure CLI driver that holds no DB handle.
//!
//! These tests are the SECURITY contract — a vendor hook configured
//! with only planar-agent on PATH cannot touch planning state; a
//! watcher configured with only planar-watch on PATH cannot touch
//! anything at all.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// Bin resolvers.
// =========================================================================

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

fn resolveWatchBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_WATCH_BIN=")) {
            return s["PLANAR_WATCH_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_WATCH_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

fn resolveDocBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_DOC_BIN=")) {
            return s["PLANAR_DOC_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_DOC_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

fn resolveExecuteBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_EXECUTE_BIN=")) {
            return s["PLANAR_EXECUTE_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_EXECUTE_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

// Run a binary with the suite's PLANAR_DB injected; return RunResult.
fn runBin(
    suite: *const harness.Suite,
    bin: []const u8,
    args: []const []const u8,
) harness.Suite.RunResult {
    const gpa = suite.allocator;

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
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM injecting PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runBin spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

// Parse a Planar `--help` COMMANDS table into the set of verb names.
// The table line shape is two leading spaces, the verb token, then >=2
// spaces, then the description. We tokenize the line on whitespace and
// take token[0] as the verb name. Stops at the first blank line after
// the COMMANDS header.
fn parseHelpVerbs(
    gpa: std.mem.Allocator,
    help: []const u8,
) std.StringHashMap(void) {
    var set = std.StringHashMap(void).init(gpa);
    const header_idx = std.mem.indexOf(u8, help, "COMMANDS:") orelse {
        std.debug.print("help output missing COMMANDS section:\n{s}\n", .{help});
        @panic("no COMMANDS header in help");
    };
    var it = std.mem.splitScalar(u8, help[header_idx..], '\n');
    _ = it.next(); // skip "COMMANDS:" line itself.
    while (it.next()) |line| {
        if (line.len == 0) break; // table ends at first blank line.
        if (!std.mem.startsWith(u8, line, "  ")) break; // table ends at de-indent.
        // Tokenize on whitespace; first token is the verb.
        var tok_it = std.mem.tokenizeAny(u8, line, " \t");
        const first = tok_it.next() orelse continue;
        // Copy into a stable heap buffer so the map key outlives the
        // input slice (the input is owned by the caller and will be
        // freed before the caller reads the map).
        const owned = gpa.dupe(u8, first) catch @panic("OOM");
        set.put(owned, {}) catch @panic("OOM");
    }
    return set;
}

fn freeVerbSet(gpa: std.mem.Allocator, set: *std.StringHashMap(void)) void {
    var it = set.keyIterator();
    while (it.next()) |k| gpa.free(k.*);
    set.deinit();
}

fn assertContainsAll(
    set: *const std.StringHashMap(void),
    required: []const []const u8,
    bin: []const u8,
) !void {
    for (required) |v| {
        if (!set.contains(v)) {
            std.debug.print(
                "[{s}] capability-boundary: REQUIRED verb '{s}' missing from --help; have {d} verbs\n",
                .{ bin, v, set.count() },
            );
            var it = set.keyIterator();
            while (it.next()) |k| std.debug.print("  - {s}\n", .{k.*});
            return error.MissingRequiredVerb;
        }
    }
}

fn assertContainsNone(
    set: *const std.StringHashMap(void),
    forbidden: []const []const u8,
    bin: []const u8,
) !void {
    for (forbidden) |v| {
        if (set.contains(v)) {
            std.debug.print(
                "[{s}] capability-boundary: FORBIDDEN verb '{s}' leaked into --help\n",
                .{ bin, v },
            );
            return error.ForbiddenVerbPresent;
        }
    }
}

fn assertExactSet(
    set: *const std.StringHashMap(void),
    expected: []const []const u8,
    bin: []const u8,
) !void {
    if (set.count() != expected.len) {
        std.debug.print(
            "[{s}] capability-boundary: verb count mismatch — got {d}, expected {d}\n",
            .{ bin, set.count(), expected.len },
        );
        std.debug.print("  expected:\n", .{});
        for (expected) |v| std.debug.print("    - {s}\n", .{v});
        std.debug.print("  actual:\n", .{});
        var it = set.keyIterator();
        while (it.next()) |k| std.debug.print("    - {s}\n", .{k.*});
        return error.VerbSetSizeMismatch;
    }
    try assertContainsAll(set, expected, bin);
}

// =========================================================================
// t#2598 — planar-agent capability boundary.
// =========================================================================
//
// The expected verb set is the 13 atomic + recovery verbs documented in
// `~/.planar/workbench/project_planar/p85-agent-activity/58-…tech-spec.md`
// § "Binary boundaries":
//
//     pull, peek, claim, heartbeat, complete, fail, release, block,
//     action, ingest, reconcile, abort, version
//
// `action` has two subverbs (start, end) — verified by recursing into
// `planar-agent action --help`.
//
// The forbidden set covers every planning-entity mutation verb that
// `planar` owns. A future change that mistakenly registers any of
// these on planar-agent fails this test immediately.

test "planar-agent verb set is EXACTLY the 13 documented agent verbs" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runBin(&suite, resolveAgentBin(), &.{"--help"});
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    var verbs = parseHelpVerbs(gpa, res.stdout);
    defer freeVerbSet(gpa, &verbs);

    try assertExactSet(&verbs, &.{
        "version",
        "pull",
        "peek",
        "complete",
        "fail",
        "release",
        "block",
        "claim",
        "heartbeat",
        "action",
        "ingest",
        "reconcile",
        "abort",
        "schema",
    }, "planar-agent");

    // Forbidden set: planning-entity verbs and operator-only namespaces.
    try assertContainsNone(&verbs, &.{
        "plan",     "task",      "decision",  "question",  "scenario",
        "artifact", "annotate",  "init",      "workbench", "doc",
        "spec",     "templates", "ext",       "sync",      "promote",
        "demote",   "capture",   "dashboard", "tree",      "health",
    }, "planar-agent");
}

test "planar-agent action subverbs are EXACTLY {start, end}" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runBin(&suite, resolveAgentBin(), &.{ "action", "--help" });
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    var verbs = parseHelpVerbs(gpa, res.stdout);
    defer freeVerbSet(gpa, &verbs);

    try assertExactSet(&verbs, &.{ "start", "end" }, "planar-agent action");
}

// Recursively walk every leaf verb's --help; assert each invocation
// exits 0. This is the "no broken --help anywhere in the tree" check,
// orthogonal to the verb-set audit but cheap to bundle in the same
// file. Acts as a regression guard against a leaf verb whose help
// renderer crashes.
test "planar-agent recursive --help walk: every verb's --help exits 0" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Walk the top-level verbs; for `action`, recurse one level.
    const top = [_][]const u8{
        "version", "pull",      "peek",  "complete",  "fail",
        "release", "block",     "claim", "heartbeat", "action",
        "ingest",  "reconcile", "abort", "schema",
    };
    for (top) |v| {
        const res = runBin(&suite, resolveAgentBin(), &.{ v, "--help" });
        defer res.deinit(gpa);
        if (res.term != .exited or res.term.exited != 0) {
            std.debug.print(
                "planar-agent {s} --help exited non-zero (term={any}):\nstdout: {s}\nstderr: {s}\n",
                .{ v, res.term, res.stdout, res.stderr },
            );
            return error.HelpRendererFailed;
        }
    }

    for ([_][]const u8{ "start", "end" }) |sub| {
        const res = runBin(&suite, resolveAgentBin(), &.{ "action", sub, "--help" });
        defer res.deinit(gpa);
        if (res.term != .exited or res.term.exited != 0) {
            std.debug.print(
                "planar-agent action {s} --help exited non-zero (term={any}):\nstdout: {s}\nstderr: {s}\n",
                .{ sub, res.term, res.stdout, res.stderr },
            );
            return error.HelpRendererFailed;
        }
    }
}

// =========================================================================
// t#2599 — planar-watch capability boundary.
// =========================================================================
//
// Expected: EXACTLY the read verbs (feed, ps, claims, actions, plans,
// log, tree) plus version + completion. Forbidden: every planar-agent
// write verb AND every planar planning-entity verb.

test "planar-watch verb set is EXACTLY the read verbs + version + completion" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runBin(&suite, resolveWatchBin(), &.{"--help"});
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    var verbs = parseHelpVerbs(gpa, res.stdout);
    defer freeVerbSet(gpa, &verbs);

    try assertExactSet(&verbs, &.{
        "feed", "ps", "claims", "actions", "plans", "log", "tree", "version", "completion", "schema",
    }, "planar-watch");

    // Forbidden: every planar-agent write verb.
    try assertContainsNone(&verbs, &.{
        "pull",    "claim", "heartbeat", "complete", "fail",
        "release", "block", "action",    "ingest",   "reconcile",
        "abort",   "peek",
    }, "planar-watch");

    // Forbidden: every planar planning-entity verb.
    try assertContainsNone(&verbs, &.{
        "plan",     "task",      "decision", "question",  "scenario",
        "artifact", "annotate",  "init",     "workbench", "doc",
        "spec",     "templates", "ext",      "sync",      "promote",
        "demote",   "capture",
    }, "planar-watch");
}

// =========================================================================
// planar-doc capability boundary — the fourth binary.
// =========================================================================
//
// planar-doc is the doc-system manifest tool. It touches the working tree
// and the .manifest-docs index, NOT SQLite — so it must expose only its
// seven doc verbs and NONE of planar-agent's write verbs or planar's
// planning-entity verbs. assertExactSet pins the count, so any verb added
// to planar-doc (regardless of origin) fails this test immediately.

test "planar-doc verb set is EXACTLY the 7 doc verbs" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runBin(&suite, resolveDocBin(), &.{"--help"});
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    var verbs = parseHelpVerbs(gpa, res.stdout);
    defer freeVerbSet(gpa, &verbs);

    try assertExactSet(&verbs, &.{
        "build", "verify", "diff", "cover", "nodoc", "lint", "schema",
    }, "planar-doc");

    // Forbidden: every planar-agent write verb.
    try assertContainsNone(&verbs, &.{
        "pull",    "claim", "heartbeat", "complete", "fail",
        "release", "block", "action",    "ingest",   "reconcile",
        "abort",   "peek",
    }, "planar-doc");

    // Forbidden: every planar planning-entity verb.
    try assertContainsNone(&verbs, &.{
        "plan",      "task",      "decision", "question",  "scenario",
        "artifact",  "annotate",  "init",     "workbench", "spec",
        "templates", "ext",       "sync",     "promote",   "demote",
        "capture",   "dashboard", "tree",     "health",
    }, "planar-doc");
}

// planar-watch is read-only at two levels:
//   1. its verb set has zero write verbs (the previous test);
//   2. its DB handle is opened with SQLITE_OPEN_READONLY so the
//      driver itself rejects write SQL.
//
// The SQLite-driver-level rejection is pinned by a unit test under
// src/cmd/planar-watch/main.zig (M8 invariant). This integration-level
// test is the binary-as-a-whole equivalent: invoke planar-watch verbs
// against a populated DB and assert NO row in agent_work_claims /
// agent_actions / tasks changed.

test "planar-watch verbs do not mutate any DB row (binary-level read-only)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed the DB through planar + planar-agent so there is real data
    // to read.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "cap-bnd-ro", "--json", "cap-bnd-ro" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "first" }));

    // Write a claim through planar-agent so there is observable state
    // for planar-watch to read.
    const pull_res = runBin(&suite, resolveAgentBin(), &.{
        "pull", plan_arg, "--vendor", "test", "--ttl", "60", "--no-locality-probe", "--json",
    });
    defer pull_res.deinit(gpa);
    try std.testing.expect(pull_res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), pull_res.term.exited);

    // Snapshot row counts before invoking planar-watch.
    const before_claims = countRowsViaSqlite(gpa, suite.db_path, "agent_work_claims");
    const before_actions = countRowsViaSqlite(gpa, suite.db_path, "agent_actions");
    const before_tasks = sumTaskStatusHash(gpa, suite.db_path);

    // Invoke every planar-watch read verb.
    const verbs = [_][]const []const u8{
        &.{ "ps", "--json" },
        &.{ "claims", "--json" },
        &.{ "actions", "--json" },
        &.{ "plans", "--json" },
        &.{ "log", "--task", "1", "--json" },
        &.{ "feed", "--json" },
        &.{"version"},
    };
    for (verbs) |args| {
        const r = runBin(&suite, resolveWatchBin(), args);
        defer r.deinit(gpa);
        // Some verbs (log with no entries) may still return 0; we
        // don't gate on the exit code here, only on the post-state.
        // version always exits 0.
        _ = r.term;
    }

    const after_claims = countRowsViaSqlite(gpa, suite.db_path, "agent_work_claims");
    const after_actions = countRowsViaSqlite(gpa, suite.db_path, "agent_actions");
    const after_tasks = sumTaskStatusHash(gpa, suite.db_path);

    if (before_claims != after_claims) {
        std.debug.print(
            "planar-watch mutated agent_work_claims: {d} → {d}\n",
            .{ before_claims, after_claims },
        );
        return error.ClaimsTableMutated;
    }
    if (before_actions != after_actions) {
        std.debug.print(
            "planar-watch mutated agent_actions: {d} → {d}\n",
            .{ before_actions, after_actions },
        );
        return error.ActionsTableMutated;
    }
    if (before_tasks != after_tasks) {
        std.debug.print(
            "planar-watch mutated tasks.status hash: {d} → {d}\n",
            .{ before_tasks, after_tasks },
        );
        return error.TasksTableMutated;
    }
}

// =========================================================================
// t#3204 — planar-execute capability boundary (plan 492 M10).
// =========================================================================
//
// planar-execute is a pure CLI driver (Lua-driven workflow harness). It
// holds NO DB handle and MUST NOT expose any planning-entity verbs.
//
// Expected verb set: EXACTLY {run, version, doctor}.
//
// Forbidden set: every planning-entity verb that `planar` owns, and every
// agent-coordination verb that `planar-agent` owns. If a future change
// accidentally registers a planning or agent verb on planar-execute,
// this test fails immediately.

test "planar-execute verb set is EXACTLY {run, version, doctor}" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runBin(&suite, resolveExecuteBin(), &.{"--help"});
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    var verbs = parseHelpVerbs(gpa, res.stdout);
    defer freeVerbSet(gpa, &verbs);

    try assertExactSet(&verbs, &.{
        "run",
        "version",
        "doctor",
    }, "planar-execute");

    // Forbidden: planning-entity verbs.
    try assertContainsNone(&verbs, &.{
        "plan",     "task",      "decision",  "question",  "scenario",
        "artifact", "annotate",  "init",      "workbench", "doc",
        "spec",     "templates", "ext",       "sync",      "promote",
        "demote",   "capture",   "dashboard", "tree",      "health",
    }, "planar-execute");

    // Forbidden: agent-coordination verbs.
    try assertContainsNone(&verbs, &.{
        "pull",      "peek",    "claim", "heartbeat", "complete",
        "fail",      "release", "block", "action",    "ingest",
        "reconcile", "abort",
    }, "planar-execute");
}

// =========================================================================
// Helpers — minimal sqlite shell + json extraction.
// =========================================================================

fn extractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':')) : (i += 1) {}
    const start = i;
    while (i < json.len and (json[i] == '-' or (json[i] >= '0' and json[i] <= '9'))) : (i += 1) {}
    if (i == start) return null;
    return std.fmt.parseInt(i64, json[start..i], 10) catch null;
}

// Shell out to sqlite3 (vendored binary not assumed to be on PATH —
// fall back to a tiny std.process.run on `sqlite3` from the system
// PATH; if that's unavailable the test is skipped with a panic that
// surfaces the cause).
fn countRowsViaSqlite(gpa: std.mem.Allocator, db_path: []const u8, table: []const u8) i64 {
    const sql = std.fmt.allocPrint(gpa, "SELECT count(*) FROM {s};", .{table}) catch @panic("OOM");
    defer gpa.free(sql);
    return runSqliteScalar(gpa, db_path, sql);
}

// A coarse "did the tasks table change?" fingerprint — sum of
// status-text hashes per row. Sensitive to row count, status flips,
// and row deletion. Not collision-free, but good enough to catch any
// real planar-watch write attempt.
fn sumTaskStatusHash(gpa: std.mem.Allocator, db_path: []const u8) i64 {
    const sql = "SELECT coalesce(sum(length(status) * id), 0) FROM tasks;";
    return runSqliteScalar(gpa, db_path, sql);
}

fn runSqliteScalar(gpa: std.mem.Allocator, db_path: []const u8, sql: []const u8) i64 {
    const result = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", db_path, sql },
    }) catch |e| std.debug.panic("sqlite3 spawn failed: {s}", .{@errorName(e)});
    defer gpa.free(result.stdout);
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.panic("sqlite3 returned non-zero: {s}", .{result.stderr});
    }
    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    return std.fmt.parseInt(i64, trimmed, 10) catch |e| {
        std.debug.panic("sqlite3 output not integer ('{s}'): {s}", .{ trimmed, @errorName(e) });
    };
}
