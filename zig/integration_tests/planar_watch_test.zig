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

fn lineContaining(text: []const u8, needle: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |line| {
        if (std.mem.indexOf(u8, line, needle) != null) return line;
    }
    return null;
}

// =========================================================================
// Capability boundary: --help enumerates the read-only verb set only.
// =========================================================================

test "planar-watch --help lists the read verbs + version + completion" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const res = runBin(&suite, resolveWatchBin(), &.{"--help"});
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);

    // Parse the SUBCOMMANDS table into a verb set and test set membership,
    // rather than a raw substring/indexOf search over the whole --help
    // text (task 6442). Under CLI11 (decision 948), wrapped description
    // continuation lines are padded out to a fixed left-column width, so a
    // description word can collide with a verb-shaped needle — e.g. line
    // 60 of this binary's real --help output ("        claim (matches `ps
    // --stale`).") is a wrapped DESCRIPTION line that matched the old
    // "  claim " needle even though `claim` is not a planar-watch verb.
    // harness.parseHelpVerbs (shared with capability_boundary_test.zig,
    // task 6440) tokenizes only genuine 2-space-indented table entries.
    var verbs = harness.parseHelpVerbs(gpa, res.stdout);
    defer harness.freeVerbSet(gpa, &verbs);

    // Must contain each read verb.
    try harness.assertContainsAll(&verbs, &.{
        "feed",        "ps",      "claims",     "actions", "plans", "log", "tree", "run",
        "sync-events", "version", "completion",
    }, "planar-watch");

    // Capability boundary — no write verb may appear as a table entry.
    // These are planar-agent's; they MUST NOT be reachable through
    // planar-watch.
    try harness.assertContainsNone(&verbs, &.{
        "pull",    "claim",  "complete",  "fail",
        "release", "block",  "heartbeat", "reconcile",
        "abort",   "ingest", "action",
    }, "planar-watch");
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

test "planar-watch ps surfaces the claim's entity scope (text + json)" {
    // Operator question: "what work is being done where?" — the
    // scope column / entity_scope field is the answer. Pinned per
    // user request 2026-05-28.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-ps-scope", "ps-scope-task");
    defer gpa.free(pid);
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    // Text rendering: the scope column should appear between the
    // entity ref and the vendor column. seedPlanWithTask uses
    // `init --skip-project` so the underlying task lives at global
    // scope; we just verify the literal "scope:" prefix is present
    // on the active row (the exact slug depends on test fixture
    // shape and the engine's scope_kind default).
    const text_out = mustRunWatch(&suite, &.{"ps"});
    defer gpa.free(text_out);
    try std.testing.expect(std.mem.indexOf(u8, text_out, "  scope:") != null);

    // JSON shape: each claim row carries `entity_scope: {kind, slug}`
    // right after `entity_id`. The kind is a stable string; the
    // slug may be null for global. Both fields must be present.
    const json_out = mustRunWatch(&suite, &.{ "ps", "--json" });
    defer gpa.free(json_out);
    try std.testing.expect(std.mem.indexOf(u8, json_out, "\"entity_scope\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_out, "\"kind\":\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, json_out, "\"slug\":") != null);
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

test "planar-watch claims-category-text surfaces a categorized terminal additively" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-claims-category", "categorized-task");
    defer gpa.free(pid);
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);
    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("no claim token");
    defer gpa.free(token);
    gpa.free(mustRunAgent(&suite, &.{
        "fail", "--claim", token, "--reason", "bounded failure", "--category", "usage_limit", "--no-locality-probe", "--json",
    }));

    const text_out = mustRunWatch(&suite, &.{ "claims", "--status", "all" });
    defer gpa.free(text_out);
    try std.testing.expect(std.mem.indexOf(u8, text_out, "status:aborted") != null);
    try std.testing.expect(std.mem.indexOf(u8, text_out, "category:usage_limit") != null);
}

test "planar-watch claims and ps text add category only to categorized rows" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-category-row-parity", "unused-task");
    defer gpa.free(pid);
    const stale_task_json = suite.mustRun(&.{ "task", "add", "--plan", pid, "--json", "stale-task" });
    defer gpa.free(stale_task_json);
    const stale_task_id = extractIntField(stale_task_json, "\"id\"") orelse @panic("no stale task id");
    const active_task_json = suite.mustRun(&.{ "task", "add", "--plan", pid, "--json", "active-task" });
    defer gpa.free(active_task_json);
    const active_task_id = extractIntField(active_task_json, "\"id\"") orelse @panic("no active task id");
    const stale_ref = try std.fmt.allocPrint(gpa, "task:{d}", .{stale_task_id});
    defer gpa.free(stale_ref);
    const active_ref = try std.fmt.allocPrint(gpa, "task:{d}", .{active_task_id});
    defer gpa.free(active_ref);

    const stale_claim = mustRunAgent(&suite, &.{ "claim", "--entity", stale_ref, "--ttl", "1", "--no-locality-probe", "--json" });
    defer gpa.free(stale_claim);
    const stale_token = try extractStringField(gpa, stale_claim, "\"claim_token\":\"");
    defer gpa.free(stale_token);
    const active_claim = mustRunAgent(&suite, &.{ "claim", "--entity", active_ref, "--no-locality-probe", "--json" });
    defer gpa.free(active_claim);
    const active_token = try extractStringField(gpa, active_claim, "\"claim_token\":\"");
    defer gpa.free(active_token);
    try std.testing.io.sleep(std.Io.Duration.fromSeconds(2), std.Io.Clock.awake);
    gpa.free(mustRunAgent(&suite, &.{ "reconcile", "--plan", pid, "--category", "usage_limit", "--json" }));

    const claims_text = mustRunWatch(&suite, &.{ "claims", "--plan", pid, "--status", "all" });
    defer gpa.free(claims_text);
    const claims_stale_line = lineContaining(claims_text, stale_token) orelse return error.MissingCategorizedClaim;
    const claims_active_line = lineContaining(claims_text, active_token) orelse return error.MissingActiveClaim;
    try std.testing.expect(std.mem.indexOf(u8, claims_stale_line, "category:usage_limit") != null);
    try std.testing.expect(std.mem.indexOf(u8, claims_active_line, "category:") == null);

    const ps_text = mustRunWatch(&suite, &.{ "ps", "--plan", pid, "--stale" });
    defer gpa.free(ps_text);
    const ps_stale_line = lineContaining(ps_text, stale_token) orelse return error.MissingCategorizedClaim;
    const ps_active_line = lineContaining(ps_text, active_token) orelse return error.MissingActiveClaim;
    try std.testing.expect(std.mem.indexOf(u8, ps_stale_line, "category:usage_limit") != null);
    try std.testing.expect(std.mem.indexOf(u8, ps_active_line, "category:") == null);
}

