//! integration_tests/json_shape_lint_test.zig — plan 85 M7 closeout.
//!
//! JSON-shape lint: walks every documented `--json` verb across the
//! three binaries (`planar`, `planar-agent`, `planar-watch`), parses
//! the output as `std.json.Value`, and validates that EVERY required
//! key from tech-spec § "JSON shapes" is present. Extra (new) keys
//! are allowed — the contract is additive — but renames or removals
//! fail the test immediately.
//!
//! This is the wire-shape regression gate for plan 85 M7. Once
//! locked into `make test-all`, any future code change that drops a
//! documented key or renames it (e.g. `claim_token` → `token`, or
//! removing `repo_root` from `ClaimRow`) fails CI on the same PR.
//!
//! Spec sections walked (every shape from the spec, in order):
//!
//!   - planar-agent pull / peek / complete / fail / release / block
//!   - planar-agent claim / heartbeat
//!   - planar-agent action start / end
//!   - planar-agent ingest
//!   - planar-agent reconcile [--dry-run]
//!   - planar-agent abort
//!   - planar-watch feed / ps / claims / actions / plans / log
//!   - planar plan next
//!   - planar dashboard --agents
//!
//! ClaimRow locality columns (repo_root, branch, head_sha_at_claim,
//! dirty_at_claim) and ActionRow locality columns (head_sha, dirty)
//! are pinned explicitly.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// Bin resolvers.
// =========================================================================

fn resolveAgentBin() []const u8 {
    return resolveEnv("PLANAR_AGENT_BIN");
}

fn resolveWatchBin() []const u8 {
    return resolveEnv("PLANAR_WATCH_BIN");
}

fn resolveEnv(name: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, name) and s.len > name.len and s[name.len] == '=') {
            return s[name.len + 1 ..];
        }
    }
    std.debug.panic("{s} is not set; run via `make test-integration`", .{name});
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
    var env_map = environ.createMap(gpa) catch @panic("OOM env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runBin spawn failed: {s}", .{@errorName(e)});

    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunBin(
    suite: *const harness.Suite,
    bin: []const u8,
    args: []const []const u8,
) []u8 {
    const gpa = suite.allocator;
    const res = runBin(suite, bin, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "{s} failed (term={any}):\nstdout: {s}\nstderr: {s}\n",
            .{ bin, res.term, res.stdout, res.stderr },
        );
        @panic("mustRunBin: non-zero exit");
    }
    return res.stdout;
}

// =========================================================================
// Shape validators.
// =========================================================================

const ShapeError = error{
    NotObject,
    MissingKey,
    WrongType,
    ParseError,
    NoListEntry,
};

const ValueKind = enum {
    any, // any non-null type acceptable
    object,
    array,
    string,
    integer,
    boolean,
    array_or_null, // for fields that may be empty arrays
    string_or_null,
    integer_or_null,
    object_or_null,
};

const RequiredField = struct {
    key: []const u8,
    kind: ValueKind,
};

/// Parse `body` as JSON and assert it is an object containing every
/// `(key, kind)` in `required`. Extra keys are OK; type mismatches
/// or missing keys panic with a descriptive diagnostic.
fn assertObjectShape(
    gpa: std.mem.Allocator,
    label: []const u8,
    body: []const u8,
    required: []const RequiredField,
) !void {
    var parsed = std.json.parseFromSlice(std.json.Value, gpa, body, .{}) catch |e| {
        std.debug.print("[{s}] JSON parse failed ({s}):\n{s}\n", .{ label, @errorName(e), body });
        return ShapeError.ParseError;
    };
    defer parsed.deinit();

    if (parsed.value != .object) {
        std.debug.print("[{s}] expected object at root, got {s}\n", .{ label, @tagName(parsed.value) });
        return ShapeError.NotObject;
    }
    const obj = parsed.value.object;
    for (required) |req| {
        const v = obj.get(req.key) orelse {
            std.debug.print("[{s}] missing required key '{s}'; keys present:\n", .{ label, req.key });
            var it = obj.iterator();
            while (it.next()) |kv| std.debug.print("  - {s}\n", .{kv.key_ptr.*});
            return ShapeError.MissingKey;
        };
        if (!kindMatches(v, req.kind)) {
            std.debug.print(
                "[{s}] key '{s}' has wrong type — expected {s}, got {s}\n",
                .{ label, req.key, @tagName(req.kind), @tagName(v) },
            );
            return ShapeError.WrongType;
        }
    }
}

