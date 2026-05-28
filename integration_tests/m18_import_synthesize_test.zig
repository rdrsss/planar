//! integration_tests/m18_import_synthesize_test.zig — M18 import/synthesize parity.

const std = @import("std");
const harness = @import("harness");

const ApplyJSON = struct {
    anchor_plan_id: i64 = 0,
    tasks_cancelled: usize = 0,
    plans_abandoned: usize = 0,
    artifacts_retired: usize = 0,
    decisions_superseded: usize = 0,
};

const ImportJSON = struct {
    mode: []const u8 = "",
    repo_slug: []const u8 = "",
    fingerprint: []const u8 = "",
    cache_path: ?[]const u8 = null,
    pending_path: ?[]const u8 = null,
    message: []const u8 = "",
    applied: ?ApplyJSON = null,
};

const SynthesizeJSON = struct {
    mode: []const u8 = "",
    repo_slug: []const u8 = "",
    fingerprint: []const u8 = "",
    cache_path: []const u8 = "",
    pending_path: []const u8 = "",
    greenfield: bool = false,
    message: []const u8 = "",
    applied: ?ApplyJSON = null,
};

const PlanJSON = struct { id: i64, slug: []const u8 = "", title: []const u8 = "" };
const TaskJSON = struct { id: i64, slug: []const u8 = "", status: []const u8 = "" };

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nparseJSON failed: {s}\nbuf: {s}\n", .{ @errorName(e), buf });
        std.testing.expect(false) catch {};
        unreachable;
    };
    return parsed.value;
}

fn mkRepo(gpa: std.mem.Allocator, sub_path: []const u8, name: []const u8) ![]const u8 {
    const base = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", sub_path, name });
    errdefer gpa.free(base);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, base);
    const docs_dir = try std.fs.path.join(gpa, &.{ base, "docs" });
    defer gpa.free(docs_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, docs_dir);
    const src_dir = try std.fs.path.join(gpa, &.{ base, "src" });
    defer gpa.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);

    const readme = try std.fs.path.join(gpa, &.{ base, "README.md" });
    defer gpa.free(readme);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = readme, .data = "# Repo\n\nPlanning notes." });
    const agents = try std.fs.path.join(gpa, &.{ base, "AGENTS.md" });
    defer gpa.free(agents);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = agents, .data = "# Agent Guide" });
    const tech = try std.fs.path.join(gpa, &.{ base, "docs", "tech-spec.md" });
    defer gpa.free(tech);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = tech, .data = "## Tech" });
    const main_go = try std.fs.path.join(gpa, &.{ base, "src", "main.go" });
    defer gpa.free(main_go);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = main_go, .data = "package main\nfunc main() {}\n" });
    return base;
}

fn mkRepoTestsOnly(gpa: std.mem.Allocator, sub_path: []const u8, name: []const u8) ![]const u8 {
    const base = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", sub_path, name });
    errdefer gpa.free(base);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, base);
    const src_dir = try std.fs.path.join(gpa, &.{ base, "src" });
    defer gpa.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);

    const readme = try std.fs.path.join(gpa, &.{ base, "README.md" });
    defer gpa.free(readme);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = readme, .data = "# Tests-only Repo\n\nNo production code." });
    const test_go = try std.fs.path.join(gpa, &.{ base, "src", "main_test.go" });
    defer gpa.free(test_go);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = test_go, .data = "package main\nfunc TestX(){}\n" });
    return base;
}

fn importCachePayload(arena: std.mem.Allocator, fp: []const u8) ![]const u8 {
    return std.fmt.allocPrint(
        arena,
        "{{\"schema_version\":1,\"fingerprint\":\"{s}\",\"anchor_title\":\"Imported Anchor\",\"phases\":[{{\"slug\":\"phase-a\",\"title\":\"Phase A\",\"summary\":\"S\",\"status\":\"active\",\"tasks\":[{{\"slug\":\"task-a\",\"title\":\"Task A\",\"body\":\"B\",\"status\":\"todo\",\"priority\":100,\"next_action\":\"\",\"citations\":[{{\"path\":\"docs/tech-spec.md\"}}]}}]}}],\"decisions\":[{{\"slug\":\"d1\",\"title\":\"Decision 1\",\"body\":\"Body\",\"source\":\"llm-inferred\",\"citation\":{{\"path\":\"docs/tech-spec.md\"}}}}],\"deferred_items\":[],\"forward_specs\":[{{\"slug\":\"fs1\",\"title\":\"F1\",\"goal\":\"G\"}},{{\"slug\":\"fs2\",\"title\":\"F2\",\"goal\":\"G\"}},{{\"slug\":\"fs3\",\"title\":\"F3\",\"goal\":\"G\"}}],\"provenance\":\"test\",\"generated_at\":\"2026-01-01T00:00:00Z\"}}\n",
        .{fp},
    );
}

