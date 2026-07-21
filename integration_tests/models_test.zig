//! integration_tests/models_test.zig — `planar models` (plan 540/543)
//!
//! Black-box coverage of the provider/model discovery verb:
//!   - `models list` text + JSON shape (probes claude/codex, curated catalog,
//!     default role→tier→model routing).
//!   - `models refresh` writes the catalog cache under PLANAR_HOME/models/.
//!
//! Asserts STRUCTURE, never installed-state: whether claude/codex are present
//! is machine-dependent, but the vendors, their curated catalogs, and the
//! default routing are invariant.

const std = @import("std");
const harness = @import("harness");

test "planar models list: text output lists both vendors, catalogs, and default routing" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const stdout = suite.mustRun(&.{ "models", "list" });
    defer gpa.free(stdout);

    const required = [_][]const u8{
        "providers:",
        "claude",
        "codex",
        "claude-opus-4-8", // curated claude catalog
        "claude-haiku-4-5", // claude small tier
        "gpt-5.5", // curated codex catalog (current frontier)
        "gpt-5.3-codex-spark", // codex small/ultra-fast
        "default routing",
        "coder",
        "reviewer",
    };
    inline for (required) |needle| {
        if (std.mem.indexOf(u8, stdout, needle) == null) {
            std.debug.print("\nmodels list text missing '{s}'\nstdout:\n{s}\n", .{ needle, stdout });
            return error.TestUnexpectedResult;
        }
    }
}

test "planar models list --json: providers + curated catalog + default routing shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const stdout = suite.mustRun(&.{ "models", "list", "--json" });
    defer gpa.free(stdout);

    const trimmed = std.mem.trim(u8, stdout, " \n");
    const parsed = std.json.parseFromSlice(std.json.Value, arena, trimmed, .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nmodels --json parse failed: {s}\nraw: {s}\n", .{ @errorName(e), stdout });
        return error.TestUnexpectedResult;
    };
    try std.testing.expect(parsed.value == .object);
    const obj = parsed.value.object;

    // providers: array of two vendors, each with a non-empty curated catalog.
    const providers = obj.get("providers").?.array;
    try std.testing.expectEqual(@as(usize, 2), providers.items.len);
    const vendors = [_][]const u8{ "claude", "codex" };
    for (providers.items, vendors) |p, want_vendor| {
        const po = p.object;
        try std.testing.expectEqualStrings(want_vendor, po.get("vendor").?.string);
        try std.testing.expect(po.get("installed").? == .bool); // present, value machine-dependent
        try std.testing.expect(po.get("models").?.array.items.len > 0);
    }

    // default_routing: coder → claude-sonnet-5, reviewer → claude-opus-4-8.
    const routing = obj.get("default_routing").?.array;
    var saw_coder = false;
    var saw_reviewer = false;
    for (routing.items) |r| {
        const ro = r.object;
        const role = ro.get("role").?.string;
        const model = ro.get("model").?.string;
        if (std.mem.eql(u8, role, "coder")) {
            saw_coder = true;
            try std.testing.expectEqualStrings("claude-sonnet-5", model);
        }
        if (std.mem.eql(u8, role, "reviewer")) {
            saw_reviewer = true;
            try std.testing.expectEqualStrings("claude-opus-4-8", model);
        }
    }
    try std.testing.expect(saw_coder and saw_reviewer);
}