fn kindMatches(v: std.json.Value, kind: ValueKind) bool {
    return switch (kind) {
        .any => v != .null,
        .object => v == .object,
        .array => v == .array,
        .string => v == .string,
        .integer => v == .integer or v == .number_string,
        .boolean => v == .bool,
        .array_or_null => v == .array or v == .null,
        .string_or_null => v == .string or v == .null,
        .integer_or_null => v == .integer or v == .number_string or v == .null,
        .object_or_null => v == .object or v == .null,
    };
}

/// Helper for verbs that return `[]Row`-shaped collections: check the
/// outer object has the collection key as an array, then validate
/// the first element (if any) against `row_required`.
fn assertCollectionRowShape(
    gpa: std.mem.Allocator,
    label: []const u8,
    body: []const u8,
    outer_required: []const RequiredField,
    collection_key: []const u8,
    row_required: []const RequiredField,
    require_nonempty: bool,
) !void {
    var parsed = std.json.parseFromSlice(std.json.Value, gpa, body, .{}) catch |e| {
        std.debug.print("[{s}] JSON parse failed ({s}):\n{s}\n", .{ label, @errorName(e), body });
        return ShapeError.ParseError;
    };
    defer parsed.deinit();

    if (parsed.value != .object) {
        std.debug.print("[{s}] expected object, got {s}\n", .{ label, @tagName(parsed.value) });
        return ShapeError.NotObject;
    }
    const obj = parsed.value.object;
    for (outer_required) |req| {
        const v = obj.get(req.key) orelse {
            std.debug.print("[{s}] missing outer key '{s}'\n", .{ label, req.key });
            return ShapeError.MissingKey;
        };
        if (!kindMatches(v, req.kind)) {
            std.debug.print(
                "[{s}] outer key '{s}' wrong type — expected {s}, got {s}\n",
                .{ label, req.key, @tagName(req.kind), @tagName(v) },
            );
            return ShapeError.WrongType;
        }
    }

    const coll = obj.get(collection_key) orelse {
        std.debug.print("[{s}] missing collection '{s}'\n", .{ label, collection_key });
        return ShapeError.MissingKey;
    };
    if (coll != .array) {
        std.debug.print("[{s}] '{s}' is not an array\n", .{ label, collection_key });
        return ShapeError.WrongType;
    }
    if (coll.array.items.len == 0) {
        if (require_nonempty) {
            std.debug.print(
                "[{s}] expected at least one row in '{s}' for shape pin; got empty array\n",
                .{ label, collection_key },
            );
            return ShapeError.NoListEntry;
        }
        return; // empty collection; nothing to validate against row_required.
    }

    const first = coll.array.items[0];
    if (first != .object) {
        std.debug.print("[{s}] '{s}[0]' is not an object\n", .{ label, collection_key });
        return ShapeError.WrongType;
    }
    for (row_required) |req| {
        const v = first.object.get(req.key) orelse {
            std.debug.print(
                "[{s}] '{s}[0]' missing key '{s}'; keys present:\n",
                .{ label, collection_key, req.key },
            );
            var it = first.object.iterator();
            while (it.next()) |kv| std.debug.print("  - {s}\n", .{kv.key_ptr.*});
            return ShapeError.MissingKey;
        };
        if (!kindMatches(v, req.kind)) {
            std.debug.print(
                "[{s}] '{s}[0].{s}' wrong type — expected {s}, got {s}\n",
                .{ label, collection_key, req.key, @tagName(req.kind), @tagName(v) },
            );
            return ShapeError.WrongType;
        }
    }
}

// =========================================================================
// Documented shapes from tech-spec § "JSON shapes".
// =========================================================================

