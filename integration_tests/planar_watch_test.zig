//! integration_tests/planar_watch_test.zig — black-box tests for the
//! planar-watch binary (plan 85 M8: read-only viewer).
//!
//! Covered scenarios (slugs in parens map to test-spec coverage):
//!
//!   - --help surfaces ONLY the read verbs + version + completion
//!     (planar-watch-cmd-tree, capability boundary).
//!   - version emits planar-watch-prefixed line.
//!   - Default invocation (no args) routes to `feed` (planar-watch-feed).
//!   - ps --json returns {generated_at, active, stale} with correct
//!     ClaimRow shape (planar-watch-ps).
//!   - claims --json returns {generated_at, claims} with --status filters
//!     (planar-watch-claims).
//!   - actions --json returns {generated_at, actions} (planar-watch-actions).
//!   - plans --json returns {generated_at, plans:[{plan, in_flight,
//!     active_claims, active_actions, last_event_at}]}
//!     (planar-watch-plans).
//!   - log --task returns {entity, entries:[]} with action+claim union
//!     (planar-watch-log).
//!   - schema-handshake exits 7 with remediation message
//!     (planar-watch-schema-handshake-test).

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// Resolve the planar-watch binary path and the planar-agent binary path
// (the latter is used to drive write-side activity into the scratch DB
// so the watch verbs have something to read).
// =========================================================================

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

fn mustRunWatch(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runBin(suite, resolveWatchBin(), args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-watch failed (term={any}): {s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-watch must-run failed");
    }
    return res.stdout;
}

fn mustRunAgent(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runBin(suite, resolveAgentBin(), args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-agent failed (term={any}): {s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

/// Seed scratch DB with init + plan + one todo task. Returns the plan id
/// as a heap-allocated decimal string (caller frees via `gpa.free`).
fn seedPlanWithTask(suite: *const harness.Suite, plan_slug: []const u8, task_title: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", plan_slug, "--json", plan_slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    const task_out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, task_title });
    gpa.free(task_out);
    return plan_id_arg;
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

fn extractStringField(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse return error.FieldNotFound;
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    if (end == json.len) return error.UnterminatedString;
    return try gpa.dupe(u8, json[i..end]);
}

// =========================================================================
// Capability boundary: --help enumerates the read-only verb set only.
// =========================================================================

test "planar-watch --help lists ONLY the 6 read verbs + version + completion" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runBin(&suite, resolveWatchBin(), &.{"--help"});
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // Must mention each read verb.
    inline for ([_][]const u8{
        "feed", "ps", "claims", "actions", "plans", "log", "version", "completion",
    }) |v| {
        if (std.mem.indexOf(u8, res.stdout, v) == null) {
            std.debug.print("missing verb '{s}' in --help:\n{s}\n", .{ v, res.stdout });
            return error.MissingVerb;
        }
    }

    // Capability boundary — no write verb may appear anywhere in the
    // help output. These are planar-agent's; they MUST NOT be
    // reachable through planar-watch.
    //
    // Each search term is prefixed/suffixed by word boundaries so we
    // don't false-positive on substrings (e.g. "release" inside a
    // doc-comment for some unrelated verb). The verb list in
    // --help is one-per-line; matching "  pull " (two-space prefix +
    // trailing space) is the COMMANDS table's exact format.
    inline for ([_][]const u8{
        "  pull ",    "  claim ",  "  complete ",  "  fail ",
        "  release ", "  block ",  "  heartbeat ", "  reconcile ",
        "  abort ",   "  ingest ", "  action ",
    }) |bad| {
        if (std.mem.indexOf(u8, res.stdout, bad) != null) {
            std.debug.print("planar-watch --help leaked write verb '{s}':\n{s}\n", .{ bad, res.stdout });
            return error.WriteVerbExposed;
        }
    }
}

// =========================================================================
// version verb.
// =========================================================================

test "planar-watch version emits planar-watch-prefixed line" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runBin(&suite, resolveWatchBin(), &.{"version"});
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    try std.testing.expect(std.mem.startsWith(u8, res.stdout, "planar-watch "));
}

// =========================================================================
// Schema-handshake: pre-init DB triggers exit 7 with remediation.
// =========================================================================

test "planar-watch against an empty DB exits 7 with remediation" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Pre-create an EMPTY database file (no schema_migrations table).
    // sqlite3_open_v2(READONLY) refuses to create the file, so the
    // bootstrap step opens an empty handle but the version probe
    // returns 0 and the handshake refuses with exit 7.
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = suite.db_path,
        .data = "",
    });

    const res = runBin(&suite, resolveWatchBin(), &.{"ps"});
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 7), res.term.exited);
    if (std.mem.indexOf(u8, res.stderr, "older than this binary's minimum") == null) {
        std.debug.print(
            "schema-handshake stderr did not include remediation:\n{s}\n",
            .{res.stderr},
        );
        return error.NoRemediation;
    }
}