test "planar models refresh: writes a parseable catalog cache under PLANAR_HOME/models/" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Point PLANAR_HOME at the suite's tmp dir so refresh never touches the
    // operator's real ~/.planar.
    const home = suite.tmpAbsPath();
    const stdout = suite.mustRunWith(&.{ "models", "refresh" }, &.{
        .{ .key = "PLANAR_HOME", .value = home },
    });
    defer gpa.free(stdout);

    // The cache file must exist and parse, with the two providers.
    const cache_path = try std.fs.path.join(arena, &.{ home, "models", "catalog.json" });
    const bytes = std.Io.Dir.cwd().readFileAlloc(std.testing.io, cache_path, arena, .limited(256 * 1024)) catch |e| {
        std.debug.print("\nmodels refresh did not write {s}: {s}\n", .{ cache_path, @errorName(e) });
        return error.TestUnexpectedResult;
    };
    const parsed = std.json.parseFromSlice(std.json.Value, arena, std.mem.trim(u8, bytes, " \n"), .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\ncached catalog.json did not parse: {s}\n", .{@errorName(e)});
        return error.TestUnexpectedResult;
    };
    try std.testing.expect(parsed.value == .object);
    try std.testing.expectEqual(@as(usize, 2), parsed.value.object.get("providers").?.array.items.len);
}

test "planar models routing: default role→vendor/model + --json shape" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Non-existent config → embedded defaults (coder=claude/sonnet, reviewer=claude/opus).
    const cfg = std.fs.path.join(gpa, &.{ std.fs.path.dirname(suite.db_path).?, "none.toml" }) catch @panic("OOM");
    defer gpa.free(cfg);
    const extra: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg }};

    const stdout = suite.mustRunWith(&.{ "models", "routing", "--json" }, extra);
    defer gpa.free(stdout);

    const parsed = std.json.parseFromSlice(std.json.Value, arena, std.mem.trim(u8, stdout, " \n"), .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nrouting --json parse failed: {s}\n{s}\n", .{ @errorName(e), stdout });
        return error.TestUnexpectedResult;
    };
    const rows = parsed.value.array;
    try std.testing.expectEqual(@as(usize, 6), rows.items.len);
    var saw_coder = false;
    for (rows.items) |row| {
        const o = row.object;
        if (std.mem.eql(u8, o.get("role").?.string, "coder")) {
            saw_coder = true;
            try std.testing.expectEqualStrings("claude", o.get("vendor").?.string);
            try std.testing.expectEqualStrings("claude-sonnet-5", o.get("model").?.string);
            try std.testing.expectEqualStrings("medium", o.get("tier").?.string);
        }
    }
    try std.testing.expect(saw_coder);
}

test "planar models routing: [role_vendors] override routes coder to codex" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg = std.fs.path.join(gpa, &.{ std.fs.path.dirname(suite.db_path).?, "config.toml" }) catch @panic("OOM");
    defer gpa.free(cfg);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg,
        .data =
        \\[role_vendors]
        \\coder = "codex"
        ,
    });
    const extra: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg }};

    const stdout = suite.mustRunWith(&.{ "models", "routing" }, extra);
    defer gpa.free(stdout);

    // coder row routes to codex / gpt-5.4; reviewer stays claude.
    var coder_ok = false;
    var lines = std.mem.splitScalar(u8, stdout, '\n');
    while (lines.next()) |line| {
        const l = std.mem.trim(u8, line, " \t\r");
        if (!std.mem.startsWith(u8, l, "coder")) continue;
        if (std.mem.indexOf(u8, l, "codex") != null and std.mem.indexOf(u8, l, "gpt-5.4") != null) coder_ok = true;
    }
    if (!coder_ok) {
        std.debug.print("\ncoder not routed to codex:\n{s}\n", .{stdout});
        return error.TestUnexpectedResult;
    }
}

test "planar models list: codex entries carry human display labels (task 3633)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const stdout = suite.mustRun(&.{ "models", "list" });
    defer gpa.free(stdout);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "gpt-5.5") != null);
    try std.testing.expect(std.mem.indexOf(u8, stdout, "frontier") != null); // label text
}

