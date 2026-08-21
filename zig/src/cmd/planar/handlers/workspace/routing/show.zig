const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workspace", "routing", "show" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const org = engine.identity.workspace.resolveOrg(d, ctx.allocator, args.workspace) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "no org associations registered; create one with `planar workspace init`", .{}),
        error.InvalidInput => exit.die(ctx, error.InvalidInput, "multiple org associations registered; pass the workspace slug or id explicitly", .{}),
        else => exit.die(ctx, e, "resolving workspace failed: {s}", .{@errorName(e)}),
    };
    defer engine.identity.workspace.deinitWorkspace(org, ctx.allocator);

    const layout = engine.identity.workspace.loadLayout(ctx.allocator, ctx.environ, org.id) catch |e|
        exit.die(ctx, e, "loading workspace layout failed: {s}", .{@errorName(e)});
    defer engine.identity.workspace.deinitLayout(layout, ctx.allocator);

    const raw = std.Io.Dir.cwd().readFileAlloc(fsIo(), layout.routing_table, ctx.allocator, std.Io.Limit.limited(16 * 1024 * 1024)) catch |e| switch (e) {
        error.FileNotFound => exit.die(ctx, error.NotFound, "routing table not found at {s}; run `planar workspace routing build` first", .{layout.routing_table}),
        else => exit.die(ctx, e, "reading routing table failed: {s}", .{@errorName(e)}),
    };
    defer ctx.allocator.free(raw);

    if (args.json) {
        try ctx.stdout.print("{s}", .{raw});
        if (raw.len == 0 or raw[raw.len - 1] != '\n') try ctx.stdout.print("\n", .{});
        return;
    }

    const table = engine.workspace.routing.read(layout.routing_table, ctx.allocator) catch |e|
        exit.die(ctx, e, "decoding routing table failed: {s}", .{@errorName(e)});
    defer engine.workspace.routing.deinit(table, ctx.allocator);

    try ctx.stdout.print("workspace: org:{s} (id {d})\n", .{ table.workspace_slug, table.workspace_id });
    try ctx.stdout.print("generated: {s} ({s})\n", .{ table.generated_at, table.generator_version });
    try ctx.stdout.print("projects:  {d}\n\n", .{table.projects.len});
    for (table.projects) |project| {
        try ctx.stdout.print("- {s}\n", .{project.slug});
        try ctx.stdout.print("    path:         {s}\n", .{project.root_path});
        if (project.capabilities.len == 0) {
            try ctx.stdout.print("    capabilities: (none)\n", .{});
        } else {
            try ctx.stdout.print("    capabilities: ", .{});
            for (project.capabilities, 0..) |cap, i| {
                if (i > 0) try ctx.stdout.print(", ", .{});
                try ctx.stdout.print("{s}", .{cap});
            }
            try ctx.stdout.print("\n", .{});
        }
        const summary = if (project.summary.len > 0) project.summary else "(no summary)";
        try ctx.stdout.print("    summary:      {s}\n", .{summary});
        if (project.depends_on.len > 0) {
            try ctx.stdout.print("    depends_on:   ", .{});
            for (project.depends_on, 0..) |dep, i| {
                if (i > 0) try ctx.stdout.print(", ", .{});
                try ctx.stdout.print("{s}", .{dep});
            }
            try ctx.stdout.print("\n", .{});
        }
        try ctx.stdout.print("    open tasks:   {d}\n", .{project.planar_focus.open_tasks});
        try ctx.stdout.print("    open Qs:      {d}\n", .{project.planar_focus.open_questions});
    }

    if (table.cross_repo.dependency_edges.len > 0) {
        try ctx.stdout.print("\ncross-repo edges:\n", .{});
        for (table.cross_repo.dependency_edges) |edge| {
            try ctx.stdout.print("  {s} -> {s} ({s})\n", .{ edge.from, edge.to, edge.reason });
        }
    }
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}