fn importCachePayloadNoTasks(arena: std.mem.Allocator, fp: []const u8) ![]const u8 {
    return std.fmt.allocPrint(
        arena,
        "{{\"schema_version\":1,\"fingerprint\":\"{s}\",\"anchor_title\":\"Imported Anchor\",\"phases\":[{{\"slug\":\"phase-a\",\"title\":\"Phase A\",\"summary\":\"S\",\"status\":\"active\",\"tasks\":[]}}],\"decisions\":[],\"deferred_items\":[],\"forward_specs\":[{{\"slug\":\"fs1\",\"title\":\"F1\",\"goal\":\"G\"}},{{\"slug\":\"fs2\",\"title\":\"F2\",\"goal\":\"G\"}},{{\"slug\":\"fs3\",\"title\":\"F3\",\"goal\":\"G\"}}],\"provenance\":\"test\",\"generated_at\":\"2026-01-01T00:00:00Z\"}}\n",
        .{fp},
    );
}

fn synthCachePayload(arena: std.mem.Allocator, fp: []const u8) ![]const u8 {
    return std.fmt.allocPrint(
        arena,
        "{{\"schema_version\":1,\"fingerprint\":\"{s}\",\"synthesized\":true,\"anchor_title\":\"Synth Anchor\",\"phases\":[{{\"slug\":\"phase-a\",\"title\":\"Phase A\",\"summary\":\"S\",\"status\":\"active\",\"tasks\":[{{\"slug\":\"task-a\",\"title\":\"Task A\",\"body\":\"B\",\"status\":\"todo\",\"priority\":100,\"next_action\":\"\",\"code_evidence\":[],\"citations\":[{{\"path\":\"docs/tech-spec.md\"}}]}}]}}],\"decisions\":[{{\"slug\":\"d1\",\"title\":\"Decision 1\",\"body\":\"Body\",\"source\":\"llm-inferred\",\"citation\":{{\"path\":\"docs/tech-spec.md\"}}}}],\"deferred_items\":[],\"forward_specs\":[{{\"slug\":\"fs1\",\"title\":\"F1\",\"goal\":\"G\"}},{{\"slug\":\"fs2\",\"title\":\"F2\",\"goal\":\"G\"}},{{\"slug\":\"fs3\",\"title\":\"F3\",\"goal\":\"G\"}}],\"reference_artifacts\":[{{\"path\":\"docs/tech-spec.md\",\"kind\":\"research\",\"title\":\"Original\"}}],\"provenance\":\"test\",\"generated_at\":\"2026-01-01T00:00:00Z\"}}\n",
        .{fp},
    );
}

test "M18 import --interpret supports pending, cache hit, and --apply" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const repo = try mkRepo(gpa, &suite.tmp_dir.sub_path, "repo-import");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-import" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const first = suite.mustRunWith(&.{ "import", repo, "--interpret", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(first);
    const first_json = parseJSON(ImportJSON, arena, first);
    try std.testing.expectEqualStrings("pending", first_json.mode);

    const cache_payload = try importCachePayload(arena, first_json.fingerprint);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = first_json.cache_path.?, .data = cache_payload });

    const second = suite.mustRunWith(&.{ "import", repo, "--interpret", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(second);
    const second_json = parseJSON(ImportJSON, arena, second);
    try std.testing.expectEqualStrings("cache_hit", second_json.mode);

    const applied = suite.mustRunWith(&.{ "import", repo, "--interpret", "--apply", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(applied);
    const applied_json = parseJSON(ImportJSON, arena, applied);
    try std.testing.expect(applied_json.applied != null);

    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{applied_json.applied.?.anchor_plan_id});
    const plan_show = suite.mustRun(&.{ "plan", "show", "--json", plan_id });
    defer gpa.free(plan_show);
    const plan = parseJSON(PlanJSON, arena, plan_show);
    try std.testing.expect(std.mem.indexOf(u8, plan.title, "Imported Anchor") != null);
}

test "M18 import rejects invalid cached result contract" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const repo = try mkRepo(gpa, &suite.tmp_dir.sub_path, "repo-import-invalid");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-import-invalid" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const first = suite.mustRunWith(&.{ "import", repo, "--interpret", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(first);
    const first_json = parseJSON(ImportJSON, arena, first);
    const bad = try std.fmt.allocPrint(arena, "{{\"schema_version\":1,\"fingerprint\":\"{s}\",\"anchor_title\":\"x\",\"phases\":[],\"decisions\":[],\"deferred_items\":[],\"forward_specs\":[],\"provenance\":\"\"}}\n", .{first_json.fingerprint});
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = first_json.cache_path.?, .data = bad });

    const stderr = suite.expectFailureWith(
        &.{ "import", repo, "--interpret", "--json" },
        &.{.{ .key = "PLANAR_HOME", .value = home }},
    );
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "invalid import arguments") != null);
}