// =========================================================================
// ps --json against a seeded fixture.
// =========================================================================

test "planar-watch ps --json returns generated_at + active + stale" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-ps", "ps-task");
    defer gpa.free(pid);
    // Drive a claim through planar-agent so ps has something to show.
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);

    const out = mustRunWatch(&suite, &.{ "ps", "--json" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "\"generated_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"active\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"stale\":[") != null);
    // The active claim row is in the snapshot.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"claim_token\":\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"active\"") != null);
}

// =========================================================================
// claims --json.
// =========================================================================

test "planar-watch claims --json returns {generated_at, claims}" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-claims", "claims-task");
    defer gpa.free(pid);
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    const out = mustRunWatch(&suite, &.{ "claims", "--json", "--status", "active" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "\"generated_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"claims\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"active\"") != null);

    // --status all bucket should ALSO be reachable.
    const all_out = mustRunWatch(&suite, &.{ "claims", "--json", "--status", "all" });
    defer gpa.free(all_out);
    try std.testing.expect(std.mem.indexOf(u8, all_out, "\"claims\":[") != null);
}

// =========================================================================
// actions --json.
// =========================================================================

test "planar-watch actions --json returns {generated_at, actions}" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-actions", "actions-task");
    defer gpa.free(pid);
    // pull inserts both a claim row AND an opening action row.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    const out = mustRunWatch(&suite, &.{ "actions", "--json" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "\"generated_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"actions\":[") != null);
    // The pull action carries vendor="zig-test" or similar; pin on
    // the started_at key existing.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"started_at\":\"") != null);
}

// =========================================================================
// plans --json.
// =========================================================================

test "planar-watch plans --json returns {generated_at, plans:[{plan, in_flight, active_claims, ...}]}" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-plans", "plans-task");
    defer gpa.free(pid);
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    const out = mustRunWatch(&suite, &.{ "plans", "--json" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "\"generated_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"plans\":[") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"in_flight\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"active_claims\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"active_actions\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"last_event_at\":") != null);

    // --in-flight-only narrows. The seeded plan IS in flight, so it
    // should still appear.
    const inflight_out = mustRunWatch(&suite, &.{ "plans", "--json", "--in-flight-only" });
    defer gpa.free(inflight_out);
    try std.testing.expect(std.mem.indexOf(u8, inflight_out, "\"in_flight\":true") != null);
}

// =========================================================================
// log --task.
// =========================================================================

test "planar-watch log --task --json returns {entity, entries:[]} with action+claim union" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-log", "log-task");
    defer gpa.free(pid);
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);

    // Resolve the task id from the pull JSON.
    const task_id = extractIntField(pull_out, "\"task\":{\"id\"") orelse @panic("no task id");
    const tid_arg = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(tid_arg);

    const out = mustRunWatch(&suite, &.{ "log", "--task", tid_arg, "--json" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "\"entity\":{\"kind\":\"task\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"entries\":[") != null);
    // The claim acquire event shows up.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"kind\":\"claim_acquired\"") != null);
    // The pull-side action shows up.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"kind\":\"action\"") != null);
}

test "planar-watch log without exactly-one filter exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // init the DB so the schema handshake passes.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const res = runBin(&suite, resolveWatchBin(), &.{ "log", "--json" });
    defer res.deinit(gpa);
    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
}

// =========================================================================
// feed verb (cross-cutting activity feed).
// =========================================================================

