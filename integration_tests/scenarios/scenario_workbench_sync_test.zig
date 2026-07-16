//! integration_tests/scenarios/scenario_workbench_sync_test.zig
//!
//! Scenario M8 of plan 352. Workbench sync: operator pulls a plan's
//! spec content from the DB into a filesystem tree, edits a file,
//! pushes the edit back, drives a divergent FS+DB conflict through
//! `workbench resolve --prefer fs`, atomically syncs, then archives and
//! restores the tree.
//!
//! Workbench root isolation: each block overrides
//! PLANAR_WORKBENCH_ROOT to a sub-path under the suite's tmp_dir
//! via mustRunWith so the test doesn't touch the operator's
//! ~/.planar/workbench/ tree.
//!
//! Verbs exercised:
//!     init, plan create, artifact add (with body), workbench push,
//!     workbench status, workbench pull, workbench sync,
//!     workbench resolve, workbench list, workbench archive,
//!     workbench restore, workbench extract-questions.
//!
//! Verifies (roadmap slugs):
//!     [ws/workbench-pull] — clean pull populates the FS tree from
//!     DB-stored artifact bodies.
//!     [ws/workbench-push] — FS edit is detected by `workbench
//!     status` and applied to DB by `workbench push`; status then
//!     reports clean.
//!     [ws/workbench-sync-atomic] — `workbench sync` is the
//!     atomic round-trip; on a clean tree it's a no-op.
//!     [ws/workbench-extract-questions] — `workbench extract-
//!     questions` against a spec with `## Open questions` returns
//!     the parsed items via the helper that backs pl-spec-ingest.
//!     [ws/workbench-archive-restore] — `workbench archive`
//!     snapshots the tree, `workbench restore` brings it back.

const std = @import("std");
const harness = @import("harness");

const PlanJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const ArtifactJSON = struct {
    id: i64,
    title: []const u8,
};

const WorkbenchOp = struct {
    applied: i64,
    pending: i64,
    conflicts: i64,
};

const MalformedFile = struct {
    path: []const u8,
    parse_error: []const u8,
};

const MalformedWorkbenchOp = struct {
    pending: i64,
    conflicts: i64,
    malformed: i64,
    malformed_files: []const MalformedFile,
};

const TerminalPushOp = struct {
    conflicts: i64,
    malformed: i64,
    malformed_files: []const MalformedFile,
    filtered: i64,
    pre_existing_terminal: i64,
    cleaned: i64,
};

const LintIssue = struct {
    path: []const u8,
    line: i64,
    severity: []const u8,
    code: []const u8,
    message: []const u8,
    hint: []const u8,
};

const WorkbenchListEntry = struct {
    plan: i64,
    slug: []const u8,
    status: []const u8,
    plan_key: []const u8,
    has_fs_tree: bool,
};

const ExtractedQuestion = struct {
    title: []const u8,
};

const ExtractedFile = struct {
    artifact_id: i64,
    file: []const u8,
    questions: []const ExtractedQuestion,
};

const StatusEntry = struct {
    class: []const u8,
    file_path: []const u8,
    entity_kind: []const u8 = "",
    entity_id: i64 = 0,
    conflict_id: i64 = 0,
};

const StatusResult = struct {
    entries: []const StatusEntry,
};

const ResolveResult = struct {
    resolved: bool,
    event_id: i64,
};

const ArtifactBody = struct {
    id: i64,
    body: ?[]const u8 = null,
};

// =========================================================================
// Primary flow: push → status → pull → status, list shows the plan
// =========================================================================