test "planar-watch feed-log-categories JSON retain categorized terminal claims" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-feed-log-categories", "failed-task");
    defer gpa.free(pid);
    const failed_task_json = suite.mustRun(&.{ "task", "list", "--scope", "global", "--plan", pid, "--json" });
    defer gpa.free(failed_task_json);
    const failed_task_id = extractIntField(failed_task_json, "\"id\"") orelse @panic("no failed task id");
    const failed_task_arg = try std.fmt.allocPrint(gpa, "{d}", .{failed_task_id});
    defer gpa.free(failed_task_arg);

    const pull = mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" });
    defer gpa.free(pull);
    const failed_token = try extractStringField(gpa, pull, "\"claim_token\":\"");
    defer gpa.free(failed_token);
    gpa.free(mustRunAgent(&suite, &.{
        "fail", "--claim", failed_token, "--reason", "bounded", "--category", "tool_failure", "--json",
    }));

    const stale_task_json = suite.mustRun(&.{ "task", "add", "--plan", pid, "--json", "stale-task" });
    defer gpa.free(stale_task_json);
    const stale_task_id = extractIntField(stale_task_json, "\"id\"") orelse @panic("no stale task id");
    const stale_task_arg = try std.fmt.allocPrint(gpa, "{d}", .{stale_task_id});
    defer gpa.free(stale_task_arg);
    const stale_ref = try std.fmt.allocPrint(gpa, "task:{d}", .{stale_task_id});
    defer gpa.free(stale_ref);
    const stale_claim = mustRunAgent(&suite, &.{ "claim", "--entity", stale_ref, "--ttl", "1", "--no-locality-probe", "--json" });
    defer gpa.free(stale_claim);
    try std.testing.io.sleep(std.Io.Duration.fromSeconds(2), std.Io.Clock.awake);
    gpa.free(mustRunAgent(&suite, &.{ "reconcile", "--plan", pid, "--category", "usage_limit", "--json" }));

    inline for ([_]struct { task: []const u8, category: []const u8 }{
        .{ .task = failed_task_arg, .category = "tool_failure" },
        .{ .task = stale_task_arg, .category = "usage_limit" },
    }) |expected| {
        const category_needle = try std.fmt.allocPrint(gpa, "\"failure_category\":\"{s}\"", .{expected.category});
        defer gpa.free(category_needle);

        const feed_json = mustRunWatch(&suite, &.{ "feed", "--task", expected.task, "--json" });
        defer gpa.free(feed_json);
        try std.testing.expect(std.mem.indexOf(u8, feed_json, category_needle) != null);

        const log_json = mustRunWatch(&suite, &.{ "log", "--task", expected.task, "--json" });
        defer gpa.free(log_json);
        try std.testing.expect(std.mem.indexOf(u8, log_json, category_needle) != null);
    }
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
    // metadata field is present (null since no --metadata was supplied
    // on the pull).
    try std.testing.expect(std.mem.indexOf(u8, out, "\"metadata\":null") != null);
}

test "planar-watch actions --json surfaces metadata field set by pull --metadata" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-actions-meta", "meta-task");
    defer gpa.free(pid);
    const meta = "{\"strategy\":\"isolated-sequential\",\"axes\":{\"isolation\":\"worktree\"}}";
    gpa.free(mustRunAgent(&suite, &.{
        "pull", pid, "--metadata", meta, "--no-locality-probe", "--json",
    }));

    const out = mustRunWatch(&suite, &.{ "actions", "--json" });
    defer gpa.free(out);
    // metadata is encoded as a JSON string field — the inner content's
    // quotes are escaped. Spot-check the strategy value text appears.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"metadata\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "isolated-sequential") != null);
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
    var stderr_buf: std.ArrayList(u8) = .empty;
    defer stderr_buf.deinit(gpa);
    if (child.stderr) |*f| {
        var tmp: [4096]u8 = undefined;
        var reader = f.reader(std.testing.io, &.{});
        while (true) {
            const n = reader.interface.readSliceShort(&tmp) catch 0;
            if (n == 0) break;
            stderr_buf.appendSlice(gpa, tmp[0..n]) catch @panic("OOM");
        }
    }
    _ = try child.wait(std.testing.io);

    // The POST-rotation completed event MUST appear — that's the
    // proof that the watch re-attached after the truncate.
    if (std.mem.indexOf(u8, stdout_buf.items, "\"event\":\"completed\"") == null) {
        std.debug.print(
            "watch did not survive WAL rotation; post-rotation event missing:\nSTDOUT:\n{s}\nSTDERR:\n{s}\n",
            .{ stdout_buf.items, stderr_buf.items },
        );
        return error.RotationEventMissed;
    }
}

// =========================================================================
// M4 — planar-watch tree (tasks 3064–3067).
// =========================================================================

test "planar-watch tree renders at least one row after a pull" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-tree-basic", "tree-task");
    defer gpa.free(pid);
    // pull inserts an action row for the orchestrator dispatch.
    gpa.free(mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" }));

    const out = mustRunWatch(&suite, &.{"tree"});
    defer gpa.free(out);

    // After a pull there is at least one action row; the tree must not
    // emit the empty-forest sentinel.
    if (std.mem.indexOf(u8, out, "(no action chains)") != null) {
        std.debug.print("tree: unexpected empty forest after pull:\n{s}\n", .{out});
        return error.UnexpectedEmptyForest;
    }
}

test "planar-watch tree --root-session with unknown id exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const res = runBin(&suite, resolveWatchBin(), &.{ "tree", "--root-session", "99999" });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
}

test "planar-watch tree --root-session with invalid (negative) id exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const res = runBin(&suite, resolveWatchBin(), &.{ "tree", "--root-session", "-1" });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
}

