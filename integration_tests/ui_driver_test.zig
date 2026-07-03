//! integration_tests/ui_driver_test.zig — P3a design_note registration.
//!
//! Covers the ingest-side prerequisite that makes UI-driver auto-detection
//! real: a design HTML under the feature workbench's `design/` directory is
//! registered as a `design_note` artifact and linked to the plan's task(s)
//! via `entity_links(relationship='addresses')` on `spec ingest --apply`.
//!
//! The harness is black-box (CLI only, no direct SQL): the workbench is
//! materialized on disk via `workbench push`, the design file is written into
//! the pushed feature dir, and the derived graph is asserted through
//! `artifact list` / `links list`.

const std = @import("std");
const harness = @import("harness");

// -------------------------------------------------------------------------
// JSON helpers (file-private; mirror spec_ingest_test.zig)
// -------------------------------------------------------------------------

fn parseJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) T {
    const trimmed = std.mem.trim(u8, buf, " \n");
    const parsed = std.json.parseFromSlice(T, arena, trimmed, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    return parsed.value;
}

fn parseNDJSON(comptime T: type, arena: std.mem.Allocator, buf: []const u8) []const T {
    var rows: std.ArrayList(T) = .empty;
    var it = std.mem.splitScalar(u8, buf, '\n');
    while (it.next()) |line_raw| {
        const line = std.mem.trim(u8, line_raw, " \t\r\n");
        if (line.len == 0) continue;
        const parsed = std.json.parseFromSlice(T, arena, line, .{
            .allocate = .alloc_always,
            .ignore_unknown_fields = true,
        }) catch unreachable;
        rows.append(arena, parsed.value) catch @panic("OOM");
    }
    return rows.toOwnedSlice(arena) catch @panic("OOM");
}

const ArtifactRow = struct {
    id: i64,
    source_path: ?[]const u8 = null,
    kind: []const u8 = "",
};

const LinkRow = struct {
    id: i64 = 0,
    from_kind: []const u8,
    from_id: i64 = 0,
    to_kind: []const u8,
    to_id: i64 = 0,
    relationship: []const u8,
};

const PushJSON = struct {
    entries: []const struct {
        file_path: []const u8,
        entity_kind: []const u8,
    },
};

/// Resolve the pushed roadmap artifact's absolute path from `workbench push`
/// JSON so we can derive the feature dir (its parent).
fn roadmapPathFromPush(
    arena: std.mem.Allocator,
    wb_root: []const u8,
    push_stdout: []const u8,
) ![]const u8 {
    const parsed = parseJSON(PushJSON, arena, push_stdout);
    for (parsed.entries) |entry| {
        if (!std.mem.eql(u8, entry.entity_kind, "artifact")) continue;
        const abs = if (std.fs.path.isAbsolute(entry.file_path))
            try arena.dupe(u8, entry.file_path)
        else
            try std.fs.path.join(arena, &.{ wb_root, entry.file_path });
        const body = std.Io.Dir.cwd().readFileAlloc(
            std.testing.io,
            abs,
            arena,
            .limited(1024 * 1024),
        ) catch continue;
        if (std.mem.indexOf(u8, body, "artifact_kind: roadmap") != null) return abs;
    }
    return error.FileNotFound;
}

fn mkdir(path: []const u8) void {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const z = std.fmt.bufPrintZ(&buf, "{s}", .{path}) catch @panic("path too long");
    _ = std.c.mkdir(z.ptr, 0o755);
}

fn countDesignNotes(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    env: []const harness.Suite.ExtraEnvEntry,
) []const ArtifactRow {
    const json = suite.mustRunWith(&.{
        "artifact", "list", "--json", "--scope", "global", "--kind", "design_note",
    }, env);
    defer suite.allocator.free(json);
    return parseJSON([]const ArtifactRow, arena, json);
}

fn addressesLinkCount(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    env: []const harness.Suite.ExtraEnvEntry,
    artifact_id: i64,
) usize {
    const ref = std.fmt.allocPrint(arena, "artifact:{d}", .{artifact_id}) catch @panic("OOM");
    const json = suite.mustRunWith(&.{ "links", "list", "--json", ref }, env);
    defer suite.allocator.free(json);
    const links = parseNDJSON(LinkRow, arena, json);
    var n: usize = 0;
    for (links) |link| {
        if (std.mem.eql(u8, link.from_kind, "artifact") and
            std.mem.eql(u8, link.to_kind, "task") and
            std.mem.eql(u8, link.relationship, "addresses")) n += 1;
    }
    return n;
}

test "spec ingest registers a design_note artifact + task link and is idempotent (P3a)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    // Workbench root env (mirrors spec_ingest_test.zig fixtures).
    const tmp_abs = suite.tmpAbsPath();
    const wb_root = std.fs.path.join(arena, &.{ tmp_abs, "workbench-ui-driver-p3a" }) catch @panic("OOM");
    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    // ---- seed anchor plan + tech-spec + roadmap ----------------------
    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "UI Driver P3a Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    const tech_body =
        \\# UI Driver P3a Tech Spec
        \\
        \\## Status
        \\
        \\Draft.
        \\
    ;
    const roadmap_body =
        \\# UI Driver P3a Roadmap
        \\
        \\## Frontend Milestone
        \\
        \\- Wire the dashboard CTA [slug: wire-dashboard-cta]
        \\
    ;

    const tech_out = suite.mustRun(&.{
        "artifact", "add",   "--json", "--kind",  "tech_spec",
        "--plan",   plan_id, "--body", tech_body, "UI Driver P3a Tech Spec",
    });
    defer gpa.free(tech_out);
    const roadmap_out = suite.mustRun(&.{
        "artifact", "add",   "--json", "--kind",     "roadmap",
        "--plan",   plan_id, "--body", roadmap_body, "UI Driver P3a Roadmap",
    });
    defer gpa.free(roadmap_out);

    // ---- push the workbench + drop a design HTML into design/ --------
    const push_out = suite.mustRunWith(&.{ "workbench", "push", "--json", plan_id }, env);
    defer gpa.free(push_out);

    const roadmap_path = try roadmapPathFromPush(arena, wb_root, push_out);
    const feature_dir = std.fs.path.dirname(roadmap_path) orelse return error.FileNotFound;
    const design_dir = std.fs.path.join(arena, &.{ feature_dir, "design" }) catch @panic("OOM");
    mkdir(design_dir);
    const design_html = std.fs.path.join(arena, &.{ design_dir, "flow.html" }) catch @panic("OOM");
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = design_html,
        .data = "<html><body><button data-testid=\"cta\">Go</button></body></html>\n",
    });

    // ---- apply -------------------------------------------------------
    const apply1 = suite.execWith(&.{ "spec", "ingest", plan_id, "--apply" }, env);
    defer apply1.deinit(gpa);
    try std.testing.expect(apply1.term == .exited and apply1.term.exited == 0);

    // Exactly one design_note artifact, source_path == "design/flow.html".
    const notes1 = countDesignNotes(&suite, arena, env);
    try std.testing.expectEqual(@as(usize, 1), notes1.len);
    try std.testing.expect(notes1[0].source_path != null);
    try std.testing.expectEqualStrings("design/flow.html", notes1[0].source_path.?);

    // At least one addresses link artifact -> task.
    const links1 = addressesLinkCount(&suite, arena, env, notes1[0].id);
    try std.testing.expect(links1 >= 1);

    // ---- re-apply: idempotent (no dup artifact, no dup link) ---------
    const apply2 = suite.execWith(&.{ "spec", "ingest", plan_id, "--apply" }, env);
    defer apply2.deinit(gpa);
    try std.testing.expect(apply2.term == .exited and apply2.term.exited == 0);

    const notes2 = countDesignNotes(&suite, arena, env);
    try std.testing.expectEqual(@as(usize, 1), notes2.len);
    const links2 = addressesLinkCount(&suite, arena, env, notes2[0].id);
    try std.testing.expectEqual(links1, links2);
}

