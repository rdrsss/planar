//! Representative M9 lifecycle coverage for the agent/skill ergonomics feature.
//!
//! Focused suites pin each verb's complete contract. These scenarios compose
//! the same public commands in the order prescribed by the rendered skills:
//! render/fresh health -> cross-vendor introspection preview -> approved triage,
//! durable knowledge -> read-only observation and guarded sync conflict
//! resolution.

const std = @import("std");
const harness = @import("harness");
const ext_sync = @import("../ext_sync_test.zig");
const installed_surface = @import("../installed_surface_test.zig");

const Id = struct { id: i64 };

fn envBin(name: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const value: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, value, name) and value.len > name.len and value[name.len] == '=')
            return value[name.len + 1 ..];
    }
    @panic("required integration-test binary environment variable is missing");
}

fn runSibling(suite: *const harness.Suite, env_name: []const u8, args: []const []const u8) harness.Suite.RunResult {
    const a = suite.allocator;
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(a);
    argv.append(a, envBin(env_name)) catch @panic("OOM");
    for (args) |arg| argv.append(a, arg) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var count: usize = 0;
    while (raw[count] != null) : (count += 1) {}
    const block: std.process.Environ.PosixBlock = .{ .slice = @ptrCast(raw[0..count :null]) };
    var env = (std.process.Environ{ .block = block }).createMap(a) catch @panic("OOM");
    defer env.deinit();
    env.put("PLANAR_DB", suite.db_path) catch @panic("OOM");
    const result = std.process.run(a, std.testing.io, .{ .argv = argv.items, .environ_map = &env }) catch @panic("spawn failed");
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr };
}

fn mustRunSibling(suite: *const harness.Suite, env_name: []const u8, args: []const []const u8) []u8 {
    const result = runSibling(suite, env_name, args);
    suite.allocator.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.print("{s} failed: {any}\n{s}\n", .{ env_name, result.term, result.stdout });
        @panic("sibling binary failed");
    }
    return result.stdout;
}

fn writeAbsolute(path: []const u8, data: []const u8) !void {
    if (std.fs.path.dirname(path)) |parent| try std.Io.Dir.cwd().createDirPath(std.testing.io, parent);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = data });
}

test "scenario: fresh managed install is healthy and status checks are read-only" {
    try installed_surface.runSelectedVendorInstallerLifecycle();
}

