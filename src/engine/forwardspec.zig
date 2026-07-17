//! Shared forward-spec selection and materialization for import/synthesize.

const std = @import("std");
const db = @import("db");
const planning = @import("planning.zig");
const workbench = @import("workbench.zig");

pub const Report = struct {
    plans_created: usize = 0,
    plans_updated: usize = 0,
    artifacts_created: usize = 0,
};

/// Materialize the selected proposals as draft anchor plans. `specs` may be
/// either import.ForwardSpec or synthesize.ForwardSpec; both expose the same
/// slug/title/goal/summary fields.
pub fn apply(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    specs: anytype,
    selection: ?[]const u8,
    scope: ?[]const u8,
) !Report {
    const raw = selection orelse return .{};
    const trimmed = std.mem.trim(u8, raw, " \t\r\n");
    if (trimmed.len == 0) return error.InvalidInput;

    const all_statuses = [_]planning.plan.Status{ .draft, .active, .paused, .done, .abandoned };
    const existing = try planning.plan.list(d, allocator, .{ .scope = scope, .statuses = &all_statuses });
    defer planning.plan.deinitMany(existing, allocator);

    var selected: std.ArrayList(usize) = .empty;
    defer selected.deinit(allocator);
    if (std.mem.eql(u8, trimmed, "all")) {
        for (specs, 0..) |_, i| try selected.append(allocator, i);
    } else {
        var seen = std.StringHashMap(void).init(allocator);
        defer seen.deinit();
        var it = std.mem.splitScalar(u8, trimmed, ',');
        while (it.next()) |part| {
            const slug = std.mem.trim(u8, part, " \t\r\n");
            if (slug.len == 0 or seen.contains(slug)) return error.InvalidInput;
            try seen.put(slug, {});

            var found: ?usize = null;
            for (specs, 0..) |spec, i| {
                if (std.mem.eql(u8, spec.slug, slug)) {
                    found = i;
                    break;
                }
            }
            try selected.append(allocator, found orelse return error.InvalidInput);
        }
    }

    if (selected.items.len == 0 and !std.mem.eql(u8, trimmed, "all")) return error.InvalidInput;
    var report: Report = .{};
    for (selected.items) |i| try applyOne(d, allocator, specs[i], scope, existing, &report);
    return report;
}

fn applyOne(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    spec: anytype,
    scope: ?[]const u8,
    existing: []const planning.plan.Plan,
    report: *Report,
) !void {
    const summary = if (spec.summary.len > 0) spec.summary else spec.goal;
    const plan_id = if (findExisting(existing, spec.slug)) |id| blk: {
        _ = d.execParams(
            "update plans set title=?, summary=?, updated_at=datetime('now') where id=?",
            &.{ .{ .text = spec.title }, .{ .text = summary }, .{ .int = id } },
        ) catch return error.QueryFailed;
        report.plans_updated += 1;
        break :blk id;
    } else blk: {
        const created = try planning.plan.create(d, allocator, .{
            .title = spec.title,
            .slug = spec.slug,
            .summary = if (summary.len > 0) summary else null,
            .status = .draft,
            .scope = scope,
        });
        defer planning.plan.deinit(created, allocator);
        report.plans_created += 1;
        break :blk created.id;
    };

    report.artifacts_created += try ensureSeedArtifacts(d, allocator, spec, plan_id, scope);
    const pushed = try workbench.sync.push(d, allocator, plan_id, .failures, false);
    workbench.sync.deinitResult(allocator, pushed);
}

fn findExisting(plans: []const planning.plan.Plan, slug: []const u8) ?i64 {
    for (plans) |plan| {
        if (plan.parent_plan_id == null and std.mem.eql(u8, plan.slug, slug)) return plan.id;
    }
    return null;
}

fn ensureSeedArtifacts(d: *db.sqlite.Db, allocator: std.mem.Allocator, spec: anytype, plan_id: i64, scope: ?[]const u8) !usize {
    var count: usize = 0;
    const summary = if (spec.summary.len > 0) spec.summary else "To be refined during specification.";

    const product_title = try std.fmt.allocPrint(allocator, "{s} — Product Spec", .{spec.title});
    defer allocator.free(product_title);
    const product_body = try std.fmt.allocPrint(
        allocator,
        "# {s} — Product Spec\n\n> Seeded from forward proposal `{s}`.\n\n## Goal\n\n{s}\n\n## Summary\n\n{s}\n",
        .{ spec.title, spec.slug, spec.goal, summary },
    );
    defer allocator.free(product_body);
    count += try createArtifactIfMissing(d, allocator, product_title, .product_spec, product_body, "pl-forward-spec://product_spec", plan_id, scope);

    const tech_title = try std.fmt.allocPrint(allocator, "{s} — Tech Spec", .{spec.title});
    defer allocator.free(tech_title);
    const tech_body = try std.fmt.allocPrint(
        allocator,
        "# {s} — Tech Spec\n\n> Seeded from forward proposal `{s}`.\n\n## Context\n\n{s}\n\n## Design\n\nTo be developed during technical planning.\n",
        .{ spec.title, spec.slug, spec.goal },
    );
    defer allocator.free(tech_body);
    count += try createArtifactIfMissing(d, allocator, tech_title, .tech_spec, tech_body, "pl-forward-spec://tech_spec", plan_id, scope);

    const roadmap_title = try std.fmt.allocPrint(allocator, "{s} — Roadmap", .{spec.title});
    defer allocator.free(roadmap_title);
    const roadmap_body = try std.fmt.allocPrint(
        allocator,
        "# {s} — Roadmap\n\n> Seeded from forward proposal `{s}`.\n\n## M1 — Refine and approve\n\n- [ ] Refine product and technical specifications.\n- [ ] Review acceptance criteria and implementation sequence.\n",
        .{ spec.title, spec.slug },
    );
    defer allocator.free(roadmap_body);
    count += try createArtifactIfMissing(d, allocator, roadmap_title, .roadmap, roadmap_body, "pl-forward-spec://roadmap", plan_id, scope);
    return count;
}

fn createArtifactIfMissing(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    title: []const u8,
    kind: planning.artifact.Kind,
    body: []const u8,
    source_path: []const u8,
    plan_id: i64,
    scope: ?[]const u8,
) !usize {
    var stmt = try d.prepare(
        \\select a.id from artifacts a
        \\join entity_links el on el.from_kind='artifact' and el.from_id=a.id
        \\ and el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
        \\where coalesce(a.source_path,'')=? limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .int = plan_id }, .{ .text = source_path } });
    if (try stmt.step() == .row) return 0;

    const created = try planning.artifact.create(d, allocator, .{
        .title = title,
        .kind = kind,
        .body = body,
        .source_path = source_path,
        .plan_id = plan_id,
        .scope = scope,
    });
    defer planning.artifact.deinit(created, allocator);
    return 1;
}
