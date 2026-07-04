//! engine/ingestor/render — text and JSON preview rendering of a Diff.
//!
//! Mirrors Go's `internal/ingestor/render.go`. Two output paths:
//!
//!   renderText — tree-shaped, per-entity prefix (+/~/-) + truncated titles.
//!                Always closes with a `coverage:` summary line and either a
//!                `Run with --apply …` footer (when there is work to do) or
//!                a `Nothing to do.` line.
//!   renderJson — single JSON object: anchor identity + `entities` array +
//!                `summary` totals + `coverage` block. Pretty-printed with
//!                two-space indent so multi-arg JSON arrays compose visibly.

const std = @import("std");
const diff_mod = @import("diff.zig");
const coverage_mod = @import("coverage.zig");

// =========================================================================
// Text renderer
// =========================================================================

/// renderText writes the tree-shaped diff preview to `writer`.
///
/// `applied` controls the footer line: false (preview) prints the
/// `Run with --apply` reminder; true suppresses the footer because the
/// caller is about to print the applied-stats line instead.
pub fn renderText(
    allocator: std.mem.Allocator,
    writer: *std.Io.Writer,
    d: diff_mod.Diff,
    applied: bool,
) !void {
    const anchor_label = if (d.assoc_slug.len > 0) d.assoc_slug else "";
    if (anchor_label.len > 0) {
        try writer.print("{s}/{s}/\n", .{ anchor_label, d.anchor_slug });
    } else {
        try writer.print("plan:{d}/{s}/\n", .{ d.anchor_plan_id, d.anchor_slug });
    }

    for (d.child_plans) |cp| try printPlan(allocator, writer, cp);
    for (d.orphan_plans) |op| try printOrphanPlan(allocator, writer, op);
    for (d.orphan_tasks) |ot| {
        const trunc = try truncate(allocator, ot.title, 36);
        defer allocator.free(trunc);
        try writer.print("  - task       {s: <36} not in current roadmap        [needs --apply-removals]\n", .{trunc});
    }

    for (d.decisions) |dec| {
        const trunc = try truncate(allocator, dec.title, 36);
        defer allocator.free(trunc);
        const note: []const u8 = if (dec.op == .remove) "  [needs --apply-removals]" else "";
        try writer.print("  {s} {s: <10} {s: <36}{s}\n", .{
            opPrefix(dec.op),
            "decision",
            trunc,
            note,
        });
    }

    for (d.new_questions) |q| {
        const trunc = try truncate(allocator, q.title, 36);
        defer allocator.free(trunc);
        const note: []const u8 = if (q.resolution.len > 0) "  (will start answered)" else "";
        try writer.print("  + {s: <10} {s: <36}{s}\n", .{ "question", trunc, note });
    }
    for (d.updated_question_status) |sc| {
        const trunc = try truncate(allocator, sc.question_title, 36);
        defer allocator.free(trunc);
        try writer.print("  ~ {s: <10} {s: <36}  [{s} → {s}]\n", .{
            "question",
            trunc,
            sc.old_status,
            sc.new_status,
        });
    }

    try writer.print("\n", .{});
    const adds = d.totalAdditions();
    const updates = d.totalUpdates();
    const removals = d.totalRemovals();
    try writer.print("{d} additions, {d} updates, {d} proposed removals.\n", .{ adds, updates, removals });

    // Coverage summary — always printed so scripts have a stable signal.
    const cov = try coverage_mod.compute(allocator, d);
    defer coverage_mod.deinitCoverage(cov, allocator);
    try renderCoverage(writer, cov);

    // Slug-collision summary — rendered after coverage so operators see it.
    if (d.slug_collisions.len > 0) {
        try writer.print("slug-collisions: {d} task slug(s) already exist globally:\n", .{d.slug_collisions.len});
        for (d.slug_collisions) |sc| {
            try writer.print("  conflict: slug '{s}' already held by task {d} (plan {d}) — apply will fail with SlugConflict\n", .{
                sc.slug, sc.existing_task_id, sc.existing_plan_id,
            });
        }
    }

    if (applied) {
        // Suppressed — caller prints stats line.
    } else if (adds > 0 or updates > 0 or removals > 0) {
        try writer.print("Run with --apply to commit; add --apply-removals to cancel proposed removals.\n", .{});
    } else {
        try writer.print("Nothing to do.\n", .{});
    }
}