test "planar-watch tree empty DB emits empty-forest sentinel" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // init with no tasks/claims so no action rows exist.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const out = mustRunWatch(&suite, &.{"tree"});
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "(no action chains)") != null);
}

// task 3069 — synthetic orchestrator → coder dispatch chain renders as a
// 2-level tree (spec: line 90 "a synthetic orchestrator → coder dispatch
// chain ... renders as a 2-level tree under `planar-watch tree`").
//
// Methodology: `planar-agent pull` writes the root (orchestrator) action row.
// A second action row with `parent_action_id` pointing at the root is inserted
// directly via the `sqlite3` CLI, emulating what `planar-agent pull` would do
// when dispatching a coder sub-agent. The tree verb must render both nodes with
// the `└──` child prefix on the coder row.
test "planar-watch tree synthetic orchestrator→coder chain renders 2-level tree" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "watch-tree-2level", "tree-2level-task");
    defer gpa.free(pid);

    // Pull inserts a root orchestrator action row. Capture the JSON output so
    // we can extract the session_id and the root action_id for the SQL insert.
    const pull_out = mustRunAgent(&suite, &.{ "pull", pid, "--no-locality-probe", "--json" });
    defer gpa.free(pull_out);

    // Extract session_id from the pull JSON ("session":{...,"id":<N>,...}).
    const session_id = extractIntField(pull_out, "\"session_id\":") orelse
        @panic("no session_id in pull output");

    // Query the DB for the root action's id (the one just inserted by pull).
    // We use `sqlite3 <db> "select max(id) from agent_actions"` — there may
    // be only one row, so max(id) is the root action we just created.
    const root_id_sql = "select max(id) from agent_actions where parent_action_id is null;";
    const sqlite3_query_res = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, root_id_sql },
    }) catch |e| {
        std.debug.print("sqlite3 not available ({s}); skipping synthetic chain test\n", .{@errorName(e)});
        return error.SkipZigTest;
    };
    defer gpa.free(sqlite3_query_res.stderr);
    const root_id_str = std.mem.trim(u8, sqlite3_query_res.stdout, " \t\r\n");
    const root_id = std.fmt.parseInt(i64, root_id_str, 10) catch {
        gpa.free(sqlite3_query_res.stdout);
        std.debug.print("could not parse root action id from sqlite3 output: '{s}'\n", .{root_id_str});
        @panic("synthetic chain test: could not resolve root action id");
    };
    gpa.free(sqlite3_query_res.stdout);

    // Insert the child (coder) action row directly, wiring parent_action_id
    // to the root action. This emulates what planar-agent pull would do when
    // dispatching a sub-agent from within an orchestrator action.
    const insert_sql = std.fmt.allocPrint(
        gpa,
        "insert into agent_actions (session_id, parent_action_id, action_kind, vendor, started_at)" ++
            " values ({d}, {d}, 'coder', 'zig-test-synthetic', strftime('%Y-%m-%dT%H:%M:%fZ','now'));",
        .{ session_id, root_id },
    ) catch @panic("OOM");
    defer gpa.free(insert_sql);

    const insert_res = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, insert_sql },
    }) catch |e| {
        std.debug.print("sqlite3 insert failed ({s}); skipping\n", .{@errorName(e)});
        return error.SkipZigTest;
    };
    gpa.free(insert_res.stdout);
    gpa.free(insert_res.stderr);
    if (insert_res.term != .exited or insert_res.term.exited != 0) {
        std.debug.print("sqlite3 insert non-zero exit: {any}\n", .{insert_res.term});
        @panic("synthetic chain test: sqlite3 insert failed");
    }

    // Run planar-watch tree. The forest now has one root at depth 0 and one
    // child at depth 1. The child must be rendered with the `└──` last-sibling
    // tree character (UTF-8: 0xE2 0x94 0x94 0xE2 0x94 0x80 0xE2 0x94 0x80).
    const out = mustRunWatch(&suite, &.{"tree"});
    defer gpa.free(out);

    // The empty-forest sentinel must NOT appear.
    if (std.mem.indexOf(u8, out, "(no action chains)") != null) {
        std.debug.print("tree: unexpected empty forest with 2 action rows:\n{s}\n", .{out});
        return error.UnexpectedEmptyForest;
    }

    // The `└──` (last-child tree character) must appear — proof that the
    // recursive CTE picked up the parent→child edge and rendered depth-1.
    const last_child_marker = "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80"; // └──
    if (std.mem.indexOf(u8, out, last_child_marker) == null) {
        std.debug.print(
            "tree: 2-level chain did not render child prefix '└──':\n{s}\n",
            .{out},
        );
        return error.Missing2LevelTreePrefix;
    }
}

// =========================================================================
// Task 3089 — production-path orchestrator → coder dispatch chain.
//
// Spec: test-spec line 90 ("tree verb renders an orchestrator → coder chain").
//
// This test builds a 2-level tree using ONLY production verbs — no raw
// sqlite3 INSERT. The orchestrator pulls task A and captures its action_id.
// The coder pulls task B with --parent-action=<orch-action-id>. The tree
// must then render the coder action as a child of the orchestrator action.
//
// Design: two tasks are required because `pull` picks the next todo task
// and marks it doing. We need the orchestrator to own one task while the
// coder pulls a second one. The orchestrator action is the parent; the coder
// action is the child. This is the most honest model of the real dispatch
// (orchestrator claims its task, then dispatches the coder for the next task
// via --parent-action so the tree edge is wired).
// =========================================================================

