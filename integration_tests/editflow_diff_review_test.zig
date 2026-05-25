//! integration_tests/editflow_diff_review_test.zig
//!
//! Coverage for `<entity> diff` and `<entity> review` across all six planning
//! entities. Verifies:
//!   - diff emits unified hunk output (`@@ ... @@`) with db/fs headers.
//!   - review with no verdict surfaces the current diff.
//!   - review with explicit verdict emits stable JSON and reports
//!     non-persistence (no per-entity review schema).
//!   - --approve and --request-changes are mutually exclusive.

const std = @import("std");
const harness = @import("harness");

const IDJSON = struct { id: i64 };
const ReviewJSON = struct {
    entity: []const u8,
    id: i64,
    anchor_plan_id: i64,
    workbench_path: []const u8,
    verdict: ?[]const u8,
    has_changes: bool,
    persisted: bool,
    persistence: []const u8,
};

const EntityCase = struct {
    kind: []const u8,
    id: i64,
};

test "diff/review parity: all six entities emit unified diff and stable review verdict output" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_backing = std.heap.ArenaAllocator.init(gpa);
    defer arena_backing.deinit();
    const arena = arena_backing.allocator();

    var tmp_buf: [std.fs.max_path_bytes]u8 = undefined;
    const tmp_len = suite.tmp_dir.dir.realPath(std.testing.io, &tmp_buf) catch @panic("cannot resolve tmp dir");
    const tmp_abs = tmp_buf[0..tmp_len];
    const wb_root = std.fs.path.join(arena, &.{ tmp_abs, "workbench" }) catch @panic("OOM");

    const env = &[_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_WORKBENCH_ROOT", .value = wb_root },
    };

    const plan = suite.mustRunJSON(IDJSON, arena, &.{ "plan", "create", "--json", "Diff Review Plan" });

    var id_buf: [64]u8 = undefined;
    const plan_id = std.fmt.bufPrint(&id_buf, "{d}", .{plan.id}) catch unreachable;

    const task = suite.mustRunJSON(IDJSON, arena, &.{ "task", "add", "--json", "--plan", plan_id, "T1" });
    const question = suite.mustRunJSON(IDJSON, arena, &.{ "question", "add", "--json", "Q1" });
    const scenario = suite.mustRunJSON(IDJSON, arena, &.{ "scenario", "add", "--json", "S1" });
    const decision = suite.mustRunJSON(IDJSON, arena, &.{ "decision", "add", "--json", "--body", "decision-body", "D1" });
    const artifact = suite.mustRunJSON(IDJSON, arena, &.{ "artifact", "add", "--json", "--kind", "tech_spec", "A1" });

    const plan_ref = std.fmt.allocPrint(arena, "plan:{d}", .{plan.id}) catch @panic("OOM");

    const t_link_out = suite.mustRun(&.{ "task", "link", std.fmt.allocPrint(arena, "{d}", .{task.id}) catch @panic("OOM"), plan_ref, "--relationship", "derives-from" });
    defer gpa.free(t_link_out);
    const q_link_out = suite.mustRun(&.{ "question", "link", std.fmt.allocPrint(arena, "{d}", .{question.id}) catch @panic("OOM"), plan_ref, "--relationship", "derives-from" });
    defer gpa.free(q_link_out);
    const s_link_out = suite.mustRun(&.{ "scenario", "link", std.fmt.allocPrint(arena, "{d}", .{scenario.id}) catch @panic("OOM"), plan_ref, "--relationship", "derives-from" });
    defer gpa.free(s_link_out);
    const d_link_out = suite.mustRun(&.{ "decision", "link", std.fmt.allocPrint(arena, "{d}", .{decision.id}) catch @panic("OOM"), plan_ref, "--relationship", "derives-from" });
    defer gpa.free(d_link_out);
    const a_link_out = suite.mustRun(&.{ "artifact", "link", std.fmt.allocPrint(arena, "{d}", .{artifact.id}) catch @panic("OOM"), plan_ref, "--relationship", "derives-from" });
    defer gpa.free(a_link_out);

    const cases = [_]EntityCase{
        .{ .kind = "plan", .id = plan.id },
        .{ .kind = "task", .id = task.id },
        .{ .kind = "question", .id = question.id },
        .{ .kind = "scenario", .id = scenario.id },
        .{ .kind = "decision", .id = decision.id },
        .{ .kind = "artifact", .id = artifact.id },
    };

    const pull_res = suite.execWith(&.{ "workbench", "pull", plan_id }, env);
    defer pull_res.deinit(gpa);
    try std.testing.expect(pull_res.term == .exited and pull_res.term.exited == 0);

    const push_res = suite.execWith(&.{ "workbench", "push", plan_id }, env);
    defer push_res.deinit(gpa);
    try std.testing.expect(push_res.term == .exited and push_res.term.exited == 0);

    for (cases) |tc| {
        const id_s = std.fmt.allocPrint(arena, "{d}", .{tc.id}) catch @panic("OOM");

        const diff_res = suite.execWith(&.{ tc.kind, "diff", id_s }, env);
        defer diff_res.deinit(gpa);
        try std.testing.expect(diff_res.term == .exited and diff_res.term.exited == 0);
        try std.testing.expectEqual(@as(usize, 0), diff_res.stdout.len);

        const clean_review_json = suite.execWith(&.{ tc.kind, "review", id_s, "--json" }, env);
        defer clean_review_json.deinit(gpa);
        try std.testing.expect(clean_review_json.term == .exited and clean_review_json.term.exited == 0);
        const clean_preview = std.json.parseFromSlice(ReviewJSON, arena, clean_review_json.stdout, .{
            .ignore_unknown_fields = true,
        }) catch |e| {
            std.debug.print("review JSON parse failed for clean {s}:{d}: {s}\nraw: {s}\n", .{
                tc.kind,
                tc.id,
                @errorName(e),
                clean_review_json.stdout,
            });
            try std.testing.expect(false);
            unreachable;
        };
        try std.testing.expectEqualStrings(tc.kind, clean_preview.value.entity);
        try std.testing.expectEqual(tc.id, clean_preview.value.id);
        try std.testing.expect(!clean_preview.value.has_changes);

        const clean_verdict = suite.execWith(&.{ tc.kind, "review", id_s, "--approve", "--json" }, env);
        defer clean_verdict.deinit(gpa);
        try std.testing.expect(clean_verdict.term == .exited and clean_verdict.term.exited == 0);
        const clean_verdict_json = std.json.parseFromSlice(ReviewJSON, arena, clean_verdict.stdout, .{
            .ignore_unknown_fields = true,
        }) catch |e| {
            std.debug.print("review JSON parse failed for clean verdict {s}:{d}: {s}\nraw: {s}\n", .{
                tc.kind,
                tc.id,
                @errorName(e),
                clean_verdict.stdout,
            });
            try std.testing.expect(false);
            unreachable;
        };
        try std.testing.expect(clean_verdict_json.value.verdict != null);
        try std.testing.expectEqualStrings("approve", clean_verdict_json.value.verdict.?);
        try std.testing.expect(!clean_verdict_json.value.has_changes);
        try std.testing.expect(!clean_verdict_json.value.persisted);
        try std.testing.expectEqualStrings("none", clean_verdict_json.value.persistence);

        const clean_review_text = suite.execWith(&.{ tc.kind, "review", id_s }, env);
        defer clean_review_text.deinit(gpa);
        try std.testing.expect(clean_review_text.term == .exited and clean_review_text.term.exited == 0);
        try std.testing.expectEqual(@as(usize, 0), clean_review_text.stdout.len);

        const original_file = try std.Io.Dir.cwd().readFileAlloc(
            std.testing.io,
            clean_preview.value.workbench_path,
            gpa,
            .limited(10 * 1024 * 1024),
        );
        defer gpa.free(original_file);
        try std.testing.expect(original_file.len > 0);
        try std.testing.expectEqual(@as(u8, '\n'), original_file[original_file.len - 1]);
        try std.Io.Dir.cwd().writeFile(std.testing.io, .{
            .sub_path = clean_preview.value.workbench_path,
            .data = original_file[0 .. original_file.len - 1],
        });

        const final_newline_diff = suite.execWith(&.{ tc.kind, "diff", id_s }, env);
        defer final_newline_diff.deinit(gpa);
        try std.testing.expect(final_newline_diff.term == .exited and final_newline_diff.term.exited == 0);
        try std.testing.expect(std.mem.containsAtLeast(u8, final_newline_diff.stdout, 1, "@@ -"));

        try std.Io.Dir.cwd().writeFile(std.testing.io, .{
            .sub_path = clean_preview.value.workbench_path,
            .data = original_file,
        });

        try std.Io.Dir.deleteFileAbsolute(std.testing.io, clean_preview.value.workbench_path);

        const diff_after_delete = suite.execWith(&.{ tc.kind, "diff", id_s }, env);
        defer diff_after_delete.deinit(gpa);
        try std.testing.expect(diff_after_delete.term == .exited and diff_after_delete.term.exited == 0);

        const from_header = std.fmt.allocPrint(arena, "--- db:{s}:{d}", .{ tc.kind, tc.id }) catch @panic("OOM");
        try std.testing.expect(std.mem.containsAtLeast(u8, diff_after_delete.stdout, 1, from_header));
        try std.testing.expect(std.mem.containsAtLeast(u8, diff_after_delete.stdout, 1, "+++ fs:"));
        try std.testing.expect(std.mem.containsAtLeast(u8, diff_after_delete.stdout, 1, "@@ -"));

        const review_preview = suite.execWith(&.{ tc.kind, "review", id_s }, env);
        defer review_preview.deinit(gpa);
        try std.testing.expect(review_preview.term == .exited and review_preview.term.exited == 0);
        try std.testing.expect(std.mem.containsAtLeast(u8, review_preview.stdout, 1, from_header));
        try std.testing.expect(std.mem.containsAtLeast(u8, review_preview.stdout, 1, "@@ -"));

        const review_verdict = suite.execWith(&.{ tc.kind, "review", id_s, "--approve", "--json" }, env);
        defer review_verdict.deinit(gpa);
        try std.testing.expect(review_verdict.term == .exited and review_verdict.term.exited == 0);

        const parsed = std.json.parseFromSlice(ReviewJSON, arena, review_verdict.stdout, .{
            .ignore_unknown_fields = true,
        }) catch |e| {
            std.debug.print("review JSON parse failed for dirty {s}:{d}: {s}\nraw: {s}\n", .{
                tc.kind,
                tc.id,
                @errorName(e),
                review_verdict.stdout,
            });
            try std.testing.expect(false);
            unreachable;
        };

        try std.testing.expectEqualStrings(tc.kind, parsed.value.entity);
        try std.testing.expectEqual(tc.id, parsed.value.id);
        try std.testing.expect(parsed.value.verdict != null);
        try std.testing.expectEqualStrings("approve", parsed.value.verdict.?);
        try std.testing.expect(parsed.value.has_changes);
        try std.testing.expect(!parsed.value.persisted);
        try std.testing.expectEqualStrings("none", parsed.value.persistence);
    }

    const mutex_fail = suite.execWith(
        &.{ "plan", "review", plan_id, "--approve", "--request-changes" },
        env,
    );
    defer mutex_fail.deinit(gpa);
    try std.testing.expect(mutex_fail.term == .exited and mutex_fail.term.exited != 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, mutex_fail.stderr, 1, "mutually exclusive"));
}