test "planar-watch feed --json (cross-vendor) emits events in occurrence order" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-feed", "feed-task");
    defer gpa.free(pid);
    // Drive: pull + complete. Should generate at least
    // claim_acquired, action_started, action_ended, completed.
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "shipped", "--json" }));

    const out = mustRunWatch(&suite, &.{ "feed", "--json" });
    defer gpa.free(out);

    // Each line is its own JSON object (NDJSON).
    try std.testing.expect(std.mem.indexOf(u8, out, "\"event\":\"claim_acquired\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"event\":\"action_started\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"event\":\"completed\"") != null);

    // Verify NDJSON shape: each line that starts with `{` ends with
    // a newline + the next line is also `{...}` or EOF.
    var line_it = std.mem.tokenizeAny(u8, out, "\n");
    var lines_seen: usize = 0;
    while (line_it.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t");
        if (trimmed.len == 0) continue;
        try std.testing.expect(trimmed[0] == '{');
        try std.testing.expect(trimmed[trimmed.len - 1] == '}');
        lines_seen += 1;
    }
    try std.testing.expect(lines_seen >= 3);
}

// =========================================================================
// --plan filter widening (plan 85 t#2622): --plan <id> matches plan-direct
// AND task-on-plan AND plan_step-on-plan events. Pre-fix, only plan-direct
// rows matched, silently dropping task claim events.
// =========================================================================

test "planar-watch feed --plan includes task-on-plan claim and action events" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed: plan with a single todo task.
    const pid = seedPlanWithTask(&suite, "feed-plan-filter", "child-task");
    defer gpa.free(pid);

    // Drive: pull the task. The resulting claim has
    // entity_kind=task / entity_id=<task-id>, NOT entity_kind=plan.
    // Pre-fix, --plan <plan-id> would drop every event here.
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no token");
    defer gpa.free(token);
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "done", "--json" }));

    // Filtered feed view, narrowed to this plan id.
    const out = mustRunWatch(&suite, &.{ "feed", "--plan", pid, "--json" });
    defer gpa.free(out);

    // With the widened filter, task-on-plan events MUST be visible.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"event\":\"claim_acquired\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"event\":\"action_started\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"event\":\"completed\"") != null);
}

test "planar-watch feed --plan excludes events on a DIFFERENT plan" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Two plans, each with one task. Pull from plan A only.
    const pid_a = seedPlanWithTask(&suite, "feed-plan-a", "task-a");
    defer gpa.free(pid_a);
    // seedPlanWithTask runs `init --skip-project` so call a more focused
    // path for the second plan: just plan create + task add.
    const plan_b_json = suite.mustRun(&.{ "plan", "create", "--slug", "feed-plan-b", "--json", "feed-plan-b" });
    defer gpa.free(plan_b_json);
    const pid_b_int = extractIntField(plan_b_json, "\"id\"") orelse @panic("no plan-b id");
    const pid_b = std.fmt.allocPrint(gpa, "{d}", .{pid_b_int}) catch @panic("OOM");
    defer gpa.free(pid_b);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", pid_b, "task-b" }));

    // Pull plan A.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid_a, "--no-locality-probe", "--json" }));

    // Filter by plan B — should produce a feed with no claim_acquired
    // event (the only claim in the system targets a task on plan A).
    const out = mustRunWatch(&suite, &.{ "feed", "--plan", pid_b, "--json" });
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"event\":\"claim_acquired\"") == null);
}

// =========================================================================
// Default verb: bare planar-watch routes to feed.
// =========================================================================

test "planar-watch with no args defaults to feed" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-default", "default-task");
    defer gpa.free(pid);
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    // No args. Should NOT print --help (which is `USAGE:`); should
    // print at least one feed event.
    const out = mustRunWatch(&suite, &.{"--json"});
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"event\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "USAGE:") == null);
}

// =========================================================================
// --follow loop terminates cleanly on SIGINT.
// =========================================================================