fn renderCoverage(writer: *std.Io.Writer, cov: coverage_mod.Coverage) !void {
    try writer.print("coverage: {d} tasks ({d} with slug, {d} without)", .{
        cov.total_tasks,
        cov.tasks_with_slug,
        cov.tasks_without_slug,
    });
    if (cov.uncovered_task_slugs.len > 0) {
        try writer.print("; {d} uncovered: ", .{cov.uncovered_task_slugs.len});
        for (cov.uncovered_task_slugs, 0..) |s, i| {
            if (i > 0) try writer.print(", ", .{});
            try writer.print("{s}", .{s});
        }
    }
    if (cov.orphan_scenarios.len > 0) {
        try writer.print("; {d} orphan scenarios: ", .{cov.orphan_scenarios.len});
        for (cov.orphan_scenarios, 0..) |s, i| {
            if (i > 0) try writer.print("; ", .{});
            try writer.print("{s}", .{s});
        }
    }
    try writer.print("\n", .{});
}

fn printPlan(
    allocator: std.mem.Allocator,
    writer: *std.Io.Writer,
    cp: diff_mod.PlanEntry,
) !void {
    const trunc_title = try truncate(allocator, cp.title, 36);
    defer allocator.free(trunc_title);
    try writer.print("  {s} plan       {s: <36} ({d} tasks)\n", .{
        opPrefix(cp.op),
        trunc_title,
        cp.tasks.len,
    });
    for (cp.tasks) |t| {
        const trunc_t = try truncate(allocator, t.title, 36);
        defer allocator.free(trunc_t);
        try writer.print("  {s}   task     {s: <36}", .{ opPrefix(t.op), trunc_t });
        if (t.touches.len > 0) {
            try writer.print("  touches=", .{});
            for (t.touches, 0..) |slug, i| {
                if (i > 0) try writer.print(",", .{});
                try writer.print("{s}", .{slug});
            }
        }
        try writer.print("\n", .{});
    }
}

fn printOrphanPlan(
    allocator: std.mem.Allocator,
    writer: *std.Io.Writer,
    op: diff_mod.PlanEntry,
) !void {
    const trunc_p = try truncate(allocator, op.title, 36);
    defer allocator.free(trunc_p);
    try writer.print("  - plan       {s: <36} not in current roadmap        [needs --apply-removals]\n", .{trunc_p});
    for (op.tasks) |t| {
        const trunc_t = try truncate(allocator, t.title, 36);
        defer allocator.free(trunc_t);
        try writer.print("  -   task     {s: <36} not in current roadmap        [needs --apply-removals]\n", .{trunc_t});
    }
}

fn opPrefix(op: diff_mod.Op) []const u8 {
    return switch (op) {
        .add => "+",
        .update => "~",
        .remove => "-",
    };
}

/// truncate produces a display-safe owned string at most `max_len` bytes
/// long, ending with "..." when the original was longer.
fn truncate(allocator: std.mem.Allocator, s: []const u8, max_len: usize) ![]u8 {
    if (s.len <= max_len) return try allocator.dupe(u8, s);
    const cut = max_len - 3;
    var out = try allocator.alloc(u8, max_len);
    @memcpy(out[0..cut], s[0..cut]);
    @memcpy(out[cut..max_len], "...");
    return out;
}

// =========================================================================
// JSON renderer
// =========================================================================

