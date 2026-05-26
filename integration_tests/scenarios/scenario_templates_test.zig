//! integration_tests/scenarios/scenario_templates_test.zig
//!
//! Scenario M9 of plan 352. Templates: operator extracts embedded
//! defaults to disk, lists, shows the body, validates the JSON
//! shape, and queries resolution paths.
//!
//! Templates root isolation: each block overrides
//! PLANAR_TEMPLATES_DIR to a sub-path under the suite's tmp_dir
//! via mustRunWith so the test doesn't touch the operator's
//! ~/.planar/templates/.
//!
//! Verbs exercised:
//!     init, plan create, templates list (with embedded + disk
//!     sources), templates init, templates show, templates path,
//!     templates validate.
//!
//! Verifies (roadmap slugs):
//!     [tm/templates-init] — pre-init, `templates list --json`
//!     returns rows with `source: "embedded"`; `templates init`
//!     extracts them to disk; post-init `templates list` returns
//!     rows with `source: "disk"`.
//!     [tm/templates-show-path] — `templates show <set> <system>
//!     <kind> --json` returns the template body JSON; `templates
//!     path --system <s> --json` returns resolution paths.
//!     [tm/templates-render] (deferred) — `templates render`
//!     requires an anchor plan that survives the template's
//!     walk; trivial fixtures fail with AnchorPlanNotFound.
//!     Filed as follow-up task on plan 352.

const std = @import("std");
const harness = @import("harness");

const TemplateEntry = struct {
    set: []const u8,
    system: []const u8,
    kind: []const u8,
    source: []const u8,
    path: []const u8,
};

