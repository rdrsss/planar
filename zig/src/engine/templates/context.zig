//! engine/templates/context — data passed to the template renderer.
//!
//! Mirrors Go `internal/templates/context.go`. Fields are populated by the
//! per-entity Build*Context functions in `builder.zig`; not every field is
//! meaningful for every template (e.g. `Task` is zero-valued when rendering
//! a plan-level template). The renderer treats absent fields as empty
//! strings, matching Go's `text/template` default behavior.

const std = @import("std");

/// PlanInfo carries the fields from a plans row needed by the template renderer.
/// Used for both the anchor feature plan and child plans.
pub const PlanInfo = struct {
    id: i64 = 0,
    slug: []const u8 = "",
    title: []const u8 = "",
    body: []const u8 = "",
    scope_kind: []const u8 = "",
    scope_id: i64 = 0,
    status: []const u8 = "",
};

/// TaskInfo carries the fields from a tasks row needed by the template renderer.
pub const TaskInfo = struct {
    id: i64 = 0,
    title: []const u8 = "",
    body: []const u8 = "",
    status: []const u8 = "",
    priority: i64 = 0,
    scope_kind: []const u8 = "",
    scope_id: i64 = 0,
};

/// ScenarioInfo carries the fields from a test_scenarios row needed by the
/// template renderer.
pub const ScenarioInfo = struct {
    id: i64 = 0,
    title: []const u8 = "",
    body: []const u8 = "",
};

/// AssocInfo carries the association identity fields needed by the template
/// renderer.
pub const AssocInfo = struct {
    slug: []const u8 = "",
    name: []const u8 = "",
};

/// ChildRef is a lightweight reference to a child plan or task within the
/// feature tree.
pub const ChildRef = struct {
    title: []const u8 = "",
    external_key: []const u8 = "",
};

/// Context is the data structure passed to the renderer for every template
/// execution. The exported field names match Go's `Context` struct so
/// templates authored against the Go binary work unchanged against the Zig
/// binary.
pub const Context = struct {
    feature: PlanInfo = .{},
    plan: PlanInfo = .{},
    task: TaskInfo = .{},
    scenario: ScenarioInfo = .{},
    touches: []const []const u8 = &.{},
    assoc: AssocInfo = .{},
    external_key: []const u8 = "",
    children: []const ChildRef = &.{},
};

/// stubContext returns a Context with every field populated by a small
/// fixed sentinel value. Used by `validate.zig` to smoke-render templates
/// without touching the database.
pub fn stubContext() Context {
    return .{
        .feature = .{ .id = 1, .slug = "stub-feature", .title = "stub-feature", .body = "stub body", .status = "drafting" },
        .plan = .{ .id = 1, .slug = "stub-plan", .title = "stub-plan", .body = "stub body", .status = "drafting" },
        .task = .{ .id = 1, .title = "stub-task", .body = "stub body", .status = "todo" },
        .scenario = .{ .id = 1, .title = "stub-scenario", .body = "stub body" },
        .touches = &.{"stub/repo"},
        .assoc = .{ .slug = "stub-assoc", .name = "Stub Assoc" },
        .external_key = "STUB-1",
        .children = &.{},
    };
}