// -------------------------------------------------------------------------
// P3b — the detection query (`planar ui-driver-query`)
// -------------------------------------------------------------------------

const UiArtifact = struct { id: i64, source_path: []const u8 = "" };
const UiQueryResult = struct {
    dispatch_ui_driver: bool,
    design_artifacts: []const UiArtifact = &.{},
    reason: []const u8 = "",
};

const IDJSON = struct { id: i64 };

fn uiDriverQuery(
    suite: *const harness.Suite,
    arena: std.mem.Allocator,
    task_ids_csv: []const u8,
) UiQueryResult {
    const json = suite.mustRun(&.{ "ui-driver-query", "--task-ids", task_ids_csv, "--json" });
    defer suite.allocator.free(json);
    return parseJSON(UiQueryResult, arena, json);
}

test "ui-driver-query detects a linked design_note; a task without one does not (P3b)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "UI Driver Query Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    // A task with a design, and a task without.
    const task_with = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "--body", "b", "Task With Design" });
    const task_without = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "--body", "b", "Task Without Design" });

    const art = suite.mustRunJSON(IDJSON, arena, &.{ "artifact", "add", "--json", "--kind", "design_note", "--source-path", "design/a.html", "--body", "b", "Flow A" });

    const art_ref = std.fmt.allocPrint(arena, "artifact:{d}", .{art.id}) catch @panic("OOM");
    const task_with_ref = std.fmt.allocPrint(arena, "task:{d}", .{task_with.id}) catch @panic("OOM");
    const add_out = suite.mustRun(&.{ "links", "add", "--relationship", "addresses", art_ref, task_with_ref });
    gpa.free(add_out);

    // Task WITH the design → dispatch true, the artifact returned.
    const with_id_csv = std.fmt.allocPrint(arena, "{d}", .{task_with.id}) catch @panic("OOM");
    const hit = uiDriverQuery(&suite, arena, with_id_csv);
    try std.testing.expect(hit.dispatch_ui_driver);
    try std.testing.expectEqual(@as(usize, 1), hit.design_artifacts.len);
    try std.testing.expectEqual(art.id, hit.design_artifacts[0].id);
    try std.testing.expectEqualStrings("design/a.html", hit.design_artifacts[0].source_path);

    // Task WITHOUT the design → dispatch false, empty set.
    const without_id_csv = std.fmt.allocPrint(arena, "{d}", .{task_without.id}) catch @panic("OOM");
    const miss = uiDriverQuery(&suite, arena, without_id_csv);
    try std.testing.expect(!miss.dispatch_ui_driver);
    try std.testing.expectEqual(@as(usize, 0), miss.design_artifacts.len);
}

