//! engine/workspace/regenerate — render workspace AGENTS.md from routing table.

const std = @import("std");
const db = @import("db");
const identity = @import("../identity.zig");
const routing = @import("routing.zig");
const manifest = @import("../docs/manifest.zig");

const default_agents_template = @embedFile("default_agents_template.md");

pub const Result = struct {
    agents_path: []const u8,
    manifest_path: []const u8,
    project_count: i64,
    bytes_written: i64,
    manifest_root: []const u8,
};

pub fn deinitResult(result: Result, allocator: std.mem.Allocator) void {
    allocator.free(result.agents_path);
    allocator.free(result.manifest_path);
    allocator.free(result.manifest_root);
}

const PlanSummary = struct {
    id: i64,
    title: []const u8,
};

pub fn regenerate(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    io: std.Io,
    environ: std.process.Environ,
    org_id: i64,
) !Result {
    const layout = try identity.workspace.loadLayout(allocator, environ, org_id);
    defer identity.workspace.deinitLayout(layout, allocator);

    const table = routing.read(layout.routing_table, allocator) catch |err| switch (err) {
        error.FileNotFound => return error.NotFound,
        else => return err,
    };
    defer routing.deinit(table, allocator);

    const plans = try queryPlanSummaries(d, allocator, org_id, "plans", "active");
    defer deinitPlanSummaries(plans, allocator);
    const questions = try queryPlanSummaries(d, allocator, org_id, "questions", "open");
    defer deinitPlanSummaries(questions, allocator);

    const template_path = try agentsTemplatePath(allocator, environ);
    defer allocator.free(template_path);
    const rendered = try renderAgents(allocator, io, table, layout.dir, plans, questions, template_path);
    defer allocator.free(rendered);

    try std.Io.Dir.cwd().createDirPath(io, layout.dir);
    try writeFileAtomic(allocator, layout.agents_md, rendered);

    const docs_manifest = try manifest.build(layout.dir, allocator);
    defer manifest.deinitManifest(docs_manifest, allocator);
    const manifest_path = try std.fs.path.join(allocator, &.{ layout.dir, manifest.file_name });
    errdefer allocator.free(manifest_path);
    try manifest.write(manifest_path, docs_manifest, allocator);

    return .{
        .agents_path = try allocator.dupe(u8, layout.agents_md),
        .manifest_path = manifest_path,
        .project_count = @intCast(table.projects.len),
        .bytes_written = @intCast(rendered.len),
        .manifest_root = try allocator.dupe(u8, docs_manifest.root),
    };
}

fn renderAgents(
    allocator: std.mem.Allocator,
    io: std.Io,
    table: routing.RoutingTable,
    source_path: []const u8,
    plans: []const PlanSummary,
    questions: []const PlanSummary,
    template_path: []const u8,
) ![]u8 {
    const custom = readOptionalTemplate(allocator, io, template_path) catch null;
    defer if (custom) |raw| allocator.free(raw);

    const body = if (custom) |raw| raw else default_agents_template;
    const view = AgentsView{
        .workspace_name = table.workspace_name,
        .workspace_slug = table.workspace_slug,
        .workspace_id = table.workspace_id,
        .generated_at = table.generated_at,
        .source_path = source_path,
        .projects = table.projects,
        .dependency_edges = table.cross_repo.dependency_edges,
        .active_plans = plans,
        .open_questions = questions,
    };
    return try renderTemplate(allocator, body, view);
}

fn readOptionalTemplate(allocator: std.mem.Allocator, io: std.Io, path: []const u8) !?[]u8 {
    var file = blk: {
        if (std.fs.path.isAbsolute(path)) {
            break :blk std.Io.Dir.openFileAbsolute(io, path, .{}) catch |err| switch (err) {
                error.FileNotFound => return null,
                else => return err,
            };
        }
        break :blk std.Io.Dir.cwd().openFile(io, path, .{}) catch |err| switch (err) {
            error.FileNotFound => return null,
            else => return err,
        };
    };
    defer file.close(io);

    var reader = file.reader(io, &.{});
    return reader.interface.allocRemaining(allocator, std.Io.Limit.limited(512 * 1024)) catch |err| switch (err) {
        error.ReadFailed => return reader.err.?,
        else => return err,
    };
}