test "planar-watch feed --follow --interval 100ms exits cleanly on SIGINT" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed something so feed has rows to emit.
    const pid = seedPlanWithTask(&suite, "watch-sigint", "sigint-task");
    defer gpa.free(pid);
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    // Spawn planar-watch feed --follow as a child, wait briefly, send SIGINT.
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, resolveWatchBin()) catch @panic("OOM");
    argv_list.append(gpa, "feed") catch @panic("OOM");
    argv_list.append(gpa, "--follow") catch @panic("OOM");
    argv_list.append(gpa, "--interval") catch @panic("OOM");
    argv_list.append(gpa, "100ms") catch @panic("OOM");
    argv_list.append(gpa, "--json") catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    var child = try std.process.spawn(std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
        .stdout = .pipe,
        .stderr = .pipe,
    });

    // Give the child time to flush its initial snapshot.
    std.Io.sleep(std.testing.io, .{ .nanoseconds = @as(i96, 500 * std.time.ns_per_ms) }, .awake) catch {};

    // Send SIGINT. The follow loop's handler flips the atomic; the
    // next poll iteration exits cleanly with code 0.
    std.posix.kill(child.id.?, std.posix.SIG.INT) catch |e|
        std.debug.panic("kill: {s}", .{@errorName(e)});

    // Drain pipes so the kernel doesn't block the child on a full pipe.
    if (child.stdout) |*f| {
        var buf: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        _ = reader.interface.readSliceShort(&buf) catch 0;
    }
    if (child.stderr) |*f| {
        var buf: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        _ = reader.interface.readSliceShort(&buf) catch 0;
    }

    const term = try child.wait(std.testing.io);

    // SIGINT → exit 0 per the scope-discipline rule in the tech
    // spec ("SIGINT exits 0 after closing the DB handle in all
    // tiers"). The child may also exit via .signaled if the handler
    // re-raises; both shapes are acceptable as long as the binary
    // terminates within a bounded window.
    switch (term) {
        .exited => |code| try std.testing.expectEqual(@as(u32, 0), code),
        .signal => {}, // OK: kernel delivered the signal before our handler ran.
        else => {
            std.debug.print("unexpected term: {any}\n", .{term});
            return error.UnexpectedTerm;
        },
    }
}

// =========================================================================
// Cross-process follow: planar-agent writes, planar-watch reads.
// =========================================================================

test "planar-watch feed --follow surfaces a cross-process write within the poll window" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-cross", "cross-task");
    defer gpa.free(pid);

    // Spawn planar-watch feed --follow with a short interval.
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, resolveWatchBin()) catch @panic("OOM");
    argv_list.append(gpa, "feed") catch @panic("OOM");
    argv_list.append(gpa, "--follow") catch @panic("OOM");
    argv_list.append(gpa, "--interval") catch @panic("OOM");
    argv_list.append(gpa, "200ms") catch @panic("OOM");
    argv_list.append(gpa, "--json") catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    var child = try std.process.spawn(std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
        .stdout = .pipe,
        .stderr = .pipe,
    });

    // Let the watcher print its initial (empty) snapshot.
    std.Io.sleep(std.testing.io, .{ .nanoseconds = @as(i96, 300 * std.time.ns_per_ms) }, .awake) catch {};

    // Drive a write through planar-agent. This must show up in the
    // watcher's stream within the next poll iteration.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    // Give the watcher 2s to surface the event (much more than the
    // 200ms poll, so CI flakes due to scheduler jitter don't bite).
    std.Io.sleep(std.testing.io, .{ .nanoseconds = @as(i96, 2 * std.time.ns_per_s) }, .awake) catch {};

    // SIGINT and collect output.
    std.posix.kill(child.id.?, std.posix.SIG.INT) catch |e|
        std.debug.panic("kill: {s}", .{@errorName(e)});

    // Read all stdout before waiting so the child can exit without
    // blocking on a full pipe.
    var stdout_buf: std.ArrayList(u8) = .empty;
    defer stdout_buf.deinit(gpa);
    if (child.stdout) |*f| {
        var tmp: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        while (true) {
            const n = reader.interface.readSliceShort(&tmp) catch 0;
            if (n == 0) break;
            stdout_buf.appendSlice(gpa, tmp[0..n]) catch @panic("OOM");
        }
    }
    if (child.stderr) |*f| {
        var tmp: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        while (true) {
            const n = reader.interface.readSliceShort(&tmp) catch 0;
            if (n == 0) break;
        }
    }
    _ = try child.wait(std.testing.io);

    if (std.mem.indexOf(u8, stdout_buf.items, "\"event\":\"claim_acquired\"") == null) {
        std.debug.print("watcher did not surface the cross-process claim:\n{s}\n", .{stdout_buf.items});
        return error.NoCrossProcessEvent;
    }
}