test "ui-driver-query enforces from_kind='artifact' against a polymorphic id collision (P3b)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Collision Plan" });
    const plan_id = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch @panic("OOM");

    // task_x will carry the real design; task_y will carry only a question
    // whose id collides with the design artifact's id.
    const task_x = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "--body", "b", "Has Design" });
    const task_y = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "--body", "b", "Has Question Only" });

    // First artifact + first question in a fresh DB both get id 1 (separate
    // per-table sequences) — that shared integer is the collision.
    const art = suite.mustRunJSON(IDJSON, arena, &.{ "artifact", "add", "--json", "--kind", "design_note", "--source-path", "design/x.html", "--body", "b", "Flow X" });
    const q = suite.mustRunJSON(IDJSON, arena, &.{ "question", "add", "--json", "--plan", plan_id, "--body", "b", "An open question" });
    try std.testing.expectEqual(art.id, q.id); // precondition: ids collide

    const art_ref = std.fmt.allocPrint(arena, "artifact:{d}", .{art.id}) catch @panic("OOM");
    const q_ref = std.fmt.allocPrint(arena, "question:{d}", .{q.id}) catch @panic("OOM");
    const task_x_ref = std.fmt.allocPrint(arena, "task:{d}", .{task_x.id}) catch @panic("OOM");
    const task_y_ref = std.fmt.allocPrint(arena, "task:{d}", .{task_y.id}) catch @panic("OOM");

    // design → task_x (addresses); question → task_y (cites, a query-match
    // relationship). Only `from_kind='artifact'` distinguishes them.
    gpa.free(suite.mustRun(&.{ "links", "add", "--relationship", "addresses", art_ref, task_x_ref }));
    gpa.free(suite.mustRun(&.{ "links", "add", "--relationship", "cites", q_ref, task_y_ref }));

    // task_y has ONLY the colliding question link → must NOT dispatch.
    const y_csv = std.fmt.allocPrint(arena, "{d}", .{task_y.id}) catch @panic("OOM");
    const y_res = uiDriverQuery(&suite, arena, y_csv);
    try std.testing.expect(!y_res.dispatch_ui_driver);
    try std.testing.expectEqual(@as(usize, 0), y_res.design_artifacts.len);

    // task_x has the real artifact link → dispatches.
    const x_csv = std.fmt.allocPrint(arena, "{d}", .{task_x.id}) catch @panic("OOM");
    const x_res = uiDriverQuery(&suite, arena, x_csv);
    try std.testing.expect(x_res.dispatch_ui_driver);
    try std.testing.expectEqual(@as(usize, 1), x_res.design_artifacts.len);
    try std.testing.expectEqual(art.id, x_res.design_artifacts[0].id);
}
