//! Shared, dependency-free contracts used by cockpit view-model domains.

/// Visual status badge rendered by the tree navigator and row widgets.
pub const StatusBadge = enum {
    active,
    stale,
    done,
    todo,
    doing,
    blocked,
    cancelled,
    draft,
    paused,
    abandoned,
    superseded,
    none,

    pub fn glyph(self: StatusBadge) []const u8 {
        return switch (self) {
            .active => "A",
            .stale => "S",
            .done => "D",
            .todo => " ",
            .doing => ">",
            .blocked => "B",
            .cancelled => "X",
            .draft => "d",
            .paused => "P",
            .abandoned => "~",
            .superseded => "^",
            .none => " ",
        };
    }
};

/// Scope selection shared by scope-sensitive cockpit views.
pub const ScopeFilter = union(enum) {
    repo: i64,
    all,
};

/// Stable identifiers consumed by the view switcher and app controller.
pub const ViewId = enum {
    agent_monitor,
    scope_explorer,
    task_board,
    decision_log,
    open_questions,
    coverage_view,
    entity_link_graph,
    external_ops_plane,
    sessions_handoff,
    audit_log,
    cli_history,
    topology,
    utility_view,
};

test "shared view-model contracts compile" {
    const std = @import("std");
    try std.testing.expectEqualStrings("A", StatusBadge.active.glyph());
    try std.testing.expectEqual(ViewId.agent_monitor, ViewId.agent_monitor);
    try std.testing.expectEqual(ScopeFilter.all, ScopeFilter.all);
}