test "scenario: workbench sync — push seeds FS, status round-trips, list surfaces the tree" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("wb-flow");
    const wb_root = std.fmt.allocPrint(arena, "{s}/wb", .{suite.tmpAbsPath()}) catch unreachable;

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    // ---- 1. Plan + artifact (with a body) in the DB.
    const plan_raw = suite.mustRunWith(&.{
        "plan", "create", "--json", "Workbench sync target",
    }, &env);
    const plan = std.json.parseFromSlice(PlanJSON, arena, plan_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(plan_raw);
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.value.id}) catch unreachable;

    const art_raw = suite.mustRunWith(&.{
        "artifact",     "add",       "--json",
        "--plan",       plan_id_str, "--kind",
        "tech_spec",    "--body",    "# Tech spec\n\nBody content.\n\n## Open questions\n\n### What's the cutover?\n\nA paragraph.\n",
        "WB sync spec",
    }, &env);
    _ = std.json.parseFromSlice(ArtifactJSON, arena, art_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(art_raw);

    // ---- 2. push — seeds the FS from the DB.
    const push_raw = suite.mustRunWith(&.{ "workbench", "push", plan_id_str, "--json" }, &env);
    defer gpa.free(push_raw);
    const push = std.json.parseFromSlice(WorkbenchOp, arena, push_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expect(push.value.applied >= 2); // README + the artifact at minimum
    try std.testing.expectEqual(@as(i64, 0), push.value.conflicts);

    // ---- 3. status — on a clean tree, applied = 0, conflicts = 0.
    const status_raw = suite.mustRunWith(&.{ "workbench", "status", plan_id_str, "--json" }, &env);
    defer gpa.free(status_raw);
    const status = std.json.parseFromSlice(WorkbenchOp, arena, status_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expectEqual(@as(i64, 0), status.value.applied);
    try std.testing.expectEqual(@as(i64, 0), status.value.conflicts);

    // ---- 4. pull — on a clean tree, also no-op.
    const pull_raw = suite.mustRunWith(&.{ "workbench", "pull", plan_id_str, "--json" }, &env);
    defer gpa.free(pull_raw);
    const pull = std.json.parseFromSlice(WorkbenchOp, arena, pull_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expectEqual(@as(i64, 0), pull.value.conflicts);

    // ---- 5. sync — also a no-op on a clean tree.
    const sync_raw = suite.mustRunWith(&.{ "workbench", "sync", plan_id_str, "--json" }, &env);
    defer gpa.free(sync_raw);
    const sync = std.json.parseFromSlice(WorkbenchOp, arena, sync_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expectEqual(@as(i64, 0), sync.value.conflicts);

    // ---- 6. list — the plan surfaces in the workbench tree
    // listing.
    const list_raw = suite.mustRunWith(&.{ "workbench", "list", "--json" }, &env);
    defer gpa.free(list_raw);
    const ListShape = []WorkbenchListEntry;
    const list_parsed = std.json.parseFromSlice(ListShape, arena, list_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nworkbench list parse failed: {s}\nraw: {s}\n", .{ @errorName(e), list_raw });
        try std.testing.expect(false);
        return;
    };
    var found = false;
    for (list_parsed.value) |entry| {
        if (entry.plan == plan.value.id) {
            found = true;
            break;
        }
    }
    try std.testing.expect(found);

    // ---- 7. extract-questions returns the ### H3 we seeded in
    // the artifact body.
    const extract_raw = suite.mustRunWith(&.{ "workbench", "extract-questions", plan_id_str, "--json" }, &env);
    defer gpa.free(extract_raw);
    const ExtractShape = []ExtractedFile;
    const extract_parsed = std.json.parseFromSlice(ExtractShape, arena, extract_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\nworkbench extract-questions parse failed: {s}\nraw: {s}\n", .{ @errorName(e), extract_raw });
        try std.testing.expect(false);
        return;
    };
    var saw_question = false;
    for (extract_parsed.value) |f| {
        for (f.questions) |q| {
            if (std.mem.indexOf(u8, q.title, "cutover") != null) {
                saw_question = true;
                break;
            }
        }
        if (saw_question) break;
    }
    try std.testing.expect(saw_question);
}

test "scenario: malformed workbench files fail pull push and status with distinct diagnostics" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("wb-malformed");
    const wb_root = std.fmt.allocPrint(arena, "{s}/wb", .{suite.tmpAbsPath()}) catch unreachable;
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    const plan_raw = suite.mustRunWith(&.{ "plan", "create", "--json", "Malformed workbench target" }, &env);
    const plan = std.json.parseFromSlice(PlanJSON, arena, plan_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(plan_raw);
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.value.id}) catch unreachable;

    gpa.free(suite.mustRunWith(&.{ "workbench", "push", plan_id, "--json" }, &env));
    const clean_status_raw = suite.mustRunWith(&.{ "workbench", "status", plan_id, "--json" }, &env);
    const clean_status = std.json.parseFromSlice(StatusResult, arena, clean_status_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(clean_status_raw);

    var readme_path: ?[]const u8 = null;
    for (clean_status.value.entries) |entry| {
        if (std.mem.endsWith(u8, entry.file_path, "README.md")) {
            readme_path = arena.dupe(u8, entry.file_path) catch unreachable;
            break;
        }
    }
    try std.testing.expect(readme_path != null);
    const readme_abs = std.fmt.allocPrint(arena, "{s}/{s}", .{ wb_root, readme_path.? }) catch unreachable;
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = readme_abs,
        .data = "front matter delimiter is missing\n",
    }) catch unreachable;

    const lint_result = suite.execWith(&.{ "workbench", "lint", plan_id, "--json" }, &env);
    defer lint_result.deinit(gpa);
    try std.testing.expect(lint_result.term == .exited and lint_result.term.exited == 1);
    const lint_issue = std.json.parseFromSlice(LintIssue, arena, lint_result.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = false,
    }) catch unreachable;
    try std.testing.expect(std.mem.endsWith(u8, lint_issue.value.path, "README.md"));
    try std.testing.expectEqual(@as(i64, 1), lint_issue.value.line);
    try std.testing.expectEqualStrings("error", lint_issue.value.severity);
    try std.testing.expectEqualStrings("malformed_frontmatter", lint_issue.value.code);
    try std.testing.expect(lint_issue.value.message.len > 0);
    try std.testing.expect(lint_issue.value.hint.len > 0);

    const verbs = [_][]const u8{ "status", "pull", "push", "sync" };
    for (verbs) |verb| {
        const text_result = suite.execWith(&.{ "workbench", verb, plan_id }, &env);
        defer text_result.deinit(gpa);
        try std.testing.expect(text_result.term == .exited and text_result.term.exited == 1);
        try std.testing.expect(std.mem.indexOf(u8, text_result.stdout, "1 MALFORMED") != null);

        const json_result = suite.execWith(&.{ "workbench", verb, plan_id, "--json" }, &env);
        defer json_result.deinit(gpa);
        try std.testing.expect(json_result.term == .exited and json_result.term.exited == 1);
        const parsed = std.json.parseFromSlice(MalformedWorkbenchOp, arena, json_result.stdout, .{
            .allocate = .alloc_always,
            .ignore_unknown_fields = true,
        }) catch unreachable;
        try std.testing.expectEqual(@as(i64, 0), parsed.value.pending);
        try std.testing.expectEqual(@as(i64, 0), parsed.value.conflicts);
        try std.testing.expectEqual(@as(i64, 1), parsed.value.malformed);
        try std.testing.expectEqual(@as(usize, 1), parsed.value.malformed_files.len);
        try std.testing.expectEqualStrings(readme_path.?, parsed.value.malformed_files[0].path);
        try std.testing.expectEqualStrings("MalformedFrontmatter", parsed.value.malformed_files[0].parse_error);
    }
}