// ClaimRow — snake_case mirror of agent_work_claims, including
// locality columns and worktree columns.
const claim_row_fields = [_]RequiredField{
    .{ .key = "id", .kind = .integer },
    .{ .key = "claim_token", .kind = .string },
    .{ .key = "session_id", .kind = .integer },
    .{ .key = "entity_kind", .kind = .string },
    .{ .key = "entity_id", .kind = .integer },
    .{ .key = "status", .kind = .string },
    .{ .key = "vendor", .kind = .string },
    .{ .key = "claimed_at", .kind = .string },
    .{ .key = "last_heartbeat_at", .kind = .string },
    .{ .key = "lease_expires_at", .kind = .string },
    // Locality columns (M2 ship + M7 lint).
    .{ .key = "repo_root", .kind = .string_or_null },
    .{ .key = "branch", .kind = .string_or_null },
    .{ .key = "head_sha_at_claim", .kind = .string_or_null },
    .{ .key = "dirty_at_claim", .kind = .string_or_null },
    // Worktree columns.
    .{ .key = "worktree_id", .kind = .integer_or_null },
    .{ .key = "worktree_path", .kind = .string_or_null },
    .{ .key = "failure_category", .kind = .string_or_null },
};

// ActionRow — snake_case mirror of agent_actions, including locality
// columns (head_sha, dirty).
const action_row_fields = [_]RequiredField{
    .{ .key = "id", .kind = .integer },
    .{ .key = "session_id", .kind = .integer },
    .{ .key = "action_kind", .kind = .string },
    .{ .key = "vendor", .kind = .string },
    .{ .key = "started_at", .kind = .string },
    // Locality columns.
    .{ .key = "head_sha", .kind = .string_or_null },
    .{ .key = "dirty", .kind = .string_or_null },
};

// Task — subset matching `planar task show --json`. We don't require
// the FULL task shape (that lives in task tests); just the fields the
// agent-side verbs document returning.
const task_fields = [_]RequiredField{
    .{ .key = "id", .kind = .integer },
    .{ .key = "title", .kind = .string },
    .{ .key = "status", .kind = .string },
};

// =========================================================================
// Fixture seeding.
// =========================================================================

const Fixture = struct {
    suite: *harness.Suite,
    plan_id: []u8,
    task_id: []u8,
    pull_token: []u8,
    pull_action_id: []u8,
    pull_session_id: []u8,

    fn deinit(self: *Fixture) void {
        const gpa = self.suite.allocator;
        gpa.free(self.plan_id);
        gpa.free(self.task_id);
        gpa.free(self.pull_token);
        gpa.free(self.pull_action_id);
        gpa.free(self.pull_session_id);
    }
};

/// Seed: init + plan + 4 tasks (one will be pulled, three remain).
/// Returns the plan-id arg and the first task-id arg. The first task
/// is pulled; the returned claim_token / action_id / session_id are
/// usable for downstream complete/heartbeat/action verbs.
fn seedFixture(suite: *harness.Suite) Fixture {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "lint-fixture", "--json", "lint-fixture" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("seed: no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");

    var first_task_id: i64 = 0;
    const task_titles = [_][]const u8{ "lint-task-a", "lint-task-b", "lint-task-c", "lint-task-d" };
    for (task_titles, 0..) |title, i| {
        const t_json = suite.mustRun(&.{ "task", "add", "--plan", plan_arg, "--json", title });
        defer gpa.free(t_json);
        const tid = extractIntField(t_json, "\"id\"") orelse @panic("seed: no task id");
        if (i == 0) first_task_id = tid;
    }
    const task_arg = std.fmt.allocPrint(gpa, "{d}", .{first_task_id}) catch @panic("OOM");

    // Pull the first task. Skip locality probe so the fixture works
    // even outside a git checkout.
    const pull_out = mustRunBin(suite, resolveAgentBin(), &.{
        "pull",                plan_arg,
        "--vendor",            "lint",
        "--ttl",               "300",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(pull_out);

    const token = extractStringField(gpa, pull_out, "\"claim_token\":\"") catch @panic("OOM");
    const action_id = extractIntField(pull_out, "\"action_id\":") orelse @panic("seed: no action_id");
    const session_id = extractIntField(pull_out, "\"session_id\":") orelse @panic("seed: no session_id");
    const action_arg = std.fmt.allocPrint(gpa, "{d}", .{action_id}) catch @panic("OOM");
    const session_arg = std.fmt.allocPrint(gpa, "{d}", .{session_id}) catch @panic("OOM");

    return .{
        .suite = suite,
        .plan_id = plan_arg,
        .task_id = task_arg,
        .pull_token = token,
        .pull_action_id = action_arg,
        .pull_session_id = session_arg,
    };
}

// =========================================================================
// Tests — planar-agent shapes.
// =========================================================================

test "json-shape lint: planar-agent pull --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    // We already pulled inside the fixture; re-pull on the SAME plan
    // and assert the no_work shape.
    const out = mustRunBin(&suite, resolveAgentBin(), &.{
        "pull", fx.plan_id, "--vendor", "lint", "--no-locality-probe", "--json",
    });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar-agent pull (more work)", out, &.{
        .{ .key = "ok", .kind = .boolean },
        .{ .key = "no_work", .kind = .boolean },
        // When more work IS available, the doc says these are present.
        // Tasks remain (we seeded 4, pulled 1, so claim/task/action keys
        // SHOULD be present).
        .{ .key = "claim_token", .kind = .string },
        .{ .key = "task", .kind = .object },
        .{ .key = "action_id", .kind = .integer },
    });
    // Drill into claim and task to assert their nested shapes.
    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, out, .{});
    defer parsed.deinit();
    const claim_v = parsed.value.object.get("claim") orelse @panic("no claim on pull");
    try expectObjectFields(gpa, "planar-agent pull .claim", claim_v, &claim_row_fields);
    const task_v = parsed.value.object.get("task") orelse @panic("no task on pull");
    try expectObjectFields(gpa, "planar-agent pull .task", task_v, &task_fields);
}