test "planar models routing: custom role in config appears in --json output (plan 586 task 3937)" {
    // A user-defined role ([roles] compactor = "small") must surface in the
    // routing table alongside the built-ins, with the correct tier and model.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const cfg = std.fs.path.join(gpa, &.{ std.fs.path.dirname(suite.db_path).?, "custom_roles.toml" }) catch @panic("OOM");
    defer gpa.free(cfg);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg,
        .data =
        \\[roles]
        \\compactor = "small"
        \\[role_vendors]
        \\compactor = "claude"
        ,
    });
    const extra: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg }};

    const stdout = suite.mustRunWith(&.{ "models", "routing", "--json" }, extra);
    defer gpa.free(stdout);

    const parsed = std.json.parseFromSlice(std.json.Value, arena, std.mem.trim(u8, stdout, " \n"), .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nrouting --json parse failed: {s}\n{s}\n", .{ @errorName(e), stdout });
        return error.TestUnexpectedResult;
    };
    const rows = parsed.value.array;

    // Must have 7 rows (6 built-ins + compactor).
    if (rows.items.len != 7) {
        std.debug.print("\nexpected 7 routing rows, got {d}\n{s}\n", .{ rows.items.len, stdout });
        return error.TestUnexpectedResult;
    }

    // Find the compactor row.
    var saw_compactor = false;
    var saw_coder = false;
    var saw_reviewer = false;
    for (rows.items) |row| {
        const o = row.object;
        const role = o.get("role").?.string;
        if (std.mem.eql(u8, role, "coder")) {
            saw_coder = true;
            // Built-in coder unchanged: medium/claude/sonnet.
            try std.testing.expectEqualStrings("claude", o.get("vendor").?.string);
            try std.testing.expectEqualStrings("medium", o.get("tier").?.string);
            try std.testing.expectEqualStrings("claude-sonnet-5", o.get("model").?.string);
        }
        if (std.mem.eql(u8, role, "reviewer")) saw_reviewer = true;
        if (std.mem.eql(u8, role, "compactor")) {
            saw_compactor = true;
            try std.testing.expectEqualStrings("claude", o.get("vendor").?.string);
            try std.testing.expectEqualStrings("small", o.get("tier").?.string);
            try std.testing.expectEqualStrings("claude-haiku-4-5", o.get("model").?.string);
        }
    }
    if (!saw_coder or !saw_reviewer or !saw_compactor) {
        std.debug.print("\nmissing expected roles: coder={} reviewer={} compactor={}\n{s}\n", .{
            saw_coder, saw_reviewer, saw_compactor, stdout,
        });
        return error.TestUnexpectedResult;
    }
}

test "planar models routing: empty config — exactly 6 built-in rows, no custom" {
    // An empty config must produce exactly the six default rows.
    // This pins the no-behavior-change invariant from the spec.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Point config at a path that does NOT exist → embedded defaults only.
    const cfg = std.fs.path.join(gpa, &.{ std.fs.path.dirname(suite.db_path).?, "absent.toml" }) catch @panic("OOM");
    defer gpa.free(cfg);
    const extra: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg }};

    const stdout = suite.mustRunWith(&.{ "models", "routing", "--json" }, extra);
    defer gpa.free(stdout);

    const parsed = std.json.parseFromSlice(std.json.Value, arena, std.mem.trim(u8, stdout, " \n"), .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nrouting --json parse failed: {s}\n{s}\n", .{ @errorName(e), stdout });
        return error.TestUnexpectedResult;
    };
    const rows = parsed.value.array;
    if (rows.items.len != 6) {
        std.debug.print("\nexpected 6 routing rows with empty config, got {d}\n{s}\n", .{ rows.items.len, stdout });
        return error.TestUnexpectedResult;
    }
    // All six built-ins present with correct defaults.
    const expected = [_]struct { role: []const u8, vendor: []const u8, tier: []const u8, model: []const u8 }{
        .{ .role = "coder", .vendor = "claude", .tier = "medium", .model = "claude-sonnet-5" },
        .{ .role = "reviewer", .vendor = "claude", .tier = "large", .model = "claude-opus-4-8" },
        .{ .role = "test-coder", .vendor = "claude", .tier = "large", .model = "claude-opus-4-8" },
        .{ .role = "documenter", .vendor = "claude", .tier = "large", .model = "claude-opus-4-8" },
        .{ .role = "doc-author", .vendor = "claude", .tier = "large", .model = "claude-opus-4-8" },
        .{ .role = "sync-reconciler", .vendor = "claude", .tier = "large", .model = "claude-opus-4-8" },
    };
    for (rows.items, expected) |row, exp| {
        const o = row.object;
        try std.testing.expectEqualStrings(exp.role, o.get("role").?.string);
        try std.testing.expectEqualStrings(exp.vendor, o.get("vendor").?.string);
        try std.testing.expectEqualStrings(exp.tier, o.get("tier").?.string);
        try std.testing.expectEqualStrings(exp.model, o.get("model").?.string);
    }
}

