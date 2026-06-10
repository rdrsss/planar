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

    // default_routing: coder → claude-sonnet-4-6, reviewer → claude-opus-4-8.
    const routing = obj.get("default_routing").?.array;
    var saw_coder = false;
    var saw_reviewer = false;
    for (routing.items) |r| {
        const ro = r.object;
        const role = ro.get("role").?.string;
        const model = ro.get("model").?.string;
        if (std.mem.eql(u8, role, "coder")) {
            saw_coder = true;
            try std.testing.expectEqualStrings("claude-sonnet-4-6", model);
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
    try std.testing.expectEqual(@as(usize, 4), rows.items.len);
    var saw_coder = false;
    for (rows.items) |row| {
        const o = row.object;
        if (std.mem.eql(u8, o.get("role").?.string, "coder")) {
            saw_coder = true;
            try std.testing.expectEqualStrings("claude", o.get("vendor").?.string);
            try std.testing.expectEqualStrings("claude-sonnet-4-6", o.get("model").?.string);
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