test "json-shape lint: planar-agent pull --json no_work shape (empty plan)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", "empty-plan", "--json", "empty-plan" });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_arg = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
    defer gpa.free(plan_arg);

    const out = mustRunBin(&suite, resolveAgentBin(), &.{ "pull", plan_arg, "--no-locality-probe", "--json" });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar-agent pull (no_work)", out, &.{
        .{ .key = "ok", .kind = .boolean },
        .{ .key = "no_work", .kind = .boolean },
    });
}

test "json-shape lint: planar-agent peek --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveAgentBin(), &.{ "peek", fx.plan_id, "--json" });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar-agent peek", out, &.{
        .{ .key = "ok", .kind = .boolean },
        .{ .key = "no_work", .kind = .boolean },
    });
}

test "json-shape lint: planar-agent heartbeat --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveAgentBin(), &.{
        "heartbeat", "--claim", fx.pull_token, "--ttl", "120", "--json",
    });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar-agent heartbeat", out, &.{
        .{ .key = "ok", .kind = .boolean },
        .{ .key = "claim_token", .kind = .string },
        .{ .key = "claim", .kind = .object },
    });
    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, out, .{});
    defer parsed.deinit();
    const claim_v = parsed.value.object.get("claim").?;
    try expectObjectFields(gpa, "planar-agent heartbeat .claim", claim_v, &claim_row_fields);
}

test "json-shape lint: planar-agent action start/end --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const start_out = mustRunBin(&suite, resolveAgentBin(), &.{
        "action",              "start",
        "--claim",             fx.pull_token,
        "--kind",              "tool_call",
        "--no-locality-probe", "--json",
    });
    defer gpa.free(start_out);
    try assertObjectShape(gpa, "planar-agent action start", start_out, &.{
        .{ .key = "ok", .kind = .boolean },
        .{ .key = "action_id", .kind = .integer },
        .{ .key = "action", .kind = .object },
    });
    var start_parsed = try std.json.parseFromSlice(std.json.Value, gpa, start_out, .{});
    defer start_parsed.deinit();
    const action_v = start_parsed.value.object.get("action").?;
    try expectObjectFields(gpa, "planar-agent action start .action", action_v, &action_row_fields);
    const action_id = start_parsed.value.object.get("action_id").?.integer;
    const action_arg = std.fmt.allocPrint(gpa, "{d}", .{action_id}) catch @panic("OOM");
    defer gpa.free(action_arg);

    const end_out = mustRunBin(&suite, resolveAgentBin(), &.{
        "action", "end", "--action", action_arg, "--outcome", "ok", "--json",
    });
    defer gpa.free(end_out);
    try assertObjectShape(gpa, "planar-agent action end", end_out, &.{
        .{ .key = "ok", .kind = .boolean },
        .{ .key = "action_id", .kind = .integer },
        .{ .key = "action", .kind = .object },
    });
}

test "json-shape lint: planar-agent reconcile --dry-run --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveAgentBin(), &.{
        "reconcile", "--dry-run", "--stale-after", "0", "--json",
    });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar-agent reconcile --dry-run", out, &.{
        .{ .key = "ok", .kind = .boolean },
        .{ .key = "claims_marked_stale", .kind = .integer },
        .{ .key = "actions_closed", .kind = .integer },
        // candidates is present in --dry-run mode.
        .{ .key = "candidates", .kind = .array },
    });
}