test "scenario: templates — list embedded, init to disk, list from disk, show + validate" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    _ = suite.registerProject("tpl-flow");
    const tpl_dir = std.fmt.allocPrint(arena, "{s}/templates", .{suite.tmpAbsPath()}) catch unreachable;

    const env = [_]harness.Suite.ExtraEnvEntry{
        .{ .key = "PLANAR_TEMPLATES_DIR", .value = tpl_dir },
    };

    // ---- 1. Pre-init: every row is sourced from `embedded`.
    //
    // `templates list --json` emits one JSON object per template
    // — one per LINE (newline-delimited JSON), not a single
    // array. Parse line-by-line.
    const pre_list_raw = suite.mustRunWith(&.{ "templates", "list", "--json" }, &env);
    defer gpa.free(pre_list_raw);
    {
        var lines = std.mem.splitScalar(u8, pre_list_raw, '\n');
        var any_embedded = false;
        while (lines.next()) |line| {
            const trimmed = std.mem.trim(u8, line, " \r");
            if (trimmed.len == 0) continue;
            const e = std.json.parseFromSlice(TemplateEntry, arena, trimmed, .{
                .allocate = .alloc_always,
                .ignore_unknown_fields = true,
            }) catch continue;
            if (std.mem.eql(u8, e.value.source, "embedded")) {
                any_embedded = true;
                break;
            }
        }
        try std.testing.expect(any_embedded);
    }

    // ---- 2. Init — extracts embedded defaults to disk.
    const init_raw = suite.mustRunWith(&.{ "templates", "init", "--json" }, &env);
    defer gpa.free(init_raw);
    // init emits one {"path": "..."} per extracted file; we just
    // assert it ran cleanly and produced some output.
    try std.testing.expect(init_raw.len > 0);

    // ---- 3. Post-init: rows now sourced from `disk`.
    const post_list_raw = suite.mustRunWith(&.{ "templates", "list", "--json" }, &env);
    defer gpa.free(post_list_raw);
    var disk_count: usize = 0;
    var captured_set: []const u8 = "";
    var captured_system: []const u8 = "";
    var captured_kind: []const u8 = "";
    {
        var lines = std.mem.splitScalar(u8, post_list_raw, '\n');
        while (lines.next()) |line| {
            const trimmed = std.mem.trim(u8, line, " \r");
            if (trimmed.len == 0) continue;
            const e = std.json.parseFromSlice(TemplateEntry, arena, trimmed, .{
                .allocate = .alloc_always,
                .ignore_unknown_fields = true,
            }) catch continue;
            if (std.mem.eql(u8, e.value.source, "disk")) {
                disk_count += 1;
                // Capture the first disk row so we can show / validate
                // it below.
                if (captured_set.len == 0) {
                    captured_set = arena.dupe(u8, e.value.set) catch unreachable;
                    captured_system = arena.dupe(u8, e.value.system) catch unreachable;
                    captured_kind = arena.dupe(u8, e.value.kind) catch unreachable;
                }
            }
        }
    }
    try std.testing.expect(disk_count > 0);
    try std.testing.expect(captured_set.len > 0);

    // ---- 4. show — emits the raw JSON template body.
    const show_raw = suite.mustRunWith(&.{
        "templates", "show", captured_set, captured_system, captured_kind, "--json",
    }, &env);
    defer gpa.free(show_raw);
    // The body should be valid JSON; we just confirm it parses
    // to an object.
    const show_parsed = std.json.parseFromSlice(std.json.Value, arena, show_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\ntemplates show JSON parse failed: {s}\nraw: {s}\n", .{ @errorName(e), show_raw });
        try std.testing.expect(false);
        return;
    };
    try std.testing.expect(show_parsed.value == .object);

    // ---- 5. validate — exits 0 on a well-formed template. The
    // verb is text-only (no --json flag), so we just assert the
    // exit code.
    const val_out = suite.mustRunWith(&.{
        "templates", "validate", captured_set, captured_system, captured_kind,
    }, &env);
    gpa.free(val_out);

    // ---- 6. path — emits the configured templates root path
    // (or an empty line when --json / --system filters are
    // provided; the verb's per-flag behavior is sparse).
    // Asserting it returns the env-overridden dir locks the
    // PLANAR_TEMPLATES_DIR plumbing.
    const path_raw = suite.mustRunWith(&.{ "templates", "path" }, &env);
    defer gpa.free(path_raw);
    try std.testing.expect(path_raw.len > 0);
    try std.testing.expect(std.mem.containsAtLeast(u8, path_raw, 1, tpl_dir));

    // ---- 7. render — exercise the engine's anchor-plan walk
    // through a real task fixture. Post-task-2454, the walker
    // falls back to the task's plan_id when no derives-from
    // edge exists, so a trivial fixture (plan + task linked
    // via plan_id) now renders cleanly. Pre-fix the verb exited
    // with AnchorPlanNotFound.
    const PlanShape = struct { id: i64 };
    const TaskShape = struct { id: i64 };
    const plan_raw = suite.mustRunWith(&.{
        "plan", "create", "--json", "Render target plan",
    }, &env);
    const plan = std.json.parseFromSlice(PlanShape, arena, plan_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(plan_raw);

    const plan_id_str = std.fmt.allocPrint(arena, "{d}", .{plan.value.id}) catch unreachable;
    const task_raw = suite.mustRunWith(&.{
        "task",          "add",   "--json",
        "--plan",        plan_id_str,
        "--next-action", "render",
        "Render target task",
    }, &env);
    const task = std.json.parseFromSlice(TaskShape, arena, task_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch unreachable;
    gpa.free(task_raw);

    const task_ref = std.fmt.allocPrint(arena, "task:{d}", .{task.value.id}) catch unreachable;
    const render_raw = suite.mustRunWith(&.{
        "templates", "render", "default", "github-issues", "issue", task_ref, "--json",
    }, &env);
    defer gpa.free(render_raw);

    // The default github-issues/issue template emits a JSON
    // object with at least `title` and `body` fields. Parse as
    // a generic object and check structurally.
    const rendered = std.json.parseFromSlice(std.json.Value, arena, render_raw, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("\ntemplates render parse failed: {s}\nraw: {s}\n", .{ @errorName(e), render_raw });
        try std.testing.expect(false);
        return;
    };
    try std.testing.expect(rendered.value == .object);
    try std.testing.expect(rendered.value.object.get("title") != null);
}