const AgentsView = struct {
    workspace_name: []const u8,
    workspace_slug: []const u8,
    workspace_id: i64,
    generated_at: []const u8,
    source_path: []const u8,
    projects: []const routing.ProjectRoute,
    dependency_edges: []const routing.DependencyEdge,
    active_plans: []const PlanSummary,
    open_questions: []const PlanSummary,
};

const CurrentItem = union(enum) {
    project: *const routing.ProjectRoute,
    edge: *const routing.DependencyEdge,
    plan: *const PlanSummary,
    question: *const PlanSummary,
};

fn renderTemplate(allocator: std.mem.Allocator, src: []const u8, view: AgentsView) anyerror![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    var cursor: usize = 0;
    try renderRange(allocator, src, &cursor, view, null, &out, null);
    return try out.toOwnedSlice(allocator);
}

fn renderRange(
    allocator: std.mem.Allocator,
    src: []const u8,
    cursor: *usize,
    view: AgentsView,
    item: ?CurrentItem,
    out: *std.ArrayList(u8),
    stop_keyword: ?[]const u8,
) anyerror!void {
    while (cursor.* < src.len) {
        const open = std.mem.indexOfPos(u8, src, cursor.*, "{{") orelse {
            try out.appendSlice(allocator, src[cursor.*..]);
            cursor.* = src.len;
            if (stop_keyword != null) return error.UnexpectedEnd;
            return;
        };
        try out.appendSlice(allocator, src[cursor.*..open]);
        const close = std.mem.indexOfPos(u8, src, open + 2, "}}") orelse return error.UnclosedDirective;
        const directive = parseDirective(src[open + 2 .. close]);
        cursor.* = close + 2;

        if (stop_keyword) |kw| {
            if (std.mem.eql(u8, directive, kw)) return;
        }

        try execDirective(allocator, src, cursor, view, item, out, directive);
    }
    if (stop_keyword != null) return error.UnexpectedEnd;
}

fn execDirective(
    allocator: std.mem.Allocator,
    src: []const u8,
    cursor: *usize,
    view: AgentsView,
    item: ?CurrentItem,
    out: *std.ArrayList(u8),
    directive: []const u8,
) anyerror!void {
    if (directive.len == 0) return;
    if (std.mem.eql(u8, directive, "end")) return error.UnexpectedEnd;
    if (std.mem.eql(u8, directive, "else")) return error.UnexpectedElse;

    if (std.mem.startsWith(u8, directive, "range ")) {
        const path = std.mem.trim(u8, directive["range ".len..], " \t");
        return execRangeBlock(allocator, src, cursor, view, out, path);
    }
    if (std.mem.startsWith(u8, directive, "if ")) {
        const path = std.mem.trim(u8, directive["if ".len..], " \t");
        return execIfBlock(allocator, src, cursor, view, item, out, path);
    }
    const value = try resolveExpr(allocator, directive, view, item);
    defer allocator.free(value);
    try out.appendSlice(allocator, value);
}

fn execRangeBlock(
    allocator: std.mem.Allocator,
    src: []const u8,
    cursor: *usize,
    view: AgentsView,
    out: *std.ArrayList(u8),
    path: []const u8,
) anyerror!void {
    const body_start = cursor.*;
    var probe = cursor.*;
    var depth: usize = 1;
    while (true) {
        const open = std.mem.indexOfPos(u8, src, probe, "{{") orelse return error.UnclosedDirective;
        const close = std.mem.indexOfPos(u8, src, open + 2, "}}") orelse return error.UnclosedDirective;
        const inner = parseDirective(src[open + 2 .. close]);
        if (std.mem.startsWith(u8, inner, "range ") or std.mem.startsWith(u8, inner, "if ")) {
            depth += 1;
        } else if (std.mem.eql(u8, inner, "end")) {
            depth -= 1;
            if (depth == 0) {
                const body = src[body_start..open];
                cursor.* = close + 2;
                try iterateAndRender(allocator, body, view, out, path);
                return;
            }
        }
        probe = close + 2;
    }
}