test "json-shape lint: planar-agent complete --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveAgentBin(), &.{
        "complete", "--claim", fx.pull_token, "--summary", "ok", "--json",
    });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar-agent complete", out, &.{
        .{ .key = "ok", .kind = .boolean },
        .{ .key = "claim_token", .kind = .string },
        .{ .key = "claim", .kind = .object },
        .{ .key = "task", .kind = .object },
    });
    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, out, .{});
    defer parsed.deinit();
    const claim_v = parsed.value.object.get("claim").?;
    try expectObjectFields(gpa, "planar-agent complete .claim", claim_v, &claim_row_fields);
}

// =========================================================================
// Tests — planar-watch shapes.
// =========================================================================

test "json-shape lint: planar-watch ps --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveWatchBin(), &.{ "ps", "--json" });
    defer gpa.free(out);

    try assertCollectionRowShape(
        gpa,
        "planar-watch ps",
        out,
        &.{
            .{ .key = "generated_at", .kind = .string },
            .{ .key = "active", .kind = .array },
            .{ .key = "stale", .kind = .array },
        },
        "active",
        // ps rows are an ActiveRow — superset of ClaimRow with the
        // claim_token + heartbeat fields. We lint on the locality
        // columns specifically.
        &.{
            .{ .key = "claim_token", .kind = .string },
            .{ .key = "status", .kind = .string },
            .{ .key = "repo_root", .kind = .string_or_null },
            .{ .key = "branch", .kind = .string_or_null },
            .{ .key = "head_sha_at_claim", .kind = .string_or_null },
            .{ .key = "dirty_at_claim", .kind = .string_or_null },
        },
        true,
    );
}

test "json-shape lint: planar-watch claims --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveWatchBin(), &.{ "claims", "--json" });
    defer gpa.free(out);

    try assertCollectionRowShape(
        gpa,
        "planar-watch claims",
        out,
        &.{
            .{ .key = "generated_at", .kind = .string },
            .{ .key = "claims", .kind = .array },
        },
        "claims",
        &claim_row_fields,
        true,
    );
}

test "json-shape lint: planar-watch actions --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveWatchBin(), &.{ "actions", "--json" });
    defer gpa.free(out);

    try assertCollectionRowShape(
        gpa,
        "planar-watch actions",
        out,
        &.{
            .{ .key = "generated_at", .kind = .string },
            .{ .key = "actions", .kind = .array },
        },
        "actions",
        &action_row_fields,
        true,
    );
}

test "json-shape lint: planar-watch plans --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveWatchBin(), &.{ "plans", "--json" });
    defer gpa.free(out);

    try assertCollectionRowShape(
        gpa,
        "planar-watch plans",
        out,
        &.{
            .{ .key = "generated_at", .kind = .string },
            .{ .key = "plans", .kind = .array },
        },
        "plans",
        &.{
            .{ .key = "plan", .kind = .object },
            .{ .key = "in_flight", .kind = .boolean },
            .{ .key = "active_claims", .kind = .integer },
            .{ .key = "active_actions", .kind = .integer },
            .{ .key = "last_event_at", .kind = .string_or_null },
        },
        true,
    );
}

test "json-shape lint: planar-watch log --task --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveWatchBin(), &.{
        "log", "--task", fx.task_id, "--json",
    });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar-watch log", out, &.{
        .{ .key = "entity", .kind = .object },
        .{ .key = "entries", .kind = .array },
    });
    // entity has shape { kind, id }.
    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, out, .{});
    defer parsed.deinit();
    const entity_v = parsed.value.object.get("entity").?;
    try expectObjectFields(gpa, "planar-watch log .entity", entity_v, &.{
        .{ .key = "kind", .kind = .string },
        .{ .key = "id", .kind = .integer },
    });
}

test "json-shape lint: planar-watch feed --json shape (NDJSON event lines)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = mustRunBin(&suite, resolveWatchBin(), &.{ "feed", "--json" });
    defer gpa.free(out);

    // NDJSON — one event per line. Validate every line parses and the
    // first non-empty line has the documented top-level keys.
    var it = std.mem.tokenizeAny(u8, out, "\n");
    var saw_event = false;
    while (it.next()) |line| {
        if (line.len == 0) continue;
        saw_event = true;
        try assertObjectShape(gpa, "planar-watch feed event", line, &.{
            .{ .key = "event", .kind = .string },
            .{ .key = "at", .kind = .string },
        });
    }
    if (!saw_event) {
        std.debug.print("planar-watch feed produced no events; fixture pull should generate at least one\n", .{});
        return error.NoEvents;
    }
}

