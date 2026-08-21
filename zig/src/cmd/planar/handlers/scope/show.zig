//! handlers/scope/show — `planar scope show [--scope]`
//!
//! Prints the scope Planar resolves for the current cwd, optionally
//! overridden by `--scope <slug>`. Mirrors the cwd-derived view from
//! Go's `cmd/planar/internal/identity/scope.go` (post plan 153 M5).

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const scope_mod = @import("../../scope.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "scope", "show" }, args_ptr);
    const ctx = runtime.current();

    const cwd = scope_mod.operatorCwd(ctx.allocator, ctx.io) catch |e| {
        try ctx.stderr.print("error: getting cwd: {s}\n", .{@errorName(e)});
        return e;
    };
    defer ctx.allocator.free(cwd);

    const resolution = scope_mod.resolve(ctx, args.scope) catch |e| {
        try ctx.stderr.print("error: resolving scope: {s}\n", .{@errorName(e)});
        return e;
    };
    defer if (args.scope == null) {
        if (resolution.scope) |s| ctx.allocator.free(s);
        if (resolution.project_slug) |s| ctx.allocator.free(s);
        if (resolution.worktree_root) |s| ctx.allocator.free(s);
        if (resolution.parent_repo_root) |s| ctx.allocator.free(s);
    };

    if (args.json) {
        try renderJson(ctx, cwd, args.scope, resolution);
    } else {
        try renderText(ctx, args.scope, resolution, cwd);
    }
}

fn renderText(
    ctx: *const runtime.Ctx,
    override: ?[]const u8,
    res: scope_mod.Resolution,
    cwd: []const u8,
) !void {
    const resolved = try scope_mod.resolveForReadSet(ctx, cwd, override);
    defer ctx.allocator.free(resolved);
    if (resolved.len > 0) {
        if (override != null) {
            try ctx.stdout.print("resolved scope (from --scope flag):\n", .{});
        } else {
            try ctx.stdout.print("resolved scope (from cwd):\n", .{});
        }
        try printResolvedTextRows(ctx, resolved);
        try printTrailer(ctx);
        return;
    }

    if (override) |slug| {
        try ctx.stdout.print("resolved scope (from --scope flag):\n", .{});
        try ctx.stdout.print("  {s}\n", .{slug});
        try printTrailer(ctx);
        return;
    }

    if (res.scope) |slug| {
        try ctx.stdout.print("resolved scope (from cwd):\n", .{});
        try ctx.stdout.print("  {s}\n", .{slug});
        try printTrailer(ctx);
        return;
    }

    switch (res.reason) {
        .no_project_match => {
            try ctx.stdout.print(
                "resolved scope: none (cwd not inside any registered Planar scope)\n",
                .{},
            );
            try ctx.stdout.print("\n", .{});
            try ctx.stdout.print(
                "cd into a registered scope or pass --scope <slug> to any verb.\n",
                .{},
            );
            try printTrailer(ctx);
        },
        .project_unassociated => {
            try ctx.stdout.print(
                "resolved scope: project '{s}' has no association memberships (treated as global).\n",
                .{res.project_slug orelse "?"},
            );
            try ctx.stdout.print("\n", .{});
            try ctx.stdout.print(
                "Tag the project with `planar association add <slug> .` or run `planar scope suggest`.\n",
                .{},
            );
            try printTrailer(ctx);
        },
        .project_multiple_associations => {
            try ctx.stdout.print(
                "resolved scope: ambiguous — project '{s}' belongs to multiple associations.\n",
                .{res.project_slug orelse "?"},
            );
            try ctx.stdout.print("\n", .{});
            try ctx.stdout.print(
                "Pass --scope <slug> to any verb to pick one explicitly.\n",
                .{},
            );
            try printTrailer(ctx);
        },
        .project_single_association => unreachable, // handled above (res.scope != null)
    }
}

fn printTrailer(ctx: *const runtime.Ctx) !void {
    try ctx.stdout.print("\n", .{});
    try ctx.stdout.print(
        "(The active scope stack was removed in plan 153 M5; scope is now derived from your current working directory.)\n",
        .{},
    );
}

fn renderJson(
    ctx: *const runtime.Ctx,
    cwd: []const u8,
    override: ?[]const u8,
    res: scope_mod.Resolution,
) !void {
    const d = try runtime.ensureDb();
    const resolved = try scope_mod.resolveForReadSet(ctx, cwd, override);
    defer ctx.allocator.free(resolved);
    try ctx.stdout.print("{{\"resolved_scopes\":[", .{});
    for (resolved, 0..) |entry, i| {
        if (i > 0) try ctx.stdout.print(",", .{});
        try writeResolvedScopeJSON(ctx, d, entry);
    }

    _ = res;
    const source = if (override != null) "flag" else if (resolved.len > 0) "cwd" else "none";
    try ctx.stdout.print("],\"source\":\"{s}\",\"cwd\":\"{s}\"}}\n", .{ source, cwd });
}