fn iterateAndRender(
    allocator: std.mem.Allocator,
    body: []const u8,
    view: AgentsView,
    out: *std.ArrayList(u8),
    path: []const u8,
) anyerror!void {
    if (std.mem.eql(u8, path, ".Projects")) {
        for (view.projects) |project| try renderBody(allocator, body, view, .{ .project = &project }, out);
        return;
    }
    if (std.mem.eql(u8, path, ".DependencyEdges")) {
        for (view.dependency_edges) |edge| try renderBody(allocator, body, view, .{ .edge = &edge }, out);
        return;
    }
    if (std.mem.eql(u8, path, ".ActivePlans")) {
        for (view.active_plans) |plan| try renderBody(allocator, body, view, .{ .plan = &plan }, out);
        return;
    }
    if (std.mem.eql(u8, path, ".OpenQuestions")) {
        for (view.open_questions) |question| try renderBody(allocator, body, view, .{ .question = &question }, out);
        return;
    }
    return error.UnknownField;
}

fn renderBody(
    allocator: std.mem.Allocator,
    body: []const u8,
    view: AgentsView,
    item: CurrentItem,
    out: *std.ArrayList(u8),
) anyerror!void {
    var inner_cursor: usize = 0;
    try renderRange(allocator, body, &inner_cursor, view, item, out, null);
}

fn execIfBlock(
    allocator: std.mem.Allocator,
    src: []const u8,
    cursor: *usize,
    view: AgentsView,
    item: ?CurrentItem,
    out: *std.ArrayList(u8),
    path: []const u8,
) anyerror!void {
    const body_start = cursor.*;
    var probe = cursor.*;
    var depth: usize = 1;
    var else_start: ?usize = null;
    var else_end: ?usize = null;
    while (true) {
        const open = std.mem.indexOfPos(u8, src, probe, "{{") orelse return error.UnclosedDirective;
        const close = std.mem.indexOfPos(u8, src, open + 2, "}}") orelse return error.UnclosedDirective;
        const inner = parseDirective(src[open + 2 .. close]);
        if (std.mem.startsWith(u8, inner, "range ") or std.mem.startsWith(u8, inner, "if ")) {
            depth += 1;
        } else if (std.mem.eql(u8, inner, "end")) {
            depth -= 1;
            if (depth == 0) {
                cursor.* = close + 2;
                const truthy = try evalTruthy(path, view, item);
                const if_body_end = else_start orelse open;
                if (truthy) {
                    var c: usize = 0;
                    try renderRange(allocator, src[body_start..if_body_end], &c, view, item, out, null);
                } else if (else_start != null and else_end != null) {
                    var c: usize = 0;
                    try renderRange(allocator, src[else_end.?..open], &c, view, item, out, null);
                }
                return;
            }
        } else if (depth == 1 and std.mem.eql(u8, inner, "else")) {
            else_start = open;
            else_end = close + 2;
        }
        probe = close + 2;
    }
}

fn evalTruthy(path: []const u8, view: AgentsView, item: ?CurrentItem) anyerror!bool {
    if (std.mem.eql(u8, path, ".Projects")) return view.projects.len > 0;
    if (std.mem.eql(u8, path, ".DependencyEdges")) return view.dependency_edges.len > 0;
    if (std.mem.eql(u8, path, ".ActivePlans")) return view.active_plans.len > 0;
    if (std.mem.eql(u8, path, ".OpenQuestions")) return view.open_questions.len > 0;
    if (item) |it| {
        switch (it) {
            .project => |p| {
                if (std.mem.eql(u8, path, ".Slug")) return p.slug.len > 0;
                if (std.mem.eql(u8, path, ".RootPath")) return p.root_path.len > 0;
                if (std.mem.eql(u8, path, ".Summary")) return p.summary.len > 0;
            },
            .edge => |e| {
                if (std.mem.eql(u8, path, ".From")) return e.from.len > 0;
                if (std.mem.eql(u8, path, ".To")) return e.to.len > 0;
                if (std.mem.eql(u8, path, ".Reason")) return e.reason.len > 0;
            },
            .plan => |p| {
                if (std.mem.eql(u8, path, ".ID")) return p.id != 0;
                if (std.mem.eql(u8, path, ".Title")) return p.title.len > 0;
            },
            .question => |q| {
                if (std.mem.eql(u8, path, ".ID")) return q.id != 0;
                if (std.mem.eql(u8, path, ".Title")) return q.title.len > 0;
            },
        }
    }
    if (std.mem.eql(u8, path, ".WorkspaceName")) return view.workspace_name.len > 0;
    if (std.mem.eql(u8, path, ".WorkspaceSlug")) return view.workspace_slug.len > 0;
    if (std.mem.eql(u8, path, ".WorkspaceID")) return view.workspace_id != 0;
    if (std.mem.eql(u8, path, ".GeneratedAt")) return view.generated_at.len > 0;
    if (std.mem.eql(u8, path, ".SourcePath")) return view.source_path.len > 0;
    return false;
}