test "M18 import --apply-removals reconciles missing tasks" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const repo = try mkRepo(gpa, &suite.tmp_dir.sub_path, "repo-import-removals");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-import-removals" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const first = suite.mustRunWith(&.{ "import", repo, "--interpret", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(first);
    const first_json = parseJSON(ImportJSON, arena, first);

    const initial_payload = try importCachePayload(arena, first_json.fingerprint);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = first_json.cache_path.?, .data = initial_payload });
    const applied_initial = suite.mustRunWith(&.{ "import", repo, "--interpret", "--apply", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(applied_initial);

    const updated_payload = try importCachePayloadNoTasks(arena, first_json.fingerprint);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = first_json.cache_path.?, .data = updated_payload });

    const applied_reconcile = suite.mustRunWith(&.{ "import", repo, "--interpret", "--apply", "--apply-removals", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(applied_reconcile);
    const reconcile_json = parseJSON(ImportJSON, arena, applied_reconcile);
    try std.testing.expect(reconcile_json.applied != null);
    try std.testing.expect(reconcile_json.applied.?.tasks_cancelled > 0);
}

test "M18 import default transcription apply works and keeps existing decisions on removals pass" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const repo = try mkRepo(gpa, &suite.tmp_dir.sub_path, "repo-import-transcribe");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-import-transcribe" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const applied_default = suite.mustRunWith(&.{ "import", repo, "--apply", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(applied_default);
    const default_json = parseJSON(ImportJSON, arena, applied_default);
    try std.testing.expectEqualStrings("skipped", default_json.mode);
    try std.testing.expect(default_json.applied != null);

    const first = suite.mustRunWith(&.{ "import", repo, "--interpret", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(first);
    const first_json = parseJSON(ImportJSON, arena, first);
    const cache_payload = try importCachePayload(arena, first_json.fingerprint);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = first_json.cache_path.?, .data = cache_payload });

    const applied_one = suite.mustRunWith(&.{ "import", repo, "--interpret", "--apply", "--apply-removals", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(applied_one);
    _ = parseJSON(ImportJSON, arena, applied_one);

    const applied_two = suite.mustRunWith(&.{ "import", repo, "--interpret", "--apply", "--apply-removals", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(applied_two);
    const second_apply_json = parseJSON(ImportJSON, arena, applied_two);
    try std.testing.expect(second_apply_json.applied != null);
    try std.testing.expectEqual(@as(usize, 0), second_apply_json.applied.?.decisions_superseded);
}

test "M18 synthesize supports pending, cache hit, apply, and literal passthrough" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const repo = try mkRepo(gpa, &suite.tmp_dir.sub_path, "repo-synth");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-synth" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const first = suite.mustRunWith(&.{ "synthesize", repo, "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(first);
    const first_json = parseJSON(SynthesizeJSON, arena, first);
    try std.testing.expectEqualStrings("pending", first_json.mode);
    try std.testing.expect(!first_json.greenfield);

    const cache_payload = try synthCachePayload(arena, first_json.fingerprint);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = first_json.cache_path, .data = cache_payload });

    const second = suite.mustRunWith(&.{ "synthesize", repo, "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(second);
    const second_json = parseJSON(SynthesizeJSON, arena, second);
    try std.testing.expectEqualStrings("cache_hit", second_json.mode);

    const applied = suite.mustRunWith(&.{ "synthesize", repo, "--apply", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(applied);
    const applied_json = parseJSON(SynthesizeJSON, arena, applied);
    try std.testing.expect(applied_json.applied != null);

    // literal should preserve default import mode (not force --interpret)
    const literal = suite.mustRunWith(&.{ "synthesize", repo, "--literal", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(literal);
    const literal_json = parseJSON(ImportJSON, arena, literal);
    try std.testing.expectEqualStrings("skipped", literal_json.mode);
    try std.testing.expect(literal_json.applied == null);

    const literal_apply = suite.mustRunWith(&.{ "synthesize", repo, "--literal", "--apply", "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(literal_apply);
    const literal_apply_json = parseJSON(ImportJSON, arena, literal_apply);
    try std.testing.expectEqualStrings("skipped", literal_apply_json.mode);
    try std.testing.expect(literal_apply_json.applied != null);
}

test "M18 synthesize rejects invalid cached result contract" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const repo = try mkRepo(gpa, &suite.tmp_dir.sub_path, "repo-synth-invalid");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-synth-invalid" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const first = suite.mustRunWith(&.{ "synthesize", repo, "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(first);
    const first_json = parseJSON(SynthesizeJSON, arena, first);

    const bad = try std.fmt.allocPrint(arena, "{{\"schema_version\":1,\"fingerprint\":\"{s}\",\"synthesized\":true,\"anchor_title\":\"x\",\"phases\":[],\"decisions\":[],\"deferred_items\":[],\"forward_specs\":[],\"reference_artifacts\":[],\"provenance\":\"\"}}\n", .{first_json.fingerprint});
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = first_json.cache_path, .data = bad });

    const stderr = suite.expectFailureWith(
        &.{ "synthesize", repo, "--json" },
        &.{.{ .key = "PLANAR_HOME", .value = home }},
    );
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.indexOf(u8, stderr, "invalid synthesize arguments") != null);
}

test "M18 synthesize auto-greenfield is true when all evidence areas are zero-signal" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const repo = try mkRepoTestsOnly(gpa, &suite.tmp_dir.sub_path, "repo-synth-greenfield-zero-signal");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-synth-greenfield-zero-signal" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const first = suite.mustRunWith(&.{ "synthesize", repo, "--json" }, &.{.{ .key = "PLANAR_HOME", .value = home }});
    defer gpa.free(first);
    const first_json = parseJSON(SynthesizeJSON, arena, first);
    try std.testing.expectEqualStrings("pending", first_json.mode);
    try std.testing.expect(first_json.greenfield);
}

// =========================================================================
// Plan 85 t#2658 — `--accept-spec` / `--no-forward-specs` flags are
// accepted by both import and synthesize (closes the UnknownFlag
// regression). Mutual exclusivity is enforced at the engine
// boundary. Forward-spec materialization itself is deferred to a
// follow-up task — this slice covers flag plumbing only.
// =========================================================================

test "Plan 85 t#2658: import + synthesize accept --accept-spec and --no-forward-specs" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo = try mkRepo(gpa, &suite.tmp_dir.sub_path, "repo-fwd-spec-flags");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-fwd-spec-flags" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const env: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_HOME", .value = home }};

    // import --accept-spec all: flag accepted, exit 0.
    const a = suite.mustRunWith(
        &.{ "import", repo, "--accept-spec", "all", "--json" },
        env,
    );
    gpa.free(a);

    // import --accept-spec <slug>: flag accepted, exit 0.
    const b = suite.mustRunWith(
        &.{ "import", repo, "--accept-spec", "v1-1-polish", "--json" },
        env,
    );
    gpa.free(b);

    // import --no-forward-specs: flag accepted, exit 0.
    const c = suite.mustRunWith(
        &.{ "import", repo, "--no-forward-specs", "--json" },
        env,
    );
    gpa.free(c);

    // synthesize --accept-spec all: flag accepted, exit 0.
    const d = suite.mustRunWith(
        &.{ "synthesize", repo, "--accept-spec", "all", "--json" },
        env,
    );
    gpa.free(d);

    // synthesize --no-forward-specs: flag accepted, exit 0.
    const e = suite.mustRunWith(
        &.{ "synthesize", repo, "--no-forward-specs", "--json" },
        env,
    );
    gpa.free(e);
}

test "Plan 85 t#2658: --accept-spec and --no-forward-specs are mutually exclusive" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const repo = try mkRepo(gpa, &suite.tmp_dir.sub_path, "repo-fwd-spec-mux");
    defer gpa.free(repo);
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &suite.tmp_dir.sub_path, "home-fwd-spec-mux" });
    defer gpa.free(home);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, home);

    const env: []const harness.Suite.ExtraEnvEntry = &.{.{ .key = "PLANAR_HOME", .value = home }};

    // import: both → engine returns InvalidInput → exit non-zero.
    const a = suite.expectFailureWith(
        &.{ "import", repo, "--accept-spec", "all", "--no-forward-specs" },
        env,
    );
    gpa.free(a);

    // synthesize: same contract.
    const b = suite.expectFailureWith(
        &.{ "synthesize", repo, "--accept-spec", "all", "--no-forward-specs" },
        env,
    );
    gpa.free(b);
}