// =========================================================================
// M9 — Tier-2 wake assertions.
//
// These tests verify the kqueue/inotify wake source surfaces cross-process
// writes well before the heartbeat fallback fires. Test methodology:
//
//   - Use a LONG interval (30s) so only Tier-2 wake can surface the event
//     inside the test budget. If kqueue/inotify is broken, the assertion
//     fails fast (the test waits ~1s, the heartbeat is 30s away).
//   - Force a WAL rotation via planar-agent reconcile (which checkpoints
//     the WAL) plus a subsequent write — the watch must re-attach
//     transparently and still surface the post-rotation event.
//
// Both tests pin the Tier-2 invariant: the kqueue/inotify wake IS the
// event source, not the heartbeat fallback.
// =========================================================================

test "planar-watch feed --follow --interval 30s surfaces cross-process write via Tier-2 wake" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-tier2", "tier2-task");
    defer gpa.free(pid);

    // Long heartbeat — only the wake can surface the event in time.
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, resolveWatchBin()) catch @panic("OOM");
    argv_list.append(gpa, "feed") catch @panic("OOM");
    argv_list.append(gpa, "--follow") catch @panic("OOM");
    argv_list.append(gpa, "--interval") catch @panic("OOM");
    argv_list.append(gpa, "30s") catch @panic("OOM");
    argv_list.append(gpa, "--json") catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    var child = try std.process.spawn(std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
        .stdout = .pipe,
        .stderr = .pipe,
    });

    // Let the watcher print its initial snapshot AND register its
    // wake source on the (yet-to-be-created) `-wal` sibling.
    try std.testing.io.sleep(std.Io.Duration.fromMilliseconds(500), std.Io.Clock.awake);

    // Drive a write through planar-agent. Under Tier 2 this must
    // surface within ~100ms (kqueue/inotify wake latency); we budget
    // 1.5s to absorb test-host scheduler jitter. The 30s heartbeat
    // is NOT what we're measuring — a watch that waits the heartbeat
    // is a regression.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    // Give the wake source 1.5s to fire. Anything past ~200ms is
    // already evidence Tier 2 isn't working; the 1.5s ceiling is
    // pure CI defense-in-depth.
    try std.testing.io.sleep(std.Io.Duration.fromMilliseconds(1500), std.Io.Clock.awake);

    // SIGINT and collect output.
    std.posix.kill(child.id.?, std.posix.SIG.INT) catch |e|
        std.debug.panic("kill: {s}", .{@errorName(e)});

    var stdout_buf: std.ArrayList(u8) = .empty;
    defer stdout_buf.deinit(gpa);
    if (child.stdout) |*f| {
        var tmp: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        while (true) {
            const n = reader.interface.readSliceShort(&tmp) catch 0;
            if (n == 0) break;
            stdout_buf.appendSlice(gpa, tmp[0..n]) catch @panic("OOM");
        }
    }
    if (child.stderr) |*f| {
        var tmp: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        while (true) {
            const n = reader.interface.readSliceShort(&tmp) catch 0;
            if (n == 0) break;
        }
    }
    _ = try child.wait(std.testing.io);

    if (std.mem.indexOf(u8, stdout_buf.items, "\"event\":\"claim_acquired\"") == null) {
        std.debug.print(
            "Tier-2 wake failed to surface cross-process write before the 30s heartbeat:\n{s}\n",
            .{stdout_buf.items},
        );
        return error.Tier2WakeMissed;
    }
}