test "planar models apply: writes the config block, idempotent without --force (task 3740)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const cfg = std.fs.path.join(gpa, &.{ std.fs.path.dirname(suite.db_path).?, "config.toml" }) catch @panic("OOM");
    defer gpa.free(cfg);
    const extra: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg }};

    const s1 = suite.mustRunWith(&.{ "models", "apply" }, extra);
    defer gpa.free(s1);
    try std.testing.expect(std.mem.indexOf(u8, s1, "wrote model routing config") != null);

    const written = std.Io.Dir.cwd().readFileAlloc(std.testing.io, cfg, gpa, .limited(64 * 1024)) catch @panic("read");
    defer gpa.free(written);
    try std.testing.expect(std.mem.indexOf(u8, written, "[models.claude]") != null);
    try std.testing.expect(std.mem.indexOf(u8, written, "[roles]") != null);

    // Second run is a no-op (models section already present) without --force.
    const s2 = suite.mustRunWith(&.{ "models", "apply" }, extra);
    defer gpa.free(s2);
    try std.testing.expect(std.mem.indexOf(u8, s2, "already present") != null);
}

test "planar models candidates: default config — tier candidate lists + mechanical routing map, embedded default provenance (plan 899)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Non-existent config → pure embedded defaults.
    const cfg = std.fs.path.join(gpa, &.{ std.fs.path.dirname(suite.db_path).?, "absent.toml" }) catch @panic("OOM");
    defer gpa.free(cfg);
    const extra: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg }};

    const stdout = suite.mustRunWith(&.{ "models", "candidates" }, extra);
    defer gpa.free(stdout);

    const required = [_][]const u8{
        "tier candidate lists",
        "claude   medium → claude-sonnet-5",
        "[embedded default]",
        "work-type routing map",
        "claude   medium mechanical",
    };
    inline for (required) |needle| {
        if (std.mem.indexOf(u8, stdout, needle) == null) {
            std.debug.print("\nmodels candidates text missing '{s}'\nstdout:\n{s}\n", .{ needle, stdout });
            return error.TestUnexpectedResult;
        }
    }
    // Every embedded default is scalar today — no unmapped work type (schema,
    // engine, architectural, cli, feature) should appear as a routing row.
    try std.testing.expect(std.mem.indexOf(u8, stdout, "claude   medium schema") == null);
}

