//! engine/templates/builder — load a render Context from the database.
//!
//! Mirrors Go `internal/templates/builder.go`. Three entry points:
//!   buildTaskContext     — task → anchor plan → assoc → touches → external
//!   buildPlanContext     — plan → anchor plan → assoc → touches → external
//!   buildScenarioContext — scenario → anchor plan → assoc → external

const std = @import("std");
const db = @import("db");
const context = @import("context.zig");

pub const Context = context.Context;

/// OwnedContext wraps a render Context with the allocations needed to back
/// every string slice. Call `deinit` to release them.
pub const OwnedContext = struct {
    arena: *std.heap.ArenaAllocator,
    ctx: Context,

    pub fn deinit(self: *OwnedContext) void {
        const child = self.arena.child_allocator;
        self.arena.deinit();
        child.destroy(self.arena);
    }
};

/// buildTaskContext returns the rendering context for a task-oriented template.
pub fn buildTaskContext(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    task_id: i64,
) !OwnedContext {
    const arena = try allocator.create(std.heap.ArenaAllocator);
    arena.* = .init(allocator);
    errdefer {
        arena.deinit();
        allocator.destroy(arena);
    }
    const a = arena.allocator();

    var task: context.TaskInfo = .{};
    {
        // scope_id is `integer` in the schema (see migrations/00003); read
        // it directly with columnInt and coalesce nulls to 0. Previously
        // we read it as text and parseInt'd, which silently allocated and
        // could lose precision on edge cases.
        var stmt = try d.prepare("select id, coalesce(title,''), coalesce(body,''), coalesce(status,''), coalesce(priority,0), coalesce(scope_kind,''), coalesce(scope_id,0) from tasks where id = ?");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = task_id }});
        const step = try stmt.step();
        if (step == .done) return error.NotFound;
        task = .{
            .id = stmt.columnInt(0),
            .title = try stmt.columnTextAlloc(1, a),
            .body = try stmt.columnTextAlloc(2, a),
            .status = try stmt.columnTextAlloc(3, a),
            .priority = stmt.columnInt(4),
            .scope_kind = try stmt.columnTextAlloc(5, a),
            .scope_id = stmt.columnInt(6),
        };
    }

    const anchor_plan = try findAnchorPlanForEntity(a, d, "task", task_id);
    const assoc = try loadAssocByID(a, d, anchor_plan.scope_id);
    const touches = try loadTouches(a, d, "task", task_id);
    const external_key = try loadExternalKey(a, d, "task", task_id);

    return .{
        .arena = arena,
        .ctx = .{
            .feature = anchor_plan,
            .plan = anchor_plan,
            .task = task,
            .assoc = assoc,
            .touches = touches,
            .external_key = external_key,
        },
    };
}

/// buildPlanContext returns the rendering context for a plan-oriented template.
pub fn buildPlanContext(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    plan_id: i64,
) !OwnedContext {
    const arena = try allocator.create(std.heap.ArenaAllocator);
    arena.* = .init(allocator);
    errdefer {
        arena.deinit();
        allocator.destroy(arena);
    }
    const a = arena.allocator();

    const plan = try loadPlan(a, d, plan_id);
    var anchor_plan: context.PlanInfo = plan;
    if (!try isTopLevelPlan(d, plan_id)) {
        anchor_plan = try findAnchorPlanForEntity(a, d, "plan", plan_id);
    }
    const assoc = try loadAssocByID(a, d, anchor_plan.scope_id);
    const touches = try loadTouches(a, d, "plan", plan_id);
    const external_key = try loadExternalKey(a, d, "plan", plan_id);

    return .{
        .arena = arena,
        .ctx = .{
            .feature = anchor_plan,
            .plan = plan,
            .assoc = assoc,
            .touches = touches,
            .external_key = external_key,
        },
    };
}

/// buildScenarioContext returns the rendering context for a test-scenario template.
pub fn buildScenarioContext(
    allocator: std.mem.Allocator,
    d: *db.sqlite.Db,
    scenario_id: i64,
) !OwnedContext {
    const arena = try allocator.create(std.heap.ArenaAllocator);
    arena.* = .init(allocator);
    errdefer {
        arena.deinit();
        allocator.destroy(arena);
    }
    const a = arena.allocator();

    var scenario: context.ScenarioInfo = .{};
    {
        var stmt = try d.prepare("select id, coalesce(title,''), coalesce(body,'') from test_scenarios where id = ?");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = scenario_id }});
        const step = try stmt.step();
        if (step == .done) return error.NotFound;
        scenario = .{
            .id = stmt.columnInt(0),
            .title = try stmt.columnTextAlloc(1, a),
            .body = try stmt.columnTextAlloc(2, a),
        };
    }

    const anchor_plan = try findAnchorPlanForEntity(a, d, "test_scenario", scenario_id);
    const assoc = try loadAssocByID(a, d, anchor_plan.scope_id);
    const external_key = try loadExternalKey(a, d, "test_scenario", scenario_id);

    return .{
        .arena = arena,
        .ctx = .{
            .feature = anchor_plan,
            .plan = anchor_plan,
            .scenario = scenario,
            .assoc = assoc,
            .external_key = external_key,
        },
    };
}

