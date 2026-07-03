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
    const IDJSON = struct { id: i64 };
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