/// renderJson writes the diff as a pretty-printed JSON object.
///
/// Schema mirrors Go's `JSONDiff`:
///   { anchor_plan_id, assoc_slug, anchor_slug,
///     entities: [{op, kind, title, scope?, derives_from?, touches?}, ...],
///     summary: {additions, updates, removals},
///     coverage: {total_tasks, tasks_with_slug, tasks_without_slug,
///                uncovered_task_slugs: [...], orphan_scenarios: [...]} }
pub fn renderJson(
    allocator: std.mem.Allocator,
    writer: *std.Io.Writer,
    d: diff_mod.Diff,
) !void {
    const cov = try coverage_mod.compute(allocator, d);
    defer coverage_mod.deinitCoverage(cov, allocator);

    const scope_label_owned: []u8 = if (d.assoc_slug.len > 0)
        try std.fmt.allocPrint(allocator, "assoc:{s}", .{d.assoc_slug})
    else
        try std.fmt.allocPrint(allocator, "plan:{d}", .{d.anchor_plan_id});
    defer allocator.free(scope_label_owned);
    const scope_label: []const u8 = scope_label_owned;

    try writer.print("{{\n  \"anchor_plan_id\": {d},\n  \"assoc_slug\": ", .{d.anchor_plan_id});
    try writeJsonString(writer, d.assoc_slug);
    try writer.print(",\n  \"anchor_slug\": ", .{});
    try writeJsonString(writer, d.anchor_slug);

    try writer.print(",\n  \"entities\": [\n", .{});
    var first = true;

    const anchor_derives = try std.fmt.allocPrint(allocator, "plan:{d}", .{d.anchor_plan_id});
    defer allocator.free(anchor_derives);
    for (d.child_plans) |cp| {
        try emitEntity(allocator, writer, &first, opName(cp.op), "plan", cp.title, scope_label, anchor_derives, null);
        for (cp.tasks) |t| {
            const derives = try std.fmt.allocPrint(allocator, "plan:{s}", .{cp.title});
            defer allocator.free(derives);
            try emitEntity(allocator, writer, &first, opName(t.op), "task", t.title, scope_label, derives, t.touches);
        }
    }
    for (d.orphan_plans) |op| {
        try emitEntity(allocator, writer, &first, opName(op.op), "plan", op.title, scope_label, "", null);
        for (op.tasks) |t| {
            try emitEntity(allocator, writer, &first, opName(t.op), "task", t.title, scope_label, "", null);
        }
    }
    for (d.orphan_tasks) |ot| {
        try emitEntity(allocator, writer, &first, opName(ot.op), "task", ot.title, scope_label, "", null);
    }
    for (d.decisions) |de| {
        try emitEntity(allocator, writer, &first, opName(de.op), "decision", de.title, scope_label, anchor_derives, null);
    }
    for (d.new_questions) |q| {
        try emitEntity(allocator, writer, &first, "add", "question", q.title, scope_label, "", null);
    }
    for (d.updated_question_status) |sc| {
        try emitEntity(allocator, writer, &first, "update", "question", sc.question_title, scope_label, "", null);
    }

    try writer.print("\n  ],\n", .{});

    try writer.print("  \"summary\": {{\n    \"additions\": {d},\n    \"updates\": {d},\n    \"removals\": {d}\n  }},\n", .{
        d.totalAdditions(),
        d.totalUpdates(),
        d.totalRemovals(),
    });

    try writer.print("  \"coverage\": {{\n    \"total_tasks\": {d},\n    \"tasks_with_slug\": {d},\n    \"tasks_without_slug\": {d},\n    \"uncovered_task_slugs\": [", .{
        cov.total_tasks,
        cov.tasks_with_slug,
        cov.tasks_without_slug,
    });
    for (cov.uncovered_task_slugs, 0..) |s, i| {
        if (i > 0) try writer.print(", ", .{});
        try writeJsonString(writer, s);
    }
    try writer.print("],\n    \"orphan_scenarios\": [", .{});
    for (cov.orphan_scenarios, 0..) |s, i| {
        if (i > 0) try writer.print(", ", .{});
        try writeJsonString(writer, s);
    }
    try writer.print("]\n  }},\n", .{});

    // slug_collisions: task slug ADD proposals that already exist globally.
    try writer.print("  \"slug_collisions\": [", .{});
    for (d.slug_collisions, 0..) |sc, i| {
        if (i > 0) try writer.print(", ", .{});
        try writer.print("{{\"slug\": ", .{});
        try writeJsonString(writer, sc.slug);
        try writer.print(", \"existing_task_id\": {d}, \"existing_plan_id\": {d}}}", .{
            sc.existing_task_id, sc.existing_plan_id,
        });
    }
    try writer.print("]\n}}\n", .{});
}