test "planar models candidates --json: config override widens a tier to a list and adds a non-mechanical routing entry, both surface with config-file provenance (plan 899)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const cfg = std.fs.path.join(gpa, &.{ std.fs.path.dirname(suite.db_path).?, "candidates.toml" }) catch @panic("OOM");
    defer gpa.free(cfg);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg,
        .data =
        \\[models.codex]
        \\large = ["gpt-5.5", "gpt-5.3-codex-spark"]
        \\[routing.codex.large]
        \\schema = "gpt-5.3-codex-spark"
        ,
    });
    const extra: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg }};

    const stdout = suite.mustRunWith(&.{ "models", "candidates", "--json" }, extra);
    defer gpa.free(stdout);

    const parsed = std.json.parseFromSlice(std.json.Value, arena, std.mem.trim(u8, stdout, " \n"), .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nmodels candidates --json parse failed: {s}\n{s}\n", .{ @errorName(e), stdout });
        return error.TestUnexpectedResult;
    };
    try std.testing.expect(parsed.value == .object);
    const obj = parsed.value.object;

    // candidates: codex/large now has 2 entries with config-file provenance.
    const candidates = obj.get("candidates").?.array;
    var saw_codex_large = false;
    for (candidates.items) |c| {
        const co = c.object;
        if (std.mem.eql(u8, co.get("vendor").?.string, "codex") and std.mem.eql(u8, co.get("tier").?.string, "large")) {
            saw_codex_large = true;
            const cands = co.get("candidates").?.array;
            try std.testing.expectEqual(@as(usize, 2), cands.items.len);
            try std.testing.expectEqualStrings("gpt-5.5", cands.items[0].string);
            try std.testing.expectEqualStrings("gpt-5.3-codex-spark", cands.items[1].string);
            try std.testing.expectEqualStrings("config file", co.get("source").?.string);
        }
    }
    try std.testing.expect(saw_codex_large);

    // routing: codex/large/schema resolves to the non-default candidate with
    // config-file provenance.
    const routing = obj.get("routing").?.array;
    var saw_schema = false;
    for (routing.items) |r| {
        const ro = r.object;
        if (std.mem.eql(u8, ro.get("vendor").?.string, "codex") and
            std.mem.eql(u8, ro.get("tier").?.string, "large") and
            std.mem.eql(u8, ro.get("work_type").?.string, "schema"))
        {
            saw_schema = true;
            try std.testing.expectEqualStrings("gpt-5.3-codex-spark", ro.get("model").?.string);
            try std.testing.expectEqualStrings("config file", ro.get("source").?.string);
        }
    }
    try std.testing.expect(saw_schema);
}

// =========================================================================
// planar models evals (plan 898/904, tech-spec 520 D8) — routing evals
// scorecard. Seeds real dispatch history through planar / planar-agent (not
// raw SQL) so the aggregator exercises the same code paths an orchestrator
// dispatch would produce.
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

fn runAgent(suite: *const harness.Suite, args: []const []const u8) harness.Suite.RunResult {
    const gpa = suite.allocator;
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, resolveAgentBin()) catch @panic("OOM");
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
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});
    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-agent failed (term={any}):\n{s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

/// `std.json.Stringify` renders a whole-number f64 without a decimal point
/// (e.g. `2.0` → `2`), so the parser reads it back as `.integer`, not
/// `.float`. Read either representation as f64.
fn jsonNumberAsF64(value: std.json.Value) f64 {
    return switch (value) {
        .integer => |i| @floatFromInt(i),
        .float => |f| f,
        else => @panic("expected a JSON number"),
    };
}

fn evalsExtractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

fn evalsExtractStringField(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse return error.FieldNotFound;
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    if (end == json.len) return error.UnterminatedString;
    return try gpa.dupe(u8, json[i..end]);
}

/// Seed a plan with one todo task; returns the plan id arg (caller frees).
fn evalsSeedPlan(suite: *const harness.Suite, slug: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id = evalsExtractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    return std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
}

fn evalsAddTask(suite: *const harness.Suite, plan_id_arg: []const u8, title: []const u8) i64 {
    const gpa = suite.allocator;
    const out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "--json", title });
    defer gpa.free(out);
    return evalsExtractIntField(out, "\"id\"") orelse @panic("no task id");
}

/// Direct-claim a task and return its claim token (caller frees).
fn evalsClaimTask(suite: *const harness.Suite, task_id: i64) []u8 {
    const gpa = suite.allocator;
    const ref = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(ref);
    const out = mustRunAgent(suite, &.{ "claim", "--entity", ref, "--no-locality-probe", "--json" });
    defer gpa.free(out);
    return evalsExtractStringField(gpa, out, "\"claim_token\":\"") catch @panic("no claim_token");
}