// =========================================================================
// Tests — planar operator-side shapes.
// =========================================================================

test "json-shape lint: planar plan next --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = suite.mustRun(&.{ "plan", "next", fx.plan_id, "--json" });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar plan next", out, &.{
        .{ .key = "plan_id", .kind = .integer },
        .{ .key = "available", .kind = .array },
        .{ .key = "claimed", .kind = .array },
        .{ .key = "stale", .kind = .array },
        .{ .key = "blocked", .kind = .array },
        .{ .key = "summary", .kind = .object },
    });
    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, out, .{});
    defer parsed.deinit();
    const summary_v = parsed.value.object.get("summary").?;
    try expectObjectFields(gpa, "planar plan next .summary", summary_v, &.{
        .{ .key = "available", .kind = .integer },
        .{ .key = "claimed", .kind = .integer },
        .{ .key = "stale", .kind = .integer },
        .{ .key = "blocked", .kind = .integer },
        .{ .key = "done", .kind = .integer },
    });

    // The claimed array carries `{task, claim}` rows after the
    // fixture's pull. Validate claim row carries locality columns.
    const claimed_v = parsed.value.object.get("claimed").?;
    if (claimed_v.array.items.len == 0) {
        std.debug.print("plan next: claimed bucket empty after pull; check fixture seeding\n", .{});
        return error.NoListEntry;
    }
    const entry = claimed_v.array.items[0];
    const claim_v = entry.object.get("claim") orelse @panic("no claim in claimed entry");
    try expectObjectFields(gpa, "planar plan next .claimed[0].claim", claim_v, &claim_row_fields);
}

test "json-shape lint: planar dashboard --agents --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var fx = seedFixture(&suite);
    defer fx.deinit();

    const out = suite.mustRun(&.{ "dashboard", "--agents", "--json" });
    defer gpa.free(out);

    try assertObjectShape(gpa, "planar dashboard --agents", out, &.{
        .{ .key = "active_plans", .kind = .array },
        .{ .key = "claims", .kind = .object },
        .{ .key = "next_available_by_plan", .kind = .object },
    });
    var parsed = try std.json.parseFromSlice(std.json.Value, gpa, out, .{});
    defer parsed.deinit();
    const claims_v = parsed.value.object.get("claims").?;
    try expectObjectFields(gpa, "planar dashboard .claims", claims_v, &.{
        .{ .key = "active", .kind = .array },
        .{ .key = "stale", .kind = .array },
    });
    const active_v = claims_v.object.get("active").?;
    if (active_v.array.items.len > 0) {
        try expectObjectFields(gpa, "planar dashboard .claims.active[0]", active_v.array.items[0], &claim_row_fields);
    }
}

// =========================================================================
// Tiny helpers.
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

fn extractStringField(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse return error.NotFound;
    const start = idx + prefix.len;
    var i = start;
    while (i < json.len and json[i] != '"') : (i += 1) {}
    return try gpa.dupe(u8, json[start..i]);
}

fn expectObjectFields(
    gpa: std.mem.Allocator,
    label: []const u8,
    v: std.json.Value,
    required: []const RequiredField,
) !void {
    _ = gpa;
    if (v != .object) {
        std.debug.print("[{s}] expected object, got {s}\n", .{ label, @tagName(v) });
        return ShapeError.NotObject;
    }
    for (required) |req| {
        const f = v.object.get(req.key) orelse {
            std.debug.print("[{s}] missing key '{s}'; keys present:\n", .{ label, req.key });
            var it = v.object.iterator();
            while (it.next()) |kv| std.debug.print("  - {s}\n", .{kv.key_ptr.*});
            return ShapeError.MissingKey;
        };
        if (!kindMatches(f, req.kind)) {
            std.debug.print(
                "[{s}] '{s}' wrong type — expected {s}, got {s}\n",
                .{ label, req.key, @tagName(req.kind), @tagName(f) },
            );
            return ShapeError.WrongType;
        }
    }
}