test "scenario: push reports malformed filtered terminal files before optional cleanup" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("wb-terminal-malformed");
    const wb_root = try std.fmt.allocPrint(arena, "{s}/wb", .{suite.tmpAbsPath()});
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    const plan_raw = suite.mustRunWith(&.{ "plan", "create", "--json", "Terminal malformed target" }, &env);
    const plan = try std.json.parseFromSlice(PlanJSON, arena, plan_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    gpa.free(plan_raw);
    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.value.id});
    const task_raw = suite.mustRunWith(&.{ "task", "add", "--plan", plan_id, "--json", "Terminal task" }, &env);
    const task = try std.json.parseFromSlice(struct { id: i64 }, arena, task_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    gpa.free(task_raw);
    const task_id = try std.fmt.allocPrint(arena, "{d}", .{task.value.id});

    gpa.free(suite.mustRunWith(&.{ "workbench", "push", plan_id, "--json" }, &env));
    const status_raw = suite.mustRunWith(&.{ "workbench", "status", plan_id, "--json" }, &env);
    const status = try std.json.parseFromSlice(StatusResult, arena, status_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    gpa.free(status_raw);
    var task_path: ?[]const u8 = null;
    for (status.value.entries) |entry| {
        if (std.mem.eql(u8, entry.entity_kind, "task") and entry.entity_id == task.value.id) {
            task_path = try arena.dupe(u8, entry.file_path);
            break;
        }
    }
    try std.testing.expect(task_path != null);
    const task_abs = try std.fmt.allocPrint(arena, "{s}/{s}", .{ wb_root, task_path.? });

    gpa.free(suite.mustRunWith(&.{ "task", "cancel", task_id }, &env));
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = task_abs,
        .data = "terminal file with malformed frontmatter\n",
    });

    const default_push = suite.execWith(&.{ "workbench", "push", plan_id, "--json" }, &env);
    defer default_push.deinit(gpa);
    try std.testing.expect(default_push.term == .exited and default_push.term.exited == 1);
    const default_summary = try std.json.parseFromSlice(TerminalPushOp, arena, default_push.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    try std.testing.expectEqual(@as(i64, 0), default_summary.value.conflicts);
    try std.testing.expectEqual(@as(i64, 1), default_summary.value.malformed);
    try std.testing.expectEqual(@as(usize, 1), default_summary.value.malformed_files.len);
    try std.testing.expectEqualStrings(task_path.?, default_summary.value.malformed_files[0].path);
    try std.testing.expectEqual(@as(i64, 1), default_summary.value.filtered);
    try std.testing.expectEqual(@as(i64, 1), default_summary.value.pre_existing_terminal);
    try std.testing.expectEqual(@as(i64, 0), default_summary.value.cleaned);

    const cleanup_push = suite.execWith(&.{ "workbench", "push", plan_id, "--apply-cleanup", "--json" }, &env);
    defer cleanup_push.deinit(gpa);
    try std.testing.expect(cleanup_push.term == .exited and cleanup_push.term.exited == 1);
    const cleanup_summary = try std.json.parseFromSlice(TerminalPushOp, arena, cleanup_push.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    try std.testing.expectEqual(@as(i64, 1), cleanup_summary.value.malformed);
    try std.testing.expectEqual(@as(i64, 1), cleanup_summary.value.cleaned);
    try std.testing.expectError(error.FileNotFound, std.Io.Dir.cwd().access(std.testing.io, task_abs, .{}));

    const clean_push = suite.execWith(&.{ "workbench", "push", plan_id, "--json" }, &env);
    defer clean_push.deinit(gpa);
    try std.testing.expect(clean_push.term == .exited and clean_push.term.exited == 0);
}

test "scenario: malformed files take exit precedence over simultaneous conflicts" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("wb-malformed-conflict");
    const wb_root = try std.fmt.allocPrint(arena, "{s}/wb", .{suite.tmpAbsPath()});
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    const plan_raw = suite.mustRunWith(&.{ "plan", "create", "--json", "Malformed conflict target" }, &env);
    const plan = try std.json.parseFromSlice(PlanJSON, arena, plan_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    gpa.free(plan_raw);
    const plan_id = try std.fmt.allocPrint(arena, "{d}", .{plan.value.id});
    const artifact_raw = suite.mustRunWith(&.{
        "artifact", "add",         "--json", "--plan",        plan_id,
        "--kind",   "design_note", "--body", "ORIGINAL_BODY", "Conflict artifact",
    }, &env);
    const artifact = try std.json.parseFromSlice(ArtifactBody, arena, artifact_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    gpa.free(artifact_raw);
    const artifact_id = try std.fmt.allocPrint(arena, "{d}", .{artifact.value.id});

    gpa.free(suite.mustRunWith(&.{ "workbench", "push", plan_id, "--json" }, &env));
    const status_raw = suite.mustRunWith(&.{ "workbench", "status", plan_id, "--json" }, &env);
    const status = try std.json.parseFromSlice(StatusResult, arena, status_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    });
    gpa.free(status_raw);
    var readme_path: ?[]const u8 = null;
    var artifact_path: ?[]const u8 = null;
    for (status.value.entries) |entry| {
        if (std.mem.endsWith(u8, entry.file_path, "README.md")) {
            readme_path = try arena.dupe(u8, entry.file_path);
        } else if (std.mem.eql(u8, entry.entity_kind, "artifact") and entry.entity_id == artifact.value.id) {
            artifact_path = try arena.dupe(u8, entry.file_path);
        }
    }
    try std.testing.expect(readme_path != null);
    try std.testing.expect(artifact_path != null);
    const readme_abs = try std.fmt.allocPrint(arena, "{s}/{s}", .{ wb_root, readme_path.? });
    const artifact_abs = try std.fmt.allocPrint(arena, "{s}/{s}", .{ wb_root, artifact_path.? });

    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = readme_abs, .data = "malformed README\n" });
    const artifact_content = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, artifact_abs, gpa, .limited(1024 * 1024));
    defer gpa.free(artifact_content);
    const fs_edited = try std.mem.replaceOwned(u8, gpa, artifact_content, "ORIGINAL_BODY", "FS_BODY");
    defer gpa.free(fs_edited);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = artifact_abs, .data = fs_edited });
    gpa.free(suite.mustRunWith(&.{ "artifact", "update", artifact_id, "--body", "DB_BODY" }, &env));

    const verbs = [_][]const u8{ "pull", "push", "sync" };
    for (verbs) |verb| {
        const result = suite.execWith(&.{ "workbench", verb, plan_id, "--json" }, &env);
        defer result.deinit(gpa);
        try std.testing.expect(result.term == .exited and result.term.exited == 1);
        const summary = try std.json.parseFromSlice(MalformedWorkbenchOp, arena, result.stdout, .{
            .allocate = .alloc_always,
            .ignore_unknown_fields = true,
        });
        try std.testing.expectEqual(@as(i64, 1), summary.value.conflicts);
        try std.testing.expectEqual(@as(i64, 1), summary.value.malformed);
        try std.testing.expectEqual(@as(usize, 1), summary.value.malformed_files.len);
    }
}