fn opName(op: diff_mod.Op) []const u8 {
    return switch (op) {
        .add => "add",
        .update => "update",
        .remove => "remove",
    };
}

fn emitEntity(
    _: std.mem.Allocator,
    writer: *std.Io.Writer,
    first: *bool,
    op: []const u8,
    kind: []const u8,
    title: []const u8,
    scope: []const u8,
    derives_from: []const u8,
    touches: ?[]const []const u8,
) !void {
    if (!first.*) try writer.print(",\n", .{});
    first.* = false;
    try writer.print("    {{\"op\": \"{s}\", \"kind\": \"{s}\", \"title\": ", .{ op, kind });
    try writeJsonString(writer, title);
    if (scope.len > 0) {
        try writer.print(", \"scope\": ", .{});
        try writeJsonString(writer, scope);
    }
    if (derives_from.len > 0) {
        try writer.print(", \"derives_from\": ", .{});
        try writeJsonString(writer, derives_from);
    }
    if (touches) |slugs| if (slugs.len > 0) {
        try writer.print(", \"touches\": [", .{});
        for (slugs, 0..) |s, i| {
            if (i > 0) try writer.print(", ", .{});
            try writeJsonString(writer, s);
        }
        try writer.print("]", .{});
    };
    try writer.print("}}", .{});
}

fn writeJsonString(writer: *std.Io.Writer, s: []const u8) !void {
    try std.json.Stringify.encodeJsonString(s, .{}, writer);
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "renderText: empty diff prints Nothing to do." {
    const a = testing.allocator;
    const d = diff_mod.Diff{
        .anchor_plan_id = 7,
        .anchor_slug = try a.dupe(u8, "anchor"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "draft"),
    };
    defer diff_mod.deinitDiff(d, a);

    var fb: [1024]u8 = undefined;
    var fbw = std.Io.Writer.fixed(&fb);
    try renderText(a, &fbw, d, false);
    const out = fbw.buffered();
    try testing.expect(std.mem.indexOf(u8, out, "0 additions, 0 updates, 0 proposed removals.") != null);
    try testing.expect(std.mem.indexOf(u8, out, "Nothing to do.") != null);
    try testing.expect(std.mem.indexOf(u8, out, "coverage: 0 tasks") != null);
}

test "renderJson: empty diff has totals and coverage zeroed" {
    const a = testing.allocator;
    const d = diff_mod.Diff{
        .anchor_plan_id = 42,
        .anchor_slug = try a.dupe(u8, "anchor"),
        .assoc_slug = try a.dupe(u8, ""),
        .current_status = try a.dupe(u8, "draft"),
    };
    defer diff_mod.deinitDiff(d, a);

    var fb: [2048]u8 = undefined;
    var fbw = std.Io.Writer.fixed(&fb);
    try renderJson(a, &fbw, d);
    const out = fbw.buffered();
    try testing.expect(std.mem.indexOf(u8, out, "\"anchor_plan_id\": 42") != null);
    try testing.expect(std.mem.indexOf(u8, out, "\"additions\": 0") != null);
    try testing.expect(std.mem.indexOf(u8, out, "\"total_tasks\": 0") != null);
}