test "scenario: rendered introspection lifecycle previews three vendors before approved triage" {
    const a = std.testing.allocator;
    var suite = harness.Suite.init(a);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(a);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    _ = suite.registerProject("m9-introspect-triage");

    const root = suite.tmpAbsPath();
    // Plan 918 M5 retired the in-tree `skills render` verb (scriptorium is
    // the sole renderer now); the authored sources it used to render from
    // are exercised directly here instead of through a render pass.
    for ([_][]const u8{
        "skills/src/pl-introspect.md",
        "skills/src/pl-feedback-triage.md",
        "skills/src/pl-sync.md",
    }) |relative| {
        try std.Io.Dir.cwd().access(std.testing.io, relative, .{});
    }

    const home = try std.fs.path.join(arena, &.{ root, "home" });
    const config = try std.fs.path.join(arena, &.{ root, "config.toml" });
    const claude = try std.fs.path.join(arena, &.{ root, "claude.jsonl" });
    const codex = try std.fs.path.join(arena, &.{ root, "codex.jsonl" });
    const copilot = try std.fs.path.join(arena, &.{ root, "copilot.jsonl" });
    try writeAbsolute(claude, "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-13T12:01:00Z\",\"tool\":{\"name\":\"planar task add\",\"input\":{\"body\":\"PRIVATE\"}},\"exit_code\":1}\n");
    try writeAbsolute(codex, "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-13T12:02:00Z\",\"command_name\":\"planar plan show\",\"arguments\":[\"PRIVATE\"],\"exit_code\":1,\"retry_of_previous\":true}\n");
    try writeAbsolute(copilot, "{\"version\":\"1\",\"kind\":\"shell_result\",\"time\":\"2026-07-13T12:03:00Z\",\"command\":{\"name\":\"planar sync pull\",\"args\":[\"PRIVATE\"]},\"exit_code\":0,\"status\":\"abandoned\"}\n");
    const cfg = try std.fmt.allocPrint(arena, "[introspection]\ncli_log = false\n[introspection.transcripts]\nclaude_path = \"{s}\"\ncodex_path = \"{s}\"\ncopilot_path = \"{s}\"\n", .{ claude, codex, copilot });
    try writeAbsolute(config, cfg);
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "HOME", .value = home },
        .{ .key = "PLANAR_CONFIG_PATH", .value = config },
        .{ .key = "PLANAR_HOME", .value = try std.fs.path.join(arena, &.{ home, ".planar" }) },
    };

    const health = suite.mustRunWith(&.{ "health", "--json" }, &env);
    defer a.free(health);
    try std.testing.expect(std.mem.indexOf(u8, health, "\"overall\":\"ok\"") != null);
    const plans_before = suite.mustRun(&.{ "plan", "list", "--scope", "global", "--json" });
    defer a.free(plans_before);
    const preview_raw = suite.mustRunWith(&.{ "report", "--days", "30", "--json" }, &env);
    defer a.free(preview_raw);
    try std.testing.expect(std.mem.indexOf(u8, preview_raw, "PRIVATE") == null);
    var preview = try std.json.parseFromSlice(std.json.Value, arena, preview_raw, .{});
    defer preview.deinit();
    const signals = preview.value.object.get("introspection_preview").?.object.get("signals").?.array.items;
    try std.testing.expectEqual(@as(usize, 3), signals.len);
    try std.testing.expectEqualStrings("claude", signals[0].object.get("vendor").?.string);
    try std.testing.expectEqualStrings("codex", signals[1].object.get("vendor").?.string);
    try std.testing.expectEqualStrings("copilot", signals[2].object.get("vendor").?.string);
    const plans_after_preview = suite.mustRun(&.{ "plan", "list", "--scope", "global", "--json" });
    defer a.free(plans_after_preview);
    try std.testing.expectEqualStrings(plans_before, plans_after_preview);

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "Planar Feedback", "--slug", "planar-feedback", "--scope", "global", "--json" });
    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.id});
    const finding = suite.mustRunJSON(Id, arena, &.{ "task", "add", "retry-pattern: planar plan show", "--plan", plan_id, "--body", "vendor=codex; category=retry; count=1", "--scope", "global", "--json" });
    const finding_ref = try std.fmt.allocPrint(arena, "task:{d}", .{finding.id});
    _ = suite.mustRunJSON(std.json.Value, arena, &.{ "feedback", "triage", "set", finding_ref, "--severity", "high", "--disposition", "accepted", "--reproduction", "reproduced", "--evidence", "redacted cross-vendor aggregate", "--scope", "global", "--json" });
    const shown = suite.mustRunJSON(std.json.Value, arena, &.{ "feedback", "triage", "show", finding_ref, "--json" });
    try std.testing.expectEqualStrings("accepted", shown.object.get("disposition").?.string);
    const post_plan = suite.mustRunJSON(std.json.Value, arena, &.{ "plan", "show", plan_id, "--json" });
    try std.testing.expectEqual(plan.id, post_plan.object.get("id").?.integer);
}