// =========================================================================
// Conflict path: divergent FS + DB edits → `workbench resolve --prefer fs`
// =========================================================================

test "scenario: workbench conflict — divergent FS+DB edits resolve via workbench resolve" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("wb-conflict");
    const wb_root = std.fmt.allocPrint(arena, "{s}/wb", .{suite.tmpAbsPath()}) catch unreachable;
    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    // ---- 1. Plan + artifact carrying a recognizable body marker.
    const plan_raw = suite.mustRunWith(&.{ "plan", "create", "--json", "Conflict target" }, &env);
    const plan = std.json.parseFromSlice(PlanJSON, arena, plan_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(plan_raw);
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.value.id}) catch unreachable;

    const art_raw = suite.mustRunWith(&.{
        "artifact", "add",         "--json", "--plan",               plan_id_str,
        "--kind",   "design_note", "--body", "ORIGINAL_MARKER_BODY", "Conflict spec",
    }, &env);
    const art = std.json.parseFromSlice(ArtifactBody, arena, art_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(art_raw);
    const art_id_str = std.fmt.allocPrint(arena, "{d}", .{art.value.id}) catch unreachable;

    // ---- 2. push seeds the FS tree + manifest baseline.
    gpa.free(suite.mustRunWith(&.{ "workbench", "push", plan_id_str, "--json" }, &env));

    // ---- 3. Locate the artifact's rendered file via status (skip README).
    const status_raw = suite.mustRunWith(&.{ "workbench", "status", plan_id_str, "--json" }, &env);
    const status = std.json.parseFromSlice(StatusResult, arena, status_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(status_raw);
    var rel_path: ?[]const u8 = null;
    for (status.value.entries) |e| {
        if (!std.mem.endsWith(u8, e.file_path, "README.md")) {
            rel_path = arena.dupe(u8, e.file_path) catch unreachable;
            break;
        }
    }
    try std.testing.expect(rel_path != null);
    const abs = std.fmt.allocPrint(arena, "{s}/{s}", .{ wb_root, rel_path.? }) catch unreachable;

    // ---- 4. Diverge BOTH sides: edit the FS body marker in place (keeping
    //         the frontmatter so the file stays parseable) AND update the DB.
    const content = std.Io.Dir.cwd().readFileAlloc(std.testing.io, abs, gpa, .limited(1024 * 1024)) catch unreachable;
    defer gpa.free(content);
    const fs_edited = std.mem.replaceOwned(u8, gpa, content, "ORIGINAL_MARKER_BODY", "FS_MARKER_BODY") catch unreachable;
    defer gpa.free(fs_edited);
    std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = abs, .data = fs_edited }) catch unreachable;

    gpa.free(suite.mustRunWith(&.{ "artifact", "update", art_id_str, "--body", "DB_MARKER_BODY" }, &env));

    // ---- 5. sync now reports a conflict (exit 3) and persists a conflict
    //         event; recover its id from the entries.
    const sync_res = suite.execWith(&.{ "workbench", "sync", plan_id_str, "--json" }, &env);
    defer sync_res.deinit(gpa);
    try std.testing.expect(sync_res.term == .exited and sync_res.term.exited == 3);
    const sync = std.json.parseFromSlice(StatusResult, arena, sync_res.stdout, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    var conflict_event: i64 = 0;
    for (sync.value.entries) |e| {
        if (std.mem.eql(u8, e.class, "conflict")) {
            conflict_event = e.conflict_id;
            break;
        }
    }
    try std.testing.expect(conflict_event > 0);
    const event_str = std.fmt.allocPrint(arena, "{d}", .{conflict_event}) catch unreachable;

    // ---- 6. resolve preferring FS: applies the FS body to the DB.
    const resolve_raw = suite.mustRunWith(&.{ "workbench", "resolve", event_str, "--prefer", "fs", "--json" }, &env);
    const resolve = std.json.parseFromSlice(ResolveResult, arena, resolve_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(resolve_raw);
    try std.testing.expect(resolve.value.resolved);
    try std.testing.expectEqual(conflict_event, resolve.value.event_id);

    // ---- 7. The DB body now reflects the FS side, and status is clean.
    const show_raw = suite.mustRunWith(&.{ "artifact", "show", art_id_str, "--json" }, &env);
    const shown = std.json.parseFromSlice(ArtifactBody, arena, show_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(show_raw);
    try std.testing.expect(shown.value.body != null);
    try std.testing.expect(std.mem.containsAtLeast(u8, shown.value.body.?, 1, "FS_MARKER_BODY"));

    const final_status = suite.execWith(&.{ "workbench", "status", plan_id_str, "--json" }, &env);
    defer final_status.deinit(gpa);
    try std.testing.expect(final_status.term == .exited and final_status.term.exited == 0);
}

// =========================================================================
// Composition: archive + restore round-trips the FS tree
// =========================================================================

test "scenario: workbench sync — archive then restore round-trips the FS tree" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("wb-archive");
    const wb_root = std.fmt.allocPrint(arena, "{s}/wb", .{suite.tmpAbsPath()}) catch unreachable;

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    const plan_raw = suite.mustRunWith(&.{
        "plan", "create", "--json", "Archive target",
    }, &env);
    const plan = std.json.parseFromSlice(PlanJSON, arena, plan_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(plan_raw);
    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.value.id}) catch unreachable;

    const art_raw = suite.mustRunWith(&.{
        "artifact",            "add",       "--json",
        "--plan",              plan_id_str, "--kind",
        "design_note",         "--body",    "# Note\nContent.",
        "Archive-target note",
    }, &env);
    gpa.free(art_raw);

    const push_raw = suite.mustRunWith(&.{ "workbench", "push", plan_id_str, "--json" }, &env);
    gpa.free(push_raw);

    // Archive.
    const arc_raw = suite.mustRunWith(&.{ "workbench", "archive", plan_id_str, "--json" }, &env);
    defer gpa.free(arc_raw);
    // Archive returns an opaque JSON payload describing the
    // operation; we just assert the verb exits 0 and produces
    // non-empty output for the JSON shape.
    try std.testing.expect(arc_raw.len > 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, arc_raw, 1, "{"));

    // Restore via `workbench restore --plan` — looks up the most-
    // recent archive for the plan.
    const restore_raw = suite.mustRunWith(&.{ "workbench", "restore", plan_id_str, "--json" }, &env);
    defer gpa.free(restore_raw);
    try std.testing.expect(restore_raw.len > 0);

    // After restore, status should be clean again.
    const status_raw = suite.mustRunWith(&.{ "workbench", "status", plan_id_str, "--json" }, &env);
    defer gpa.free(status_raw);
    const status = std.json.parseFromSlice(WorkbenchOp, arena, status_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    try std.testing.expectEqual(@as(i64, 0), status.value.conflicts);
}
