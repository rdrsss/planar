//! handlers/test_spec/status — `planar test-spec status <plan> [--json]`
//!
//! Read-only inspector. Walks the anchor plan + its milestones +
//! attached scenarios and emits per-milestone coverage counts.
//!
//! Text mode is a tabular human report; `--json` emits one NDJSON
//! object per milestone followed by a summary object on its own line —
//! the same shape Go's `renderTestSpecStatusJSON` produces, so scripts
//! that parse one binary's output also parse the other's.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

const test_spec_status = engine.planning.test_spec_status;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "test-spec", "status" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const anchor = fetchAnchor(d, ctx.allocator, args.plan) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "plan '{s}' not found", .{args.plan}),
        else => exit.die(ctx, e, "lookup plan '{s}': {s}", .{ args.plan, @errorName(e) }),
    };
    defer ctx.allocator.free(anchor.slug);

    const status = test_spec_status.compute(d, ctx.allocator, anchor.id) catch |e|
        exit.die(ctx, e, "computing test-spec status: {s}", .{@errorName(e)});
    defer test_spec_status.deinit(status, ctx.allocator);

    if (args.json) {
        try renderJson(ctx.stdout, status);
    } else {
        try renderText(ctx.stdout, anchor.slug, status);
    }
}

const Anchor = struct {
    id: i64,
    slug: []const u8,
};

/// fetchAnchor resolves an anchor plan from a numeric id or a top-level
/// plan slug. Restricted to anchors (parent_plan_id IS NULL) to match
/// Go's `workbench.FetchAnchorBySlugOrID` semantics for this verb —
/// `test-spec status` is anchor-scoped.
fn fetchAnchor(d: *db.sqlite.Db, allocator: std.mem.Allocator, arg: []const u8) !Anchor {
    if (std.fmt.parseInt(i64, arg, 10)) |id| {
        var stmt = try d.prepare(
            \\select p.id, p.slug from plans p
            \\ where p.id = ? and p.parent_plan_id is null
        );
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = id }});
        switch (try stmt.step()) {
            .done => return error.NotFound,
            .row => return .{
                .id = stmt.columnInt(0),
                .slug = try stmt.columnTextAlloc(1, allocator),
            },
        }
    } else |_| {}

    var stmt = try d.prepare(
        \\select p.id, p.slug from plans p
        \\ where p.parent_plan_id is null and p.slug = ?
        \\ order by p.id limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .text = arg }});
    switch (try stmt.step()) {
        .done => return error.NotFound,
        .row => return .{
            .id = stmt.columnInt(0),
            .slug = try stmt.columnTextAlloc(1, allocator),
        },
    }
}

// =========================================================================
// Render
// =========================================================================

fn renderText(
    w: *std.Io.Writer,
    anchor_slug: []const u8,
    s: test_spec_status.Status,
) !void {
    try w.print(
        "test-spec status for plan {d} ({s})\n",
        .{ s.summary.anchor_plan_id, anchor_slug },
    );
    try w.print(
        "  {s:<32} {s:>5} {s:>5} {s:>5}   {s:>5} {s:>5} {s:>5} {s:>5} {s:>5}\n",
        .{ "milestone", "tasks", "slug", "cov", "happy", "empty", "error", "edge", "other" },
    );
    var title_buf: [33]u8 = undefined; // 32 chars + null guard slot
    for (s.milestones) |m| {
        const title = truncate32(m.title, &title_buf);
        try w.print(
            "  {s:<32} {d:>5} {d:>5} {d:>5}   {d:>5} {d:>5} {d:>5} {d:>5} {d:>5}\n",
            .{
                title,
                m.total_tasks,
                m.tasks_with_slug,
                m.tasks_covered,
                m.happy,
                m.empty,
                m.@"error",
                m.edge,
                m.other,
            },
        );
    }
    try w.print("\n", .{});
    try w.print(
        "  {d} scenarios total; {d} of {d} slug-bearing tasks covered ({d} total tasks).\n",
        .{
            s.summary.total_scenarios,
            s.summary.tasks_covered,
            s.summary.tasks_with_slug,
            s.summary.total_tasks,
        },
    );
}

/// renderJson emits NDJSON: one object per milestone followed by a
/// summary object on its own line. Mirrors Go's
/// `renderTestSpecStatusJSON`.
fn renderJson(w: *std.Io.Writer, s: test_spec_status.Status) !void {
    for (s.milestones) |m| {
        try w.writeAll("{\"plan_id\":");
        try w.print("{d}", .{m.plan_id});
        try w.writeAll(",\"title\":");
        try output.writeJsonString(w, m.title);
        try w.print(
            ",\"total_tasks\":{d},\"tasks_with_slug\":{d},\"tasks_covered\":{d}",
            .{ m.total_tasks, m.tasks_with_slug, m.tasks_covered },
        );
        try w.print(
            ",\"happy\":{d},\"empty\":{d},\"error\":{d},\"edge\":{d},\"other\":{d}",
            .{ m.happy, m.empty, m.@"error", m.edge, m.other },
        );
        try w.writeAll("}\n");
    }
    try w.print(
        "{{\"anchor_plan_id\":{d},\"total_tasks\":{d},\"tasks_with_slug\":{d},\"tasks_covered\":{d},\"total_scenarios\":{d}}}\n",
        .{
            s.summary.anchor_plan_id,
            s.summary.total_tasks,
            s.summary.tasks_with_slug,
            s.summary.tasks_covered,
            s.summary.total_scenarios,
        },
    );
}

/// truncate32 mirrors Go's helper of the same name: titles longer than
/// 32 chars get truncated to 29 chars plus the literal "..." suffix.
/// Uses the caller-owned `buf` to avoid an allocation per row.
fn truncate32(s: []const u8, buf: *[33]u8) []const u8 {
    if (s.len <= 32) return s;
    @memcpy(buf[0..29], s[0..29]);
    @memcpy(buf[29..32], "...");
    return buf[0..32];
}