test "scenario: durable knowledge is visible through read-only operational observation" {
    const a = std.testing.allocator;
    var suite = harness.Suite.init(a);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(a);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    _ = suite.registerProject("m9-knowledge-observe");

    const plan = suite.mustRunJSON(Id, arena, &.{ "plan", "create", "Decision lifecycle", "--scope", "global", "--json" });
    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.id});
    const artifact = suite.mustRunJSON(Id, arena, &.{ "artifact", "add", "SQLite evidence", "--kind", "tech_spec", "--body", "WAL preserves concurrent readers.", "--scope", "global", "--json" });
    const decision = suite.mustRunJSON(Id, arena, &.{ "decision", "add", "Adopt SQLite WAL", "--body", "Use WAL for operational concurrency.", "--scope", "global", "--json" });
    const annotation = suite.mustRunJSON(Id, arena, &.{ "annotate", "add", "--text", "The busy timeout complements WAL.", "--anchor-path", "src/db/db.zig", "--scope", "global", "--json" });
    const decision_ref = try std.fmt.allocPrint(arena, "decision:{d}", .{decision.id});
    const artifact_ref = try std.fmt.allocPrint(arena, "artifact:{d}", .{artifact.id});
    const annotation_ref = try std.fmt.allocPrint(arena, "annotation:{d}", .{annotation.id});
    const plan_ref = try std.fmt.allocPrint(arena, "plan:{d}", .{plan.id});
    a.free(suite.mustRun(&.{ "links", "add", decision_ref, plan_ref, "--relationship", "addresses", "--json" }));
    a.free(suite.mustRun(&.{ "links", "add", decision_ref, artifact_ref, "--relationship", "cites", "--json" }));
    a.free(suite.mustRun(&.{ "links", "add", annotation_ref, decision_ref, "--relationship", "cites", "--json" }));
    const links = suite.mustRun(&.{ "links", "list", decision_ref, "--json" });
    defer a.free(links);
    try std.testing.expect(std.mem.indexOf(u8, links, "\"relationship\":\"addresses\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, links, "\"relationship\":\"cites\"") != null);
    _ = suite.mustRunJSON(std.json.Value, arena, &.{ "decision", "show", try std.fmt.allocPrint(arena, "{d}", .{decision.id}), "--json" });
    _ = suite.mustRunJSON(std.json.Value, arena, &.{ "artifact", "show", try std.fmt.allocPrint(arena, "{d}", .{artifact.id}), "--json" });
    _ = suite.mustRunJSON(std.json.Value, arena, &.{ "annotate", "show", try std.fmt.allocPrint(arena, "{d}", .{annotation.id}), "--json" });

    const task = suite.mustRunJSON(Id, arena, &.{ "task", "add", "Observe this work", "--plan", plan_id, "--scope", "global", "--json" });
    const task_id = try std.fmt.allocPrint(arena, "{d}", .{task.id});
    const task_ref = try std.fmt.allocPrint(arena, "task:{d}", .{task.id});
    const claim_raw = mustRunSibling(&suite, "PLANAR_AGENT_BIN", &.{ "claim", "--entity", task_ref, "--role", "documenter", "--no-locality-probe", "--json" });
    defer a.free(claim_raw);
    var claim = try std.json.parseFromSlice(std.json.Value, arena, claim_raw, .{});
    defer claim.deinit();
    const token = claim.value.object.get("claim_token").?.string;
    const task_after_claim = suite.mustRun(&.{ "task", "show", task_id, "--json" });
    defer a.free(task_after_claim);
    try std.testing.expect(std.mem.indexOf(u8, task_after_claim, "\"status\":\"doing\"") != null);
    a.free(mustRunSibling(&suite, "PLANAR_AGENT_BIN", &.{ "heartbeat", "--claim", token, "--status", "scanning docs 1/1", "--json" }));
    const observed = mustRunSibling(&suite, "PLANAR_WATCH_BIN", &.{ "ps", "--json" });
    defer a.free(observed);
    var observed_json = try std.json.parseFromSlice(std.json.Value, arena, observed, .{});
    defer observed_json.deinit();
    var found_task = false;
    for (observed_json.value.object.get("active").?.array.items) |row| {
        const obj = row.object;
        if (std.mem.eql(u8, obj.get("entity_kind").?.string, "task") and obj.get("entity_id").?.integer == task.id)
            found_task = true;
    }
    try std.testing.expect(found_task);
    try std.testing.expect(std.mem.indexOf(u8, observed, "scanning docs 1/1") != null);
    const task_after_observe = suite.mustRun(&.{ "task", "show", task_id, "--json" });
    defer a.free(task_after_observe);
    try std.testing.expectEqualStrings(task_after_claim, task_after_observe);
    a.free(mustRunSibling(&suite, "PLANAR_AGENT_BIN", &.{ "release", "--claim", token, "--reason", "observation complete", "--no-locality-probe", "--json" }));
    const released = suite.mustRunJSON(std.json.Value, arena, &.{ "task", "show", task_id, "--json" });
    try std.testing.expectEqualStrings("todo", released.object.get("status").?.string);
}

test "scenario: approved sync conflict resolution uses the rendered public workflow" {
    try ext_sync.runSyncConflictEvidenceAndResolutionLifecycle();
}

const GuidanceFact = struct {
    key: []const u8,
    expected: []const u8,
    evidence: []const u8,
};

const GuidanceObservation = struct {
    path: []const u8,
    key: []const u8,
    actual: []const u8,
};

const GuidanceRow = struct {
    signal: []const u8,
    path: []const u8,
    action: []const u8,
    expected: []const u8,
    actual: []const u8,
    evidence: []const u8,
    operator_gated: bool,
};