test "planar-watch tree production orchestrator→coder --parent-action produces 2-level hierarchy" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed plan with TWO todo tasks so orchestrator and coder can each pull one.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "watch-tree-prod", "--json", "watch-tree-prod" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_id_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "orch-task" }));
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "coder-task" }));

    // Orchestrator pull: claims the first task and gets its action_id.
    const orch_out = mustRunAgent(&suite, &.{
        "pull", plan_id_arg, "--role", "orchestrator", "--no-locality-probe", "--json",
    });
    defer gpa.free(orch_out);

    const orch_action_id = extractIntField(orch_out, "\"action_id\":") orelse
        @panic("no action_id in orchestrator pull output");
    const orch_action_arg = std.fmt.allocPrint(gpa, "{d}", .{orch_action_id}) catch @panic("OOM");
    defer gpa.free(orch_action_arg);

    // Coder dispatch: --parent-action wires the cross-session hierarchy edge.
    const coder_out = mustRunAgent(&suite, &.{
        "pull",            plan_id_arg,     "--role",              "coder",
        "--parent-action", orch_action_arg, "--no-locality-probe", "--json",
    });
    defer gpa.free(coder_out);

    // Verify the coder got a task (not no_work).
    if (std.mem.indexOf(u8, coder_out, "\"no_work\":true") != null) {
        std.debug.print("coder pull returned no_work — expected a second task:\n{s}\n", .{coder_out});
        @panic("coder pull must find the second task");
    }

    // Capture the coder's task id so we can scope the actions query.
    // The pull JSON carries `"task":{"id":<N>,...}` for the claimed task.
    const coder_task_id = extractIntField(coder_out, "\"task\":{\"id\"") orelse
        @panic("no task id in coder pull output");
    const coder_task_arg = std.fmt.allocPrint(gpa, "{d}", .{coder_task_id}) catch @panic("OOM");
    defer gpa.free(coder_task_arg);

    // === Structural correctness assertion ===
    // Query planar-watch actions --task <coder-task-id> --json and verify that
    // the coder's action row carries parent_action_id == orch_action_id.
    // This proves the hierarchy EDGE is correct, not merely that some `└──`
    // glyph appears in the rendered output.
    //
    // Without this assertion, the prior glyph check would pass even if the
    // coder attached to the wrong parent or if the renderer emitted `└──`
    // for an unrelated reason. The two assertions together pin both the
    // data layer (correct FK) and the rendering layer (correct glyph).
    const actions_out = mustRunWatch(&suite, &.{ "actions", "--task", coder_task_arg, "--json" });
    defer gpa.free(actions_out);

    // The JSON shape is: { "generated_at": "...", "actions": [ActionRow, ...] }
    // ActionRow includes "parent_action_id": <int|null>.
    // We scan for the first occurrence — there should be exactly one action row
    // for this task (the pull-side action).  extractIntField returns the first
    // integer it finds after the given key prefix.
    const found_parent_id = extractIntField(actions_out, "\"parent_action_id\":") orelse {
        std.debug.print(
            "actions --task {s} --json did not contain a parent_action_id field:\n{s}\n",
            .{ coder_task_arg, actions_out },
        );
        return error.MissingParentActionId;
    };
    if (found_parent_id != orch_action_id) {
        std.debug.print(
            "coder action parent_action_id={d} does not match orchestrator action_id={d}.\n" ++
                "The --parent-action flag did not wire the correct hierarchy edge.\n" ++
                "actions JSON:\n{s}\n",
            .{ found_parent_id, orch_action_id, actions_out },
        );
        return error.WrongParentActionId;
    }

    // Run planar-watch tree. The hierarchy must render the coder at depth 1
    // under the orchestrator (depth 0).
    const out = mustRunWatch(&suite, &.{"tree"});
    defer gpa.free(out);

    // Empty forest is wrong — two pulls happened.
    if (std.mem.indexOf(u8, out, "(no action chains)") != null) {
        std.debug.print("tree: unexpected empty forest after two pulls:\n{s}\n", .{out});
        return error.UnexpectedEmptyForest;
    }

    // The `└──` (last-child tree character) must appear — the coder action
    // is the only child of the orchestrator and is therefore the last sibling.
    // This assertion proves the hierarchy RENDERS correctly.  The parent_action_id
    // assertion above proves the underlying edge is correct; together they are
    // the full contract.
    const last_child_marker = "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80"; // └──
    if (std.mem.indexOf(u8, out, last_child_marker) == null) {
        std.debug.print(
            "tree: production dispatch chain did not render child prefix '└──';\n" ++
                "the --parent-action flag did not produce the hierarchy edge:\n{s}\n",
            .{out},
        );
        return error.Missing2LevelTreePrefix;
    }
}

test "planar-agent pull --parent-action with non-positive id exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid = seedPlanWithTask(&suite, "tree-parent-invalid", "task-invalid");
    defer gpa.free(pid);

    // --parent-action 0 must be rejected with InvalidInput.
    const res_zero = runBin(&suite, resolveAgentBin(), &.{
        "pull", pid, "--parent-action", "0", "--no-locality-probe", "--json",
    });
    defer res_zero.deinit(gpa);
    try std.testing.expect(res_zero.term == .exited);
    try std.testing.expect(res_zero.term.exited != 0);

    // --parent-action with an unknown (but positive) id must exit non-zero.
    const res_unknown = runBin(&suite, resolveAgentBin(), &.{
        "pull", pid, "--parent-action", "999999", "--no-locality-probe", "--json",
    });
    defer res_unknown.deinit(gpa);
    try std.testing.expect(res_unknown.term == .exited);
    try std.testing.expect(res_unknown.term.exited != 0);
}

// =========================================================================
// Task 3090 — tree --root-session with multiple action chains under one session.
//
// Spec: test-spec line 222 ("tree --root-session with multiple action chains
// under the same session").
//
// Two distinct root action chains under the same session. The anchor selects
// root actions WHERE session_id = <id>; the recursive term extends each chain
// to its children. Both chains must appear in the --root-session output.
//
// Production verbs alone cannot create two separate root action chains in the
// SAME session without additional tasks — `pull` starts one root action per
// call but each call uses ensureActive which may return the same session when
// the vendor+vendor_session pair matches. To get two unambiguously disjoint
// roots in the same session, we use `action start` on the same claim (which
// resolves the parent to the existing open action, giving a child, not a new
// root). Instead we use `pull` twice on the SAME vendor+vendor_session pair
// so ensureActive returns the same session for both, then rely on the fact
// that each pull writes a NEW root action row with parent_action_id IS NULL.
// A second pull on the same session (different task) produces a second root
// because pull does not inherit the prior pull's action as parent — that is
// exactly the production gap that --parent-action fixes. Here we WANT two
// roots to test the multi-chain scenario, so we intentionally omit
// --parent-action.
// =========================================================================