fn writeResolvedScopeJSON(
    ctx: *const runtime.Ctx,
    d: anytype,
    resolved: scope_mod.ReadScope,
) !void {
    switch (resolved.kind) {
        .global => {
            try ctx.stdout.print(
                "{{\"kind\":\"global\",\"id\":0,\"slug\":\"\",\"name\":\"\",\"kind_label\":\"global\"}}",
                .{},
            );
        },
        .association => {
            const assoc_id = resolved.id;
            var slug: []const u8 = "";
            var name: []const u8 = "";
            var kind_label: []const u8 = "";
            {
                var stmt = try d.prepare("select coalesce(slug,''), coalesce(name,''), coalesce(kind,'') from associations where id = ?");
                defer stmt.finalize();
                try stmt.bind(&.{.{ .int = assoc_id }});
                if ((try stmt.step()) == .row) {
                    slug = try stmt.columnTextAlloc(0, ctx.allocator);
                    name = try stmt.columnTextAlloc(1, ctx.allocator);
                    kind_label = try stmt.columnTextAlloc(2, ctx.allocator);
                }
            }
            defer {
                if (slug.len > 0) ctx.allocator.free(slug);
                if (name.len > 0) ctx.allocator.free(name);
                if (kind_label.len > 0) ctx.allocator.free(kind_label);
            }
            try ctx.stdout.print(
                "{{\"kind\":\"association\",\"id\":{d},\"slug\":\"{s}\",\"name\":\"{s}\",\"kind_label\":\"{s}\"}}",
                .{ assoc_id, slug, name, kind_label },
            );
        },
        .repo => {
            const project_id = resolved.id;
            var slug: []const u8 = "";
            {
                var stmt = try d.prepare("select coalesce(slug,'') from projects where id = ?");
                defer stmt.finalize();
                try stmt.bind(&.{.{ .int = project_id }});
                if ((try stmt.step()) == .row) {
                    slug = try stmt.columnTextAlloc(0, ctx.allocator);
                }
            }
            defer if (slug.len > 0) ctx.allocator.free(slug);
            try ctx.stdout.print(
                "{{\"kind\":\"repo\",\"id\":{d},\"slug\":\"{s}\",\"name\":\"\",\"kind_label\":\"repo\"}}",
                .{ project_id, slug },
            );
        },
    }
}

fn printResolvedTextRows(ctx: *const runtime.Ctx, rows: []const scope_mod.ReadScope) !void {
    const d = try runtime.ensureDb();
    for (rows) |row| switch (row.kind) {
        .global => try ctx.stdout.print("  global\n", .{}),
        .repo => {
            var slug: []const u8 = "";
            {
                var stmt = try d.prepare("select coalesce(slug,'') from projects where id = ?");
                defer stmt.finalize();
                try stmt.bind(&.{.{ .int = row.id }});
                if ((try stmt.step()) == .row) slug = try stmt.columnTextAlloc(0, ctx.allocator);
            }
            defer if (slug.len > 0) ctx.allocator.free(slug);
            if (slug.len > 0) {
                try ctx.stdout.print("  repo:{s}\n", .{slug});
            } else {
                try ctx.stdout.print("  repo:{d}\n", .{row.id});
            }
        },
        .association => {
            var slug: []const u8 = "";
            var kind_label: []const u8 = "";
            {
                var stmt = try d.prepare("select coalesce(slug,''), coalesce(kind,'') from associations where id = ?");
                defer stmt.finalize();
                try stmt.bind(&.{.{ .int = row.id }});
                if ((try stmt.step()) == .row) {
                    slug = try stmt.columnTextAlloc(0, ctx.allocator);
                    kind_label = try stmt.columnTextAlloc(1, ctx.allocator);
                }
            }
            defer {
                if (slug.len > 0) ctx.allocator.free(slug);
                if (kind_label.len > 0) ctx.allocator.free(kind_label);
            }
            const label = if (kind_label.len > 0) kind_label else "assoc";
            if (slug.len > 0) {
                const pref = try std.fmt.allocPrint(ctx.allocator, "{s}:", .{label});
                defer ctx.allocator.free(pref);
                if (std.mem.startsWith(u8, slug, pref)) {
                    try ctx.stdout.print("  {s}\n", .{slug});
                } else {
                    try ctx.stdout.print("  {s}:{s}\n", .{ label, slug });
                }
            } else {
                try ctx.stdout.print("  {s}:{d}\n", .{ label, row.id });
            }
        },
    };
}