fn resolveExpr(
    allocator: std.mem.Allocator,
    expr: []const u8,
    view: AgentsView,
    item: ?CurrentItem,
) anyerror![]u8 {
    if (std.mem.startsWith(u8, expr, "commaJoin ")) {
        const arg = std.mem.trim(u8, expr["commaJoin ".len..], " \t");
        return try resolveCommaJoin(allocator, arg, item);
    }
    if (expr.len == 0 or expr[0] != '.') return error.UnsupportedDirective;

    if (item) |it| {
        switch (it) {
            .project => |p| {
                if (std.mem.eql(u8, expr, ".Slug")) return try allocator.dupe(u8, p.slug);
                if (std.mem.eql(u8, expr, ".RootPath")) return try allocator.dupe(u8, p.root_path);
                if (std.mem.eql(u8, expr, ".Summary")) return try allocator.dupe(u8, p.summary);
            },
            .edge => |e| {
                if (std.mem.eql(u8, expr, ".From")) return try allocator.dupe(u8, e.from);
                if (std.mem.eql(u8, expr, ".To")) return try allocator.dupe(u8, e.to);
                if (std.mem.eql(u8, expr, ".Reason")) return try allocator.dupe(u8, e.reason);
            },
            .plan => |p| {
                if (std.mem.eql(u8, expr, ".ID")) return try std.fmt.allocPrint(allocator, "{d}", .{p.id});
                if (std.mem.eql(u8, expr, ".Title")) return try allocator.dupe(u8, p.title);
            },
            .question => |q| {
                if (std.mem.eql(u8, expr, ".ID")) return try std.fmt.allocPrint(allocator, "{d}", .{q.id});
                if (std.mem.eql(u8, expr, ".Title")) return try allocator.dupe(u8, q.title);
            },
        }
    }

    if (std.mem.eql(u8, expr, ".WorkspaceName")) return try allocator.dupe(u8, view.workspace_name);
    if (std.mem.eql(u8, expr, ".WorkspaceSlug")) return try allocator.dupe(u8, view.workspace_slug);
    if (std.mem.eql(u8, expr, ".WorkspaceID")) return try std.fmt.allocPrint(allocator, "{d}", .{view.workspace_id});
    if (std.mem.eql(u8, expr, ".GeneratedAt")) return try allocator.dupe(u8, view.generated_at);
    if (std.mem.eql(u8, expr, ".SourcePath")) return try allocator.dupe(u8, view.source_path);

    return error.UnknownField;
}

fn resolveCommaJoin(
    allocator: std.mem.Allocator,
    arg: []const u8,
    item: ?CurrentItem,
) anyerror![]u8 {
    if (!std.mem.eql(u8, arg, ".Capabilities")) return error.UnknownField;
    const it = item orelse return error.UnknownField;
    switch (it) {
        .project => |p| {
            var out = std.ArrayList(u8).empty;
            errdefer out.deinit(allocator);
            for (p.capabilities, 0..) |cap, i| {
                if (i > 0) try out.appendSlice(allocator, ", ");
                try out.appendSlice(allocator, cap);
            }
            return try out.toOwnedSlice(allocator);
        },
        else => return error.UnknownField,
    }
}

fn parseDirective(raw: []const u8) []const u8 {
    return std.mem.trim(u8, raw, " \t\r\n-");
}