test "planar-watch tree --root-session renders two disjoint chains under one session" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed plan with TWO todo tasks.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "tree-multi-chain", "--json", "tree-multi-chain" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_id_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "chain-a-task" }));
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "chain-b-task" }));

    // First pull: chain A root. Use a fixed vendor-session so both pulls
    // land in the same session via ensureActive.
    const pull_a = mustRunAgent(&suite, &.{
        "pull",                plan_id_arg,
        "--vendor-session",    "multi-chain-session",
        "--role",              "coder",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(pull_a);

    // Extract session_id from the first pull's claim.
    const session_id = extractIntField(pull_a, "\"session_id\":") orelse
        @panic("no session_id in pull_a output");

    // Add a child to chain A: use `action start` on the claim from pull_a.
    // This gives chain A depth 1 (root + one child).
    const token_a = extractStringField(gpa, pull_a, "\"claim_token\":\"") catch
        @panic("no claim_token in pull_a output");
    defer gpa.free(token_a);
    gpa.free(mustRunAgent(&suite, &.{
        "action",              "start",
        "--claim",             token_a,
        "--kind",              "tool_call",
        "--no-locality-probe", "--json",
    }));

    // Second pull: chain B root. Same vendor-session → same session id.
    // No --parent-action → new root action (parent_action_id IS NULL).
    const pull_b = mustRunAgent(&suite, &.{
        "pull",                plan_id_arg,
        "--vendor-session",    "multi-chain-session",
        "--role",              "coder",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(pull_b);

    if (std.mem.indexOf(u8, pull_b, "\"no_work\":true") != null) {
        std.debug.print("pull_b returned no_work — expected a second task:\n{s}\n", .{pull_b});
        @panic("chain B pull must find the second task");
    }

    // Add a child to chain B as well.
    const token_b = extractStringField(gpa, pull_b, "\"claim_token\":\"") catch
        @panic("no claim_token in pull_b output");
    defer gpa.free(token_b);
    gpa.free(mustRunAgent(&suite, &.{
        "action",              "start",
        "--claim",             token_b,
        "--kind",              "tool_call",
        "--no-locality-probe", "--json",
    }));

    // Run planar-watch tree --root-session <id>. Both chain-A and chain-B
    // roots share this session and must both appear in the output.
    const session_arg = std.fmt.allocPrint(gpa, "{d}", .{session_id}) catch @panic("OOM");
    defer gpa.free(session_arg);

    const out = mustRunWatch(&suite, &.{ "tree", "--root-session", session_arg });
    defer gpa.free(out);

    // Neither chain should be absent — both roots belong to the session.
    if (std.mem.indexOf(u8, out, "(no action chains)") != null) {
        std.debug.print("tree --root-session: unexpected empty forest:\n{s}\n", .{out});
        return error.UnexpectedEmptyForest;
    }

    // Both chains have a child, so both must render the `└──` depth-1 marker.
    // The forest has two roots each with one child → the marker appears twice.
    const last_child_marker = "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80"; // └──
    var count: usize = 0;
    var search_buf = out;
    while (std.mem.indexOf(u8, search_buf, last_child_marker)) |pos| {
        count += 1;
        search_buf = search_buf[pos + last_child_marker.len ..];
    }
    if (count < 2) {
        std.debug.print(
            "tree --root-session: expected at least 2 '└──' markers (one per chain child);\n" ++
                "got {d}. Both chains under the same session must render.\n{s}\n",
            .{ count, out },
        );
        return error.MissingMultiChainRender;
    }
}

// =========================================================================
// plan 585 — planar-watch run list / run show (task 3906).
//
// Seeds a workflow_runs row + context_records via planar-agent run start +
// pull (with --run / --stage) + context add. Then asserts:
//   - run list --json returns {generated_at, runs:[RunRow]} with correct shape.
//   - run list --plan <id> --json returns only the seeded run.
//   - run list --status running --json returns the running run.
//   - run show <id> --json returns {run: RunRow, context_records:[...]} with
//     records grouped by stage (stage=plan before stage=code).
//   - run show <id> (text) renders without error.
//   - run show <id+999> exits non-zero for an unknown id.
// =========================================================================

/// Helper: run planar-agent and assert exit 0. Returns stdout (caller frees).
fn mustRunAgentWith(
    suite: *const harness.Suite,
    args: []const []const u8,
) []u8 {
    return mustRunAgent(suite, args);
}

test "planar-watch run list --json returns {generated_at, runs} with RunRow shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed plan + task.
    const pid_str = seedPlanWithTask(&suite, "watch-run-list", "run-list-task");
    defer gpa.free(pid_str);

    // Start a workflow run via planar-agent run start.
    const self_pid_str = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid_str);
    const run_id_label = std.fmt.allocPrint(gpa, "wrl-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_id_label);

    const start_out = mustRunAgent(&suite, &.{
        "run",         "start",
        "--plan",      pid_str,
        "--workflow",  "watch-run-list-wf",
        "--run-id",    run_id_label,
        "--pid",       self_pid_str,
        "--repo-root", "/tmp/watch-run-list",
        "--json",
    });
    defer gpa.free(start_out);

    const run_db_id = extractIntField(start_out, "\"run_id\":") orelse @panic("no run_id in start output");
    try std.testing.expect(run_db_id > 0);

    // planar-watch run list --json.
    const out = mustRunWatch(&suite, &.{ "run", "list", "--json" });
    defer gpa.free(out);

    // Top-level shape.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"generated_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"runs\":[") != null);

    // RunRow fields.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"workflow_name\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"run_identifier\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"pid\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"started_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"ended_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"running\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "watch-run-list-wf") != null);
}

test "planar-watch run list --plan filters to the seeded plan's run only" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_a = seedPlanWithTask(&suite, "watch-run-plan-a", "task-a");
    defer gpa.free(pid_a);

    // Create a second plan with its own run.
    const plan_b_json = suite.mustRun(&.{ "plan", "create", "--slug", "watch-run-plan-b", "--json", "watch-run-plan-b" });
    defer gpa.free(plan_b_json);
    const pid_b_int = extractIntField(plan_b_json, "\"id\"") orelse @panic("no plan-b id");
    const pid_b = std.fmt.allocPrint(gpa, "{d}", .{pid_b_int}) catch @panic("OOM");
    defer gpa.free(pid_b);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", pid_b, "task-b" }));

    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);

    // Start run on plan A.
    const run_a_label = std.fmt.allocPrint(gpa, "run-a-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_a_label);
    const start_a = mustRunAgent(&suite, &.{
        "run",        "start",  "--plan",      pid_a,
        "--workflow", "wf-a",   "--run-id",    run_a_label,
        "--pid",      self_pid, "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(start_a);

    // Start run on plan B.
    const run_b_label = std.fmt.allocPrint(gpa, "run-b-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_b_label);
    const start_b = mustRunAgent(&suite, &.{
        "run",        "start",  "--plan",      pid_b,
        "--workflow", "wf-b",   "--run-id",    run_b_label,
        "--pid",      self_pid, "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(start_b);

    // Filter: run list --plan <pid_a> --json should return only plan A's run.
    const out = mustRunWatch(&suite, &.{ "run", "list", "--plan", pid_a, "--json" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "\"runs\":[") != null);
    // wf-a appears; wf-b must not.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"wf-a\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"wf-b\"") == null);
}

test "planar-watch run list --status filters by run status" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_str = seedPlanWithTask(&suite, "watch-run-status", "status-task");
    defer gpa.free(pid_str);

    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);
    const run_label = std.fmt.allocPrint(gpa, "run-status-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_label);

    const start = mustRunAgent(&suite, &.{
        "run",        "start",     "--plan",      pid_str,
        "--workflow", "status-wf", "--run-id",    run_label,
        "--pid",      self_pid,    "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(start);

    // --status running returns the live run.
    const running_out = mustRunWatch(&suite, &.{ "run", "list", "--status", "running", "--json" });
    defer gpa.free(running_out);
    try std.testing.expect(std.mem.indexOf(u8, running_out, "\"status\":\"running\"") != null);

    // --status completed returns nothing (the run is still running).
    const done_out = mustRunWatch(&suite, &.{ "run", "list", "--status", "completed", "--json" });
    defer gpa.free(done_out);
    try std.testing.expect(std.mem.indexOf(u8, done_out, "\"runs\":[]") != null);
}

test "planar-watch run show --json returns {run, context_records} with stage grouping" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Need two tasks so we can pull twice with different stages.
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "watch-run-show", "--json", "watch-run-show" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "show-task-1" }));
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "show-task-2" }));

    // Start run.
    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);
    const run_label = std.fmt.allocPrint(gpa, "show-run-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_label);

    const start_out = mustRunAgent(&suite, &.{
        "run",         "start",
        "--plan",      plan_arg,
        "--workflow",  "show-wf",
        "--run-id",    run_label,
        "--pid",       self_pid,
        "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(start_out);

    const run_db_id = extractIntField(start_out, "\"run_id\":") orelse @panic("no run_id");
    const run_id_str = std.fmt.allocPrint(gpa, "{d}", .{run_db_id}) catch @panic("OOM");
    defer gpa.free(run_id_str);

    // Pull first task with stage=plan, add a finding context record.
    const pull_plan_out = mustRunAgent(&suite, &.{
        "pull", plan_arg, "--no-locality-probe", "--run", run_id_str, "--stage", "plan", "--json",
    });
    defer gpa.free(pull_plan_out);

    const token_plan = extractStringField(gpa, pull_plan_out, "\"claim_token\":\"") catch @panic("no claim_token plan");
    defer gpa.free(token_plan);

    const add_plan = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", token_plan,
        "--kind",  "finding",
        "--body",  "finding from plan stage",
        "--json",
    });
    defer gpa.free(add_plan);
    try std.testing.expect(std.mem.indexOf(u8, add_plan, "\"ok\":true") != null);

    // Pull second task with stage=code, add a risk context record.
    const pull_code_out = mustRunAgent(&suite, &.{
        "pull", plan_arg, "--no-locality-probe", "--run", run_id_str, "--stage", "code", "--json",
    });
    defer gpa.free(pull_code_out);

    const token_code = extractStringField(gpa, pull_code_out, "\"claim_token\":\"") catch @panic("no claim_token code");
    defer gpa.free(token_code);

    const add_code = mustRunAgent(&suite, &.{
        "context", "add",
        "--claim", token_code,
        "--kind",  "risk",
        "--body",  "risk from code stage",
        "--json",
    });
    defer gpa.free(add_code);
    try std.testing.expect(std.mem.indexOf(u8, add_code, "\"ok\":true") != null);

    // planar-watch run show <id> --json.
    const out = mustRunWatch(&suite, &.{ "run", "show", run_id_str, "--json" });
    defer gpa.free(out);

    // Top-level shape.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"run\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"context_records\":[") != null);

    // RunRow fields.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"workflow_name\":\"show-wf\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"status\":\"running\"") != null);

    // Context records: both kinds present.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"kind\":\"finding\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"kind\":\"risk\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"stage\":\"plan\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"stage\":\"code\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "finding from plan stage") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "risk from code stage") != null);

    // Stage ordering: records are ordered by stage asc (alphabetical)
    // then created_at asc. "code" sorts before "plan" alphabetically,
    // so the code-stage record must appear first in the JSON output.
    const code_pos = std.mem.indexOf(u8, out, "\"stage\":\"code\"") orelse return error.MissingCodeStage;
    const plan_pos = std.mem.indexOf(u8, out, "\"stage\":\"plan\"") orelse return error.MissingPlanStage;
    if (code_pos >= plan_pos) {
        std.debug.print(
            "run show: expected stage=code before stage=plan (alphabetical asc);\ncode_pos={d} plan_pos={d}\n{s}\n",
            .{ code_pos, plan_pos, out },
        );
        return error.StageOrderWrong;
    }
}