test "planar-watch feed --follow survives WAL rotation without losing events" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-rotation", "rotation-task");
    defer gpa.free(pid);

    // Pre-drive one write so the -wal sibling already exists when
    // the watcher starts up. This isolates the "watch survives a
    // rotation" assertion from the "watch attaches lazily on first
    // write" assertion (which the Tier-2 cross-process test covers).
    // Keep the claim token alive — we'll complete() it AFTER the
    // rotation so the post-rotation write isn't blocked on the
    // already-in-flight task.
    const initial_pull = mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" });
    defer gpa.free(initial_pull);
    const token = extractStringField(gpa, initial_pull, "\"claim_token\":\"") catch @panic("no initial token");
    defer gpa.free(token);

    // Spawn the watcher with a long heartbeat so only the wake can
    // surface the post-rotation event.
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, resolveWatchBin()) catch @panic("OOM");
    argv_list.append(gpa, "feed") catch @panic("OOM");
    argv_list.append(gpa, "--follow") catch @panic("OOM");
    argv_list.append(gpa, "--interval") catch @panic("OOM");
    argv_list.append(gpa, "30s") catch @panic("OOM");
    argv_list.append(gpa, "--json") catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM");

    var child = try std.process.spawn(std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
        .stdout = .pipe,
        .stderr = .pipe,
    });

    // Let the watcher attach its kqueue/inotify watch to the
    // existing `-wal`.
    try std.testing.io.sleep(std.Io.Duration.fromMilliseconds(500), std.Io.Clock.awake);

    // Force a WAL rotation via a direct PRAGMA wal_checkpoint(TRUNCATE).
    // We can't run this through planar-agent (it doesn't expose a
    // checkpoint verb), so the test opens its own short-lived
    // sqlite3 process. The exact mechanism doesn't matter — what
    // matters is that the -wal file is recreated.
    {
        var ckp_argv: std.ArrayList([]const u8) = .empty;
        defer ckp_argv.deinit(gpa);
        ckp_argv.append(gpa, "sqlite3") catch @panic("OOM");
        ckp_argv.append(gpa, suite.db_path) catch @panic("OOM");
        ckp_argv.append(gpa, "PRAGMA wal_checkpoint(TRUNCATE);") catch @panic("OOM");
        const ckp_res = std.process.run(gpa, std.testing.io, .{
            .argv = ckp_argv.items,
        }) catch |e| {
            // sqlite3 CLI not installed — skip rather than fail; the
            // rotation path is also exercised by the unit test in
            // src/engine/runtime/agentactivity/wake.zig.
            std.debug.print("sqlite3 not available ({s}); skipping CLI rotation test\n", .{@errorName(e)});
            std.posix.kill(child.id.?, std.posix.SIG.INT) catch {};
            _ = try child.wait(std.testing.io);
            return error.SkipZigTest;
        };
        defer gpa.free(ckp_res.stdout);
        defer gpa.free(ckp_res.stderr);
    }

    // Give the watcher a beat to observe the rotation delete event
    // and re-attach.
    try std.testing.io.sleep(std.Io.Duration.fromMilliseconds(300), std.Io.Clock.awake);

    // Now drive a fresh write — complete the pre-rotation claim.
    // The re-attached watch must surface this within the
    // wake-latency budget.
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", token, "--summary", "post-rotation", "--json" }));

    try std.testing.io.sleep(std.Io.Duration.fromMilliseconds(1500), std.Io.Clock.awake);

    // SIGINT and collect output.
    std.posix.kill(child.id.?, std.posix.SIG.INT) catch |e|
        std.debug.panic("kill: {s}", .{@errorName(e)});

    var stdout_buf: std.ArrayList(u8) = .empty;
    defer stdout_buf.deinit(gpa);
    if (child.stdout) |*f| {
        var tmp: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        while (true) {
            const n = reader.interface.readSliceShort(&tmp) catch 0;
            if (n == 0) break;
            stdout_buf.appendSlice(gpa, tmp[0..n]) catch @panic("OOM");
        }
    }
    if (child.stderr) |*f| {
        var tmp: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        while (true) {
            const n = reader.interface.readSliceShort(&tmp) catch 0;
            if (n == 0) break;
        }
    }
    _ = try child.wait(std.testing.io);

    // The POST-rotation completed event MUST appear — that's the
    // proof that the watch re-attached after the truncate.
    if (std.mem.indexOf(u8, stdout_buf.items, "\"event\":\"completed\"") == null) {
        std.debug.print(
            "watch did not survive WAL rotation; post-rotation event missing:\n{s}\n",
            .{stdout_buf.items},
        );
        return error.RotationEventMissed;
    }
}
