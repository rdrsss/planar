//! handlers/ui_driver_query — `planar ui-driver-query --task-ids <id,…> [--json]`
//!
//! Detection query for the UI-verification harness (P3b): given a set of
//! claimed task ids, return whether any of them has a linked `design_note`
//! artifact (P3a registers these). The `pl-ui-driver-hook` skill shells this
//! verb after a Bash presence-gate to decide whether the orchestrator should
//! dispatch the `ui-driver` worker (Phase 3.6).
//!
//! Output JSON:
//!   { "dispatch_ui_driver": <bool>,
//!     "design_artifacts": [ { "id": <n>, "source_path": "design/…" }, … ],
//!     "reason": "…" }
//!
//! The `from_kind = 'artifact'` predicate is MANDATORY: `entity_links` is
//! polymorphic with no per-kind foreign key, so the id space is shared across
//! kinds. A bare `a.id = el.from_id` join would false-positive when a
//! non-artifact link's `from_id` numerically collides with an artifact id and
//! dispatch the wrong (or a phantom) design. The regression test seeds such a
//! collision.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "ui-driver-query",
    .desc = "Report whether any claimed task has a linked design_note (UI-driver detection).",
    .long_desc = "Detection query for the UI-verification harness (P3b).\n\n" ++
        "  Given a set of claimed task ids, returns whether any of them has a\n" ++
        "  linked design_note artifact and the matching artifacts. The\n" ++
        "  pl-ui-driver-hook skill shells this after a Bash presence-gate to\n" ++
        "  decide whether to dispatch the ui-driver worker (orchestrator Phase\n" ++
        "  3.6). Read-only.",
    .flags = &.{
        .{ .long = "--task-ids", .kind = .string },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

const DesignArtifact = struct {
    id: i64,
    source_path: []const u8,
};

const QueryResult = struct {
    dispatch_ui_driver: bool,
    design_artifacts: []const DesignArtifact,
    reason: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"ui-driver-query"}, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Parse --task-ids (comma-separated integers). Absent / empty / all-invalid
    // is not an error: it simply yields dispatch_ui_driver = false.
    var task_ids: std.ArrayList(i64) = .empty;
    defer task_ids.deinit(ctx.allocator);
    if (args.task_ids) |raw| {
        var it = std.mem.splitScalar(u8, raw, ',');
        while (it.next()) |tok| {
            const trimmed = std.mem.trim(u8, tok, " \t");
            if (trimmed.len == 0) continue;
            const id = std.fmt.parseInt(i64, trimmed, 10) catch
                exit.die(ctx, error.InvalidInput, "invalid --task-ids entry '{s}': expected an integer", .{trimmed});
            try task_ids.append(ctx.allocator, id);
        }
    }

    var artifacts: std.ArrayList(DesignArtifact) = .empty;
    defer {
        for (artifacts.items) |a| ctx.allocator.free(a.source_path);
        artifacts.deinit(ctx.allocator);
    }

    if (task_ids.items.len > 0) {
        try queryDesignNotes(d, ctx.allocator, task_ids.items, &artifacts);
    }

    const dispatch = artifacts.items.len > 0;
    const reason = if (task_ids.items.len == 0)
        "no task ids provided"
    else if (dispatch)
        "design_note linked to a claimed task"
    else
        "no design_note linked to the claimed task(s)";

    const result = QueryResult{
        .dispatch_ui_driver = dispatch,
        .design_artifacts = artifacts.items,
        .reason = reason,
    };

    if (args.json) {
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("dispatch_ui_driver: {}\nreason: {s}\n", .{ dispatch, reason });
        for (result.design_artifacts) |a| {
            try ctx.stdout.print("  design_note {d}: {s}\n", .{ a.id, a.source_path });
        }
    }
}

/// queryDesignNotes runs the polymorphic-safe detection join and appends one
/// `DesignArtifact` per DISTINCT design_note linked (via cites/addresses) to
/// any of the given task ids. `from_kind = 'artifact'` is mandatory (see file
/// header). source_path strings are allocator-owned; caller frees.
fn queryDesignNotes(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    task_ids: []const i64,
    out: *std.ArrayList(DesignArtifact),
) !void {
    var sql: std.ArrayList(u8) = .empty;
    defer sql.deinit(allocator);
    try sql.appendSlice(allocator,
        \\select distinct a.id, a.source_path
        \\  from entity_links el
        \\  join artifacts a on a.id = el.from_id
        \\ where el.from_kind = 'artifact'
        \\   and el.to_kind = 'task'
        \\   and a.kind = 'design_note'
        \\   and el.relationship in ('cites','addresses')
        \\   and el.to_id in (
    );
    for (task_ids, 0..) |_, i| {
        if (i > 0) try sql.appendSlice(allocator, ",");
        try sql.appendSlice(allocator, "?");
    }
    try sql.appendSlice(allocator, ") order by a.id");

    const sql_z = try allocator.dupeZ(u8, sql.items);
    defer allocator.free(sql_z);

    var params: std.ArrayList(db.sqlite.Param) = .empty;
    defer params.deinit(allocator);
    for (task_ids) |id| try params.append(allocator, .{ .int = id });

    var stmt = try d.prepare(sql_z);
    defer stmt.finalize();
    try stmt.bind(params.items);

    while (true) {
        switch (try stmt.step()) {
            .done => break,
            .row => {
                const id = stmt.columnInt(0);
                const sp = (try stmt.columnTextOpt(1, allocator)) orelse try allocator.dupe(u8, "");
                try out.append(allocator, .{ .id = id, .source_path = sp });
            },
        }
    }
}