/// Emit one dispatch-shape note (`agents/orchestrator.md` step 8a) naming a
/// single task's {tier, candidate, work_type} in `model_choice`.
fn evalsCaptureDispatchNote(suite: *const harness.Suite, task_id: i64, tier: []const u8, candidate: []const u8, work_type: []const u8) void {
    const gpa = suite.allocator;
    const body = std.fmt.allocPrint(
        gpa,
        "dispatch_shape: strict\nmodel_choice: {{\"{d}\":{{\"tier\":\"{s}\",\"candidate\":\"{s}\",\"work_type\":\"{s}\"}}}}",
        .{ task_id, tier, candidate, work_type },
    ) catch @panic("OOM");
    defer gpa.free(body);
    gpa.free(suite.mustRun(&.{ "capture", "note", body }));
}

test "planar models evals --json: two candidates for the same work type rank by approval rate and iteration count (plan 898/904 D8)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id_arg = evalsSeedPlan(&suite, "evals-rank");
    defer gpa.free(plan_id_arg);

    // Task A: claude-opus-4-8, one dispatch cycle, approved.
    const task_a = evalsAddTask(&suite, plan_id_arg, "evals task A");
    evalsCaptureDispatchNote(&suite, task_a, "large", "claude-opus-4-8", "schema");
    const claim_a = evalsClaimTask(&suite, task_a);
    defer gpa.free(claim_a);
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", claim_a, "--json" }));

    // Task B: claude-haiku-4-5, two dispatch cycles (request-changes loop-back
    // simulated by a second note against the same task), then aborted.
    const task_b = evalsAddTask(&suite, plan_id_arg, "evals task B");
    evalsCaptureDispatchNote(&suite, task_b, "large", "claude-haiku-4-5", "schema");
    evalsCaptureDispatchNote(&suite, task_b, "large", "claude-haiku-4-5", "schema");
    const claim_b = evalsClaimTask(&suite, task_b);
    defer gpa.free(claim_b);
    gpa.free(mustRunAgent(&suite, &.{ "fail", "--claim", claim_b, "--reason", "test abort", "--json" }));

    const stdout = suite.mustRun(&.{ "models", "evals", "--json" });
    defer gpa.free(stdout);

    const parsed = std.json.parseFromSlice(std.json.Value, arena, std.mem.trim(u8, stdout, " \n"), .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nmodels evals --json parse failed: {s}\n{s}\n", .{ @errorName(e), stdout });
        return error.TestUnexpectedResult;
    };
    const obj = parsed.value.object;

    const scorecard = obj.get("scorecard").?.array;
    var opus_rank: ?i64 = null;
    var haiku_rank: ?i64 = null;
    var haiku_dispatch_count: ?i64 = null;
    var haiku_avg_iterations: ?f64 = null;
    for (scorecard.items) |row| {
        const ro = row.object;
        if (!std.mem.eql(u8, ro.get("work_type").?.string, "schema")) continue;
        if (std.mem.eql(u8, ro.get("candidate").?.string, "claude-opus-4-8")) {
            opus_rank = ro.get("rank").?.integer;
            try std.testing.expectEqualStrings("claude", ro.get("vendor").?.string);
            try std.testing.expect(!ro.get("insufficient_data").?.bool);
        }
        if (std.mem.eql(u8, ro.get("candidate").?.string, "claude-haiku-4-5")) {
            haiku_rank = ro.get("rank").?.integer;
            haiku_dispatch_count = ro.get("dispatch_count").?.integer;
            haiku_avg_iterations = jsonNumberAsF64(ro.get("avg_iterations").?);
        }
    }
    try std.testing.expect(opus_rank != null and haiku_rank != null);
    try std.testing.expectEqual(@as(i64, 1), opus_rank.?);
    try std.testing.expectEqual(@as(i64, 2), haiku_rank.?);
    // One task ("evals task B") went through two dispatch cycles before
    // aborting: dispatch_count (distinct tasks) is 1; avg_iterations is 2.
    try std.testing.expectEqual(@as(i64, 1), haiku_dispatch_count.?);
    try std.testing.expectEqual(@as(f64, 2.0), haiku_avg_iterations.?);

    const recs = obj.get("recommendations").?.array;
    var saw_opus_rec = false;
    for (recs.items) |rec| {
        const ro = rec.object;
        if (std.mem.eql(u8, ro.get("work_type").?.string, "schema")) {
            saw_opus_rec = true;
            try std.testing.expectEqualStrings("claude-opus-4-8", ro.get("candidate").?.string);
        }
    }
    try std.testing.expect(saw_opus_rec);

    const signals = obj.get("signals_sourced").?.object;
    try std.testing.expect(signals.get("reviewer_disposition").?.bool);
    try std.testing.expect(signals.get("iteration_count").?.bool);
    try std.testing.expect(!signals.get("quality_gate_pass_fail").?.bool);
    try std.testing.expect(signals.get("test_coder_expansion").?.bool);
}