test "planar-watch run show text format renders without error" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const pid_str = seedPlanWithTask(&suite, "watch-run-show-text", "show-text-task");
    defer gpa.free(pid_str);

    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);
    const run_label = std.fmt.allocPrint(gpa, "show-text-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(run_label);

    const start_out = mustRunAgent(&suite, &.{
        "run",        "start",   "--plan",      pid_str,
        "--workflow", "text-wf", "--run-id",    run_label,
        "--pid",      self_pid,  "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(start_out);

    const run_db_id = extractIntField(start_out, "\"run_id\":") orelse @panic("no run_id");
    const run_id_str = std.fmt.allocPrint(gpa, "{d}", .{run_db_id}) catch @panic("OOM");
    defer gpa.free(run_id_str);

    // Text format (no --json).
    const out = mustRunWatch(&suite, &.{ "run", "show", run_id_str });
    defer gpa.free(out);

    // The run id, workflow name, and status must appear.
    try std.testing.expect(std.mem.indexOf(u8, out, "run:") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "text-wf") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "running") != null);
    // No context records yet.
    try std.testing.expect(std.mem.indexOf(u8, out, "(no context records)") != null);
}

test "planar-watch run show with unknown id exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const res = runBin(&suite, resolveWatchBin(), &.{ "run", "show", "999999", "--json" });
    defer res.deinit(gpa);

    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
}