fn renderAgentsDefault(
    allocator: std.mem.Allocator,
    table: routing.RoutingTable,
    source_path: []const u8,
    plans: []const PlanSummary,
    questions: []const PlanSummary,
) ![]u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    errdefer out.deinit();

    try out.writer.print(
        \\---
        \\title: "Workspace AGENTS Guide — {s}"
        \\doc_kind: agents
        \\template_version: 1
        \\source_artifacts: []
        \\source_decisions: []
        \\source_plans: []
        \\source_versions: {{}}
        \\regenerated_at: "{s}"
        \\regenerated_by: planar-workspace-regenerate
        \\references: {{}}
        \\---
        \\
        \\# Workspace AGENTS Guide — {s}
        \\
        \\**Workspace:** org:{s} (id {d})
        \\**Generated:** {s}
        \\**Source:** {s}
        \\
        \\This file is auto-generated. Do not edit directly; run
        \\`planar workspace regenerate` to refresh.
        \\
        \\## Projects in this workspace
        \\
    , .{
        table.workspace_name,
        table.generated_at,
        table.workspace_name,
        table.workspace_slug,
        table.workspace_id,
        table.generated_at,
        source_path,
    });

    if (table.projects.len == 0) {
        try out.writer.print("_No projects registered. Run `planar assoc add ...` to add members._\n\n", .{});
    } else {
        try out.writer.print("| Slug | Path | Purpose | Capabilities |\n|---|---|---|---|\n", .{});
        for (table.projects) |project| {
            try out.writer.print("| {s} | {s} | {s} | ", .{
                project.slug,
                project.root_path,
                project.summary,
            });
            for (project.capabilities, 0..) |cap, i| {
                if (i > 0) try out.writer.print(", ", .{});
                try out.writer.print("{s}", .{cap});
            }
            try out.writer.print(" |\n", .{});
        }
        try out.writer.print("\n", .{});
    }

    try out.writer.print("## Cross-repo dependencies\n\n", .{});
    if (table.cross_repo.dependency_edges.len == 0) {
        try out.writer.print("_No cross-repo dependencies detected._\n\n", .{});
    } else {
        for (table.cross_repo.dependency_edges) |edge| {
            try out.writer.print("- {s} → {s} ({s})\n", .{ edge.from, edge.to, edge.reason });
        }
        try out.writer.print("\n", .{});
    }

    try out.writer.print("## Cross-repo plans in flight\n\n", .{});
    if (plans.len == 0) {
        try out.writer.print("_No active plans scoped to this workspace._\n\n", .{});
    } else {
        for (plans) |plan| {
            try out.writer.print("- plan {d}: {s}\n", .{ plan.id, plan.title });
        }
        try out.writer.print("\n", .{});
    }

    try out.writer.print("## Open cross-repo questions\n\n", .{});
    if (questions.len == 0) {
        try out.writer.print("_No open questions scoped to this workspace._\n\n", .{});
    } else {
        for (questions) |question| {
            try out.writer.print("- q{d}: {s}\n", .{ question.id, question.title });
        }
        try out.writer.print("\n", .{});
    }

    try out.writer.print(
        \\## Quick-reference CLI
        \\
        \\- `planar tree --scope org:{s}` — see all in-flight work
        \\- `planar task list --scope org:{s}` — cross-repo tasks
        \\- `planar plan show <id>` — drill into a specific plan
        \\- `planar workspace regenerate` — refresh this file after changes
        \\
        \\## How to dispatch work
        \\
        \\Cross-repo coordination tasks live in `org:{s}`.
        \\Repo-specific work belongs in the per-project scope. Pass
        \\`--scope <slug>` explicitly when running write verbs from outside
        \\the target repo; the strict resolver refuses to guess.
    , .{
        table.workspace_slug,
        table.workspace_slug,
        table.workspace_slug,
    });

    try out.writer.flush();
    return out.toOwnedSlice();
}

fn agentsTemplatePath(allocator: std.mem.Allocator, environ: std.process.Environ) ![]u8 {
    const home = try identity.workspace.planarHome(allocator, environ);
    defer allocator.free(home);
    return try std.fs.path.join(allocator, &.{ home, "templates", "doc-prompts", "agents.md" });
}

fn queryPlanSummaries(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    org_id: i64,
    comptime table_name: []const u8,
    comptime status: []const u8,
) ![]const PlanSummary {
    const sql = comptime std.fmt.comptimePrint(
        \\select id, coalesce(title, '')
        \\from {s}
        \\where scope_kind = 'association' and scope_id = ? and status = '{s}'
        \\order by id
    , .{ table_name, status });
    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = org_id }}) catch return error.QueryFailed;

    var out = std.ArrayList(PlanSummary).empty;
    errdefer {
        for (out.items) |row| allocator.free(row.title);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .title = try stmt.columnTextAlloc(1, allocator),
            }),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn deinitPlanSummaries(rows: []const PlanSummary, allocator: std.mem.Allocator) void {
    for (rows) |row| allocator.free(row.title);
    allocator.free(rows);
}

fn writeFileAtomic(allocator: std.mem.Allocator, path: []const u8, content: []const u8) !void {
    const tmp = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp);
    {
        try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = tmp, .data = content });
    }
    try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), path, fsIo());
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}