test "planar models evals --json: sibling candidate with no history reports insufficient-data; running evals writes nothing (preview-first, plan 898/904 D8)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const cfg = std.fs.path.join(gpa, &.{ std.fs.path.dirname(suite.db_path).?, "evals-config.toml" }) catch @panic("OOM");
    defer gpa.free(cfg);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg,
        .data =
        \\[models.claude]
        \\large = ["claude-opus-4-8", "claude-sonnet-5"]
        ,
    });
    const extra: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_CONFIG_PATH", .value = cfg }};

    const plan_id_arg = evalsSeedPlan(&suite, "evals-insufficient");
    defer gpa.free(plan_id_arg);

    const task_a = evalsAddTask(&suite, plan_id_arg, "evals only task");
    evalsCaptureDispatchNote(&suite, task_a, "large", "claude-opus-4-8", "schema");
    const claim_a = evalsClaimTask(&suite, task_a);
    defer gpa.free(claim_a);
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", claim_a, "--json" }));

    // Baseline: effective routing map before running evals.
    const routing_before = suite.mustRunWith(&.{ "models", "routing", "--json" }, extra);
    defer gpa.free(routing_before);

    const stdout = suite.mustRunWith(&.{ "models", "evals", "--json" }, extra);
    defer gpa.free(stdout);

    const parsed = std.json.parseFromSlice(std.json.Value, arena, std.mem.trim(u8, stdout, " \n"), .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nmodels evals --json parse failed: {s}\n{s}\n", .{ @errorName(e), stdout });
        return error.TestUnexpectedResult;
    };
    const obj = parsed.value.object;
    const scorecard = obj.get("scorecard").?.array;

    var saw_insufficient = false;
    for (scorecard.items) |row| {
        const ro = row.object;
        if (std.mem.eql(u8, ro.get("work_type").?.string, "schema") and
            std.mem.eql(u8, ro.get("candidate").?.string, "claude-sonnet-5"))
        {
            saw_insufficient = true;
            try std.testing.expect(ro.get("insufficient_data").?.bool);
            try std.testing.expectEqual(@as(i64, 0), ro.get("dispatch_count").?.integer);
            try std.testing.expect(ro.get("rank").? == .null);
        }
    }
    if (!saw_insufficient) {
        std.debug.print("\nexpected insufficient-data row for claude-sonnet-5/schema\n{s}\n", .{stdout});
        return error.TestUnexpectedResult;
    }

    // Preview-first (D8): the effective routing map is byte-for-byte
    // unchanged after running evals.
    const routing_after = suite.mustRunWith(&.{ "models", "routing", "--json" }, extra);
    defer gpa.free(routing_after);
    try std.testing.expectEqualStrings(routing_before, routing_after);
}