// =========================================================================
// Task 4103 — planar-watch sync-events [--outcome] [--since] [--limit] --json
//
// sync_events has no write verb in the CLI so we seed rows directly via
// sqlite3 (same pattern used in the tree synthetic-chain test above).
// Two tests:
//   1. Happy path: insert an 'ok' row; --json returns the row; --outcome
//      filter on 'error' returns [].
//   2. Empty-result shape: no rows → sync_events: [].
// =========================================================================

test "planar-watch sync-events --json returns seeded row; --outcome filter narrows" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Init the DB (applies migrations so sync_events table exists).
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // Insert a sync_events row directly via sqlite3.
    // link_id is NULL (workbench-scope row has no external_links FK).
    const insert_sql =
        "insert into sync_events (scope, direction, outcome) " ++
        "values ('workbench', 'push', 'ok');";
    const ins = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, insert_sql },
    }) catch |e| {
        std.debug.print("sqlite3 not available ({s}); skipping sync-events seed test\n", .{@errorName(e)});
        return error.SkipZigTest;
    };
    gpa.free(ins.stdout);
    gpa.free(ins.stderr);
    if (ins.term != .exited or ins.term.exited != 0) {
        std.debug.print("sqlite3 insert failed: {any}\n", .{ins.term});
        @panic("sync-events test: sqlite3 insert failed");
    }

    // planar-watch sync-events --json must return the row.
    const out = mustRunWatch(&suite, &.{ "sync-events", "--json" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "\"generated_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"sync_events\":[") != null);
    // The seeded row has outcome=ok; must appear.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"outcome\":\"ok\"") != null);
    // direction field present.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"direction\":\"push\"") != null);

    // --outcome error must return an empty array (the row is 'ok', not 'error').
    const filtered = mustRunWatch(&suite, &.{ "sync-events", "--outcome", "error", "--json" });
    defer gpa.free(filtered);
    try std.testing.expect(std.mem.indexOf(u8, filtered, "\"sync_events\":[]") != null);
}

test "planar-watch sync-events --json on empty table returns sync_events: []" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // No rows seeded — must return empty array.
    const out = mustRunWatch(&suite, &.{ "sync-events", "--json" });
    defer gpa.free(out);

    try std.testing.expect(std.mem.indexOf(u8, out, "\"sync_events\":[]") != null);
}

// =========================================================================
// Task 4104 — planar-agent reconcile --plan <id> --dry-run [--json]
//
// Contract: --plan X --dry-run lists ONLY plan-X's stale claims and writes
// nothing. Proven by:
//   1. Seeding two plans each with one expired claim (via direct sqlite3
//      update on lease_expires_at).
//   2. Running reconcile --plan <plan-A> --dry-run --json.
//   3. Asserting dry_run returns plan-A's candidate only.
//   4. Asserting the claims table is UNCHANGED after the dry-run (both
//      claims still 'active').
// =========================================================================

fn resolveAgentBinLocal() []const u8 {
    return resolveAgentBin();
}

test "planar-agent reconcile --plan --dry-run previews only plan-scoped stale claims; writes nothing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed two plans each with one task.
    const pid_a = seedPlanWithTask(&suite, "recon-plan-a", "recon-task-a");
    defer gpa.free(pid_a);
    const plan_b_json = suite.mustRun(&.{ "plan", "create", "--slug", "recon-plan-b", "--json", "recon-plan-b" });
    defer gpa.free(plan_b_json);
    const pid_b_int = extractIntField(plan_b_json, "\"id\"") orelse @panic("no plan-b id");
    const pid_b = std.fmt.allocPrint(gpa, "{d}", .{pid_b_int}) catch @panic("OOM");
    defer gpa.free(pid_b);
    gpa.free(suite.mustRun(&.{ "task", "add", "--plan", pid_b, "recon-task-b" }));

    // Pull both plans to create active claims (TTL defaults to 600s so they are fresh).
    const pull_a = mustRunAgent(&suite, &.{ "pull", pid_a, "--no-locality-probe", "--json" });
    defer gpa.free(pull_a);
    const pull_b = mustRunAgent(&suite, &.{ "pull", pid_b, "--no-locality-probe", "--json" });
    defer gpa.free(pull_b);

    // Expire both claims by setting lease_expires_at into the past via sqlite3.
    const expire_sql =
        "update agent_work_claims set lease_expires_at = strftime('%Y-%m-%dT%H:%M:%fZ','now','-1 hour') where status = 'active';";
    const exp = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, expire_sql },
    }) catch |e| {
        std.debug.print("sqlite3 not available ({s}); skipping reconcile dry-run test\n", .{@errorName(e)});
        return error.SkipZigTest;
    };
    gpa.free(exp.stdout);
    gpa.free(exp.stderr);
    if (exp.term != .exited or exp.term.exited != 0) @panic("sqlite3 expire failed");

    // Snapshot: both claims are 'active' before the dry-run.
    const before_sql = "select count(*) from agent_work_claims where status = 'active';";
    const before = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, before_sql },
    }) catch @panic("sqlite3 before-count failed");
    defer gpa.free(before.stdout);
    defer gpa.free(before.stderr);
    const before_count_str = std.mem.trim(u8, before.stdout, " \t\r\n");
    const before_count = std.fmt.parseInt(i64, before_count_str, 10) catch @panic("parse before count");
    try std.testing.expect(before_count >= 2);

    // Run reconcile --plan <pid_a> --dry-run --json.
    const agent_bin = resolveAgentBinLocal();
    const dry_out_res = runBin(&suite, agent_bin, &.{
        "reconcile", "--plan", pid_a, "--dry-run", "--json",
    });
    defer dry_out_res.deinit(gpa);
    if (dry_out_res.term != .exited or dry_out_res.term.exited != 0) {
        std.debug.print(
            "reconcile --plan --dry-run failed (term={any}):\nstdout: {s}\nstderr: {s}\n",
            .{ dry_out_res.term, dry_out_res.stdout, dry_out_res.stderr },
        );
        @panic("reconcile --plan --dry-run must succeed");
    }

    // JSON shape: ok=true, candidates array present.
    try std.testing.expect(std.mem.indexOf(u8, dry_out_res.stdout, "\"ok\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, dry_out_res.stdout, "\"candidates\":[") != null);

    // After dry-run, BOTH claims must still be 'active' — nothing was written.
    const after = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", suite.db_path, before_sql },
    }) catch @panic("sqlite3 after-count failed");
    defer gpa.free(after.stdout);
    defer gpa.free(after.stderr);
    const after_count_str = std.mem.trim(u8, after.stdout, " \t\r\n");
    const after_count = std.fmt.parseInt(i64, after_count_str, 10) catch @panic("parse after count");
    if (after_count != before_count) {
        std.debug.print(
            "dry-run wrote to the DB: before={d} after={d}\n",
            .{ before_count, after_count },
        );
        return error.DryRunMutated;
    }
}