fn loadPlan(a: std.mem.Allocator, d: *db.sqlite.Db, plan_id: i64) !context.PlanInfo {
    // scope_id is `integer` in the schema; read via columnInt with
    // coalesce-to-0 for null. Previously this used columnText + parseInt,
    // which allocated unnecessarily and risked silent precision loss.
    var stmt = try d.prepare(
        \\select id, coalesce(slug,''), coalesce(title,''), coalesce(summary,''), coalesce(status,''),
        \\       coalesce(scope_kind,''), coalesce(scope_id,0)
        \\from plans where id = ?
    );
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = plan_id }});
    const step = try stmt.step();
    if (step == .done) return error.NotFound;
    return .{
        .id = stmt.columnInt(0),
        .slug = try stmt.columnTextAlloc(1, a),
        .title = try stmt.columnTextAlloc(2, a),
        .body = try stmt.columnTextAlloc(3, a),
        .status = try stmt.columnTextAlloc(4, a),
        .scope_kind = try stmt.columnTextAlloc(5, a),
        .scope_id = stmt.columnInt(6),
    };
}

fn isTopLevelPlan(d: *db.sqlite.Db, plan_id: i64) !bool {
    var stmt = try d.prepare("select coalesce(parent_plan_id, 0) from plans where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = plan_id }});
    const step = try stmt.step();
    if (step == .done) return error.NotFound;
    return stmt.columnInt(0) == 0;
}

fn findAnchorPlanForEntity(
    a: std.mem.Allocator,
    d: *db.sqlite.Db,
    from_kind: []const u8,
    from_id: i64,
) !context.PlanInfo {
    // Find an immediate plan via derives-from links.
    var first_plan_id: i64 = 0;
    {
        var stmt = try d.prepare(
            \\select to_id from entity_links
            \\where from_kind = ? and from_id = ? and to_kind = 'plan' and relationship = 'derives-from'
            \\order by id limit 1
        );
        defer stmt.finalize();
        try stmt.bind(&.{ .{ .text = from_kind }, .{ .int = from_id } });
        const step = try stmt.step();
        if (step == .done) {
            // For plans themselves walk up via parent_plan_id directly.
            if (std.mem.eql(u8, from_kind, "plan")) {
                first_plan_id = from_id;
            } else {
                return error.AnchorPlanNotFound;
            }
        } else {
            first_plan_id = stmt.columnInt(0);
        }
    }

    // Walk up via parent_plan_id.
    var current = first_plan_id;
    while (true) {
        var stmt = try d.prepare("select coalesce(parent_plan_id, 0) from plans where id = ?");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = current }});
        const step = try stmt.step();
        if (step == .done) return error.AnchorPlanNotFound;
        const parent = stmt.columnInt(0);
        if (parent == 0) break;
        current = parent;
    }
    return try loadPlan(a, d, current);
}

fn loadAssocByID(a: std.mem.Allocator, d: *db.sqlite.Db, assoc_id: i64) !context.AssocInfo {
    if (assoc_id == 0) return .{};
    var stmt = try d.prepare("select coalesce(slug,''), coalesce(name,'') from associations where id = ?");
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = assoc_id }});
    const step = try stmt.step();
    if (step == .done) return .{};
    return .{
        .slug = try stmt.columnTextAlloc(0, a),
        .name = try stmt.columnTextAlloc(1, a),
    };
}

fn loadTouches(a: std.mem.Allocator, d: *db.sqlite.Db, kind: []const u8, id: i64) ![]const []const u8 {
    var stmt = try d.prepare(
        \\select coalesce(p.slug,'') from entity_links el
        \\join projects p on p.id = el.to_id
        \\where el.from_kind = ? and el.from_id = ? and el.to_kind = 'repo' and el.relationship = 'touches'
        \\order by el.id
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .text = kind }, .{ .int = id } });

    var list: std.ArrayList([]const u8) = .empty;
    while (true) {
        const step = try stmt.step();
        if (step == .done) break;
        try list.append(a, try stmt.columnTextAlloc(0, a));
    }
    return try list.toOwnedSlice(a);
}

fn loadExternalKey(a: std.mem.Allocator, d: *db.sqlite.Db, kind: []const u8, id: i64) ![]const u8 {
    var stmt = try d.prepare(
        \\select coalesce(external_id, '') from external_links
        \\where entity_kind = ? and entity_id = ? and link_role = 'mirror'
        \\order by id limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{ .{ .text = kind }, .{ .int = id } });
    const step = try stmt.step();
    if (step == .done) return "";
    return try stmt.columnTextAlloc(0, a);
}