fn guidanceRows(
    allocator: std.mem.Allocator,
    facts: []const GuidanceFact,
    observations: []const GuidanceObservation,
) ![]GuidanceRow {
    var rows: std.ArrayList(GuidanceRow) = .empty;
    for (observations) |observation| {
        for (facts) |fact| {
            if (!std.mem.eql(u8, fact.key, observation.key)) continue;
            if (!std.mem.eql(u8, fact.expected, observation.actual)) try rows.append(allocator, .{
                .signal = "guidance-identity-drift",
                .path = observation.path,
                .action = "defer",
                .expected = fact.expected,
                .actual = observation.actual,
                .evidence = fact.evidence,
                .operator_gated = true,
            });
        }
    }
    return rows.toOwnedSlice(allocator);
}

fn deriveMigrationTail(allocator: std.mem.Allocator) ![]u8 {
    var dir = try std.Io.Dir.cwd().openDir(std.testing.io, "migrations", .{ .iterate = true });
    defer dir.close(std.testing.io);
    var latest: ?[]u8 = null;
    errdefer if (latest) |value| allocator.free(value);
    var it = dir.iterate();
    while (try it.next(std.testing.io)) |entry| {
        if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".up.sql")) continue;
        if (latest == null or std.mem.order(u8, latest.?, entry.name) == .lt) {
            if (latest) |value| allocator.free(value);
            latest = try allocator.dupe(u8, entry.name);
        }
    }
    return latest orelse error.FileNotFound;
}