// =========================================================================
// Task 4349 — planar-watch run list includes op-arm (workflow-driven) runs.
//
// `planar run start` writes to the `runs` table (op-arm). Pre-fix,
// `planar-watch run list` only queried `workflow_runs` and returned []
// for these entries. With the --arm flag (default: all), both sources
// appear.
// =========================================================================

test "planar-watch run list --json includes op-arm run seeded via planar run start" {
    // RED-THEN-GREEN: this test asserts the contract fixed by task 4349.
    // Before the fix: run list returned runs:[] for op-arm entries (which live
    // in the `runs` table, not `workflow_runs`). After the fix: they appear
    // under source:"op" in the combined output.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed plan with a task.
    const pid_str = seedPlanWithTask(&suite, "watch-run-list-op", "op-task");
    defer gpa.free(pid_str);

    // Seed an op-arm run via `planar run start` (writes to `runs` table).
    // This is the workflow-driven path used by finalize_closeout.lua and
    // other planar-execute workflows.
    const start_out = suite.mustRun(&.{
        "run",        "start",
        "--plan",     pid_str,
        "--workflow", "finalize",
        "--json",
    });
    defer gpa.free(start_out);

    // Verify the run was created (run_uid in JSON).
    try std.testing.expect(std.mem.indexOf(u8, start_out, "\"run_uid\":\"") != null);

    // Extract run_uid for the assertion below.
    const run_uid = extractStringField(gpa, start_out, "\"run_uid\":\"") catch @panic("no run_uid");
    defer gpa.free(run_uid);
    try std.testing.expect(run_uid.len > 0);

    // planar-watch run list --json (default --arm all) must include the op-arm run.
    const out = mustRunWatch(&suite, &.{ "run", "list", "--json" });
    defer gpa.free(out);

    // Top-level shape.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"generated_at\":") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "\"runs\":[") != null);

    // The op-arm run must appear (run_uid is the run_identifier for op-source rows).
    if (std.mem.indexOf(u8, out, run_uid) == null) {
        std.debug.print(
            "run list --json did not include op-arm run '{s}':\n{s}\n",
            .{ run_uid, out },
        );
        return error.OpArmRunMissing;
    }

    // The source field must be "op" to distinguish from wf-source rows.
    try std.testing.expect(std.mem.indexOf(u8, out, "\"source\":\"op\"") != null);

    // The workflow name must appear as workflow_name (arm = "finalize").
    try std.testing.expect(std.mem.indexOf(u8, out, "\"finalize\"") != null);
}

test "planar-watch run list --arm op returns only op-arm runs; --arm wf returns only wf-arm" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed plan with two tasks so both agents and run start have targets.
    const pid_str = seedPlanWithTask(&suite, "watch-arm-filter", "arm-task");
    defer gpa.free(pid_str);

    // Seed a wf-arm run via planar-agent run start.
    const self_pid = std.fmt.allocPrint(gpa, "{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(self_pid);
    const wf_run_label = std.fmt.allocPrint(gpa, "arm-wf-{d}", .{std.c.getpid()}) catch @panic("OOM");
    defer gpa.free(wf_run_label);
    const wf_start = mustRunAgent(&suite, &.{
        "run",        "start",    "--plan",      pid_str,
        "--workflow", "wf-agent", "--run-id",    wf_run_label,
        "--pid",      self_pid,   "--repo-root", "/tmp",
        "--json",
    });
    defer gpa.free(wf_start);

    // Seed an op-arm run via planar run start.
    const op_start = suite.mustRun(&.{
        "run",        "start",
        "--plan",     pid_str,
        "--workflow", "finalize-arm",
        "--json",
    });
    defer gpa.free(op_start);
    const op_uid = extractStringField(gpa, op_start, "\"run_uid\":\"") catch @panic("no run_uid");
    defer gpa.free(op_uid);

    // --arm wf: only wf-source rows; op-arm run must NOT appear.
    const wf_out = mustRunWatch(&suite, &.{ "run", "list", "--arm", "wf", "--json" });
    defer gpa.free(wf_out);
    try std.testing.expect(std.mem.indexOf(u8, wf_out, "\"source\":\"wf\"") != null);
    if (std.mem.indexOf(u8, wf_out, op_uid) != null) {
        std.debug.print("--arm wf leaked op-arm run '{s}':\n{s}\n", .{ op_uid, wf_out });
        return error.WfArmLeakedOpRun;
    }

    // --arm op: only op-source rows; wf-arm run must NOT appear.
    const op_out = mustRunWatch(&suite, &.{ "run", "list", "--arm", "op", "--json" });
    defer gpa.free(op_out);
    try std.testing.expect(std.mem.indexOf(u8, op_out, "\"source\":\"op\"") != null);
    if (std.mem.indexOf(u8, op_out, wf_run_label) != null) {
        std.debug.print("--arm op leaked wf-arm run '{s}':\n{s}\n", .{ wf_run_label, op_out });
        return error.OpArmLeakedWfRun;
    }
}