test "scenario: guidance-closeout-envelope stale identity blocks clean while clean invents no rows" {
    const a = std.testing.allocator;
    // RETIRED sub-assertion (planar plan 933 task 5456 — doc-cluster transfer):
    // 8cf4705 (task 5448) had retargeted the guidance-closeout-envelope
    // vocabulary check onto `agents/documenter.md` after `agents/orchestrator.md`
    // was raised. The doc-cluster transfer now raises `agents/documenter.md` and
    // `agents/doc-author.md` to tabularium (commit 967ed7f), so that anchor is
    // gone too. The specific "authoritative documentation identity" vocabulary
    // (`authoritative_identity`, `guidance-identity-drift`, `operator-gated`,
    // `clean closeout`) is inherently the documenter role's own contract; no
    // retained planar surface carries the token set (grepped: `authoritative_identity`
    // has ZERO retained hits, and no single retained doc holds all four), so this
    // sub-assertion is RETIRED rather than retargeted onto an unrelated doc.
    // The scenario's substance is unaffected: the guidance-identity-drift envelope
    // is computed below from planar's own repository/build identity evidence via
    // the in-test `guidanceRows` model, not from any raised agent doc.

    // Derive the live clean envelope from the same repo/build evidence named by
    // the authored workflow. Guidance prose is observed only after the facts
    // are known, so it cannot become its own authority.
    const migration_tail = try deriveMigrationTail(a);
    defer a.free(migration_tail);
    // The invariant is that the newest migration is reversible and declares its
    // OWN version — not that it happens to be any particular file. Naming the
    // file here would make every future migration fail this scenario for a
    // reason unrelated to what it asserts.
    const migration_base = migration_tail[0 .. migration_tail.len - ".up.sql".len];
    const underscore = std.mem.indexOfScalar(u8, migration_base, '_') orelse
        return error.MalformedMigrationName;
    const version = try std.fmt.parseInt(u32, migration_base[0..underscore], 10);

    const down_path = try std.fmt.allocPrint(a, "migrations/{s}.down.sql", .{migration_base});
    defer a.free(down_path);
    try std.Io.Dir.cwd().access(std.testing.io, down_path, .{});
    const up_path = try std.fmt.allocPrint(a, "migrations/{s}", .{migration_tail});
    defer a.free(up_path);
    const up_body = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, up_path, a, .limited(128 * 1024));
    defer a.free(up_body);
    const version_insert = try std.fmt.allocPrint(a, "values ({d},", .{version});
    defer a.free(version_insert);
    try std.testing.expect(std.mem.indexOf(u8, up_body, version_insert) != null);

    const build = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, "build.zig", a, .limited(512 * 1024));
    defer a.free(build);
    for ([_][]const u8{ "planar", "planar-agent", "planar-watch", "planar-execute" }) |binary| {
        const declaration = try std.fmt.allocPrint(a, ".name = \"{s}\"", .{binary});
        defer a.free(declaration);
        try std.testing.expect(std.mem.indexOf(u8, build, declaration) != null);
    }

    const ignore = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, ".gitignore", a, .limited(128 * 1024));
    defer a.free(ignore);
    for ([_][]const u8{ "/commands/", "/skills/codex/", "/skills/copilot/", "/agents/claude/", "/agents/codex/", "/agents/copilot/" }) |generated| {
        try std.testing.expect(std.mem.indexOf(u8, ignore, generated) != null);
    }
    // Source-vs-generated sanity check: confirm an authored skill+agent
    // source pair lives under `skills/src/` and `agents/` (not an
    // accidentally-committed generated/gitignored path). Retargeted from the
    // now-raised `skills/src/pl-documenter.md` + `agents/documenter.md`
    // (doc-cluster transfer to tabularium, planar plan 933 task 5456) to the
    // retained `skills/src/pl-plan.md` + `agents/planner.md` pair; any retained
    // authored source satisfies this structural invariant.
    try std.Io.Dir.cwd().access(std.testing.io, "skills/src/pl-plan.md", .{});
    try std.Io.Dir.cwd().access(std.testing.io, "agents/planner.md", .{});

    var link_buf: [std.fs.max_path_bytes]u8 = undefined;
    const link_len = try std.Io.Dir.cwd().readLink(std.testing.io, "AGENTS.md", &link_buf);
    try std.testing.expectEqualStrings("CLAUDE.md", link_buf[0..link_len]);
    const agents_guidance = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, "AGENTS.md", a, .limited(512 * 1024));
    defer a.free(agents_guidance);
    const claude_guidance = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, "CLAUDE.md", a, .limited(512 * 1024));
    defer a.free(claude_guidance);
    try std.testing.expectEqualStrings(claude_guidance, agents_guidance);
    const readme = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, "README.md", a, .limited(512 * 1024));
    defer a.free(readme);
    // The guidance docs must name the CURRENT migration tail. Deriving it here
    // (rather than repeating a literal) is what makes this assertion mean "the
    // docs track the schema" instead of "the schema is still 00030" — the
    // latter fails on every future migration for the wrong reason.
    for ([_][]const u8{ claude_guidance, readme }) |guidance| {
        try std.testing.expect(std.mem.indexOf(u8, guidance, migration_tail) != null);
    }

    const version_text = try std.fmt.allocPrint(a, "{d}", .{version});
    defer a.free(version_text);
    const version_evidence = try std.fmt.allocPrint(a, "{s} up migration", .{migration_base});
    defer a.free(version_evidence);
    const facts = [_]GuidanceFact{
        .{ .key = "migration_tail", .expected = migration_tail, .evidence = "migrations/ + schema_migrations insert" },
        .{ .key = "schema_version", .expected = version_text, .evidence = version_evidence },
        .{ .key = "binary_set", .expected = "planar,planar-agent,planar-watch,planar-execute", .evidence = "build.zig installed artifacts" },
    };
    const stale = [_]GuidanceObservation{
        .{ .path = "CLAUDE.md", .key = "migration_tail", .actual = "00027_external_sync_baseline.up.sql" },
        .{ .path = "README.md", .key = "schema_version", .actual = "28" },
    };
    const stale_rows = try guidanceRows(a, &facts, &stale);
    defer a.free(stale_rows);
    try std.testing.expectEqual(@as(usize, 2), stale_rows.len);
    for (stale_rows) |row| {
        try std.testing.expectEqualStrings("guidance-identity-drift", row.signal);
        try std.testing.expectEqualStrings("defer", row.action);
        try std.testing.expect(row.operator_gated);
        try std.testing.expect(row.evidence.len > 0);
    }
    const clean_closeout = stale_rows.len == 0;
    try std.testing.expect(!clean_closeout);

    const clean = [_]GuidanceObservation{
        .{ .path = "CLAUDE.md", .key = "migration_tail", .actual = migration_tail },
        .{ .path = "README.md", .key = "schema_version", .actual = version_text },
        .{ .path = "README.md", .key = "binary_set", .actual = "planar,planar-agent,planar-watch,planar-execute" },
    };
    const clean_rows = try guidanceRows(a, &facts, &clean);
    defer a.free(clean_rows);
    try std.testing.expectEqual(@as(usize, 0), clean_rows.len);
    try std.testing.expect(clean_rows.len == 0);
}
