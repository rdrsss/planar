//! handlers/scope/show — `planar scope show [--scope]`
//!
//! Prints the scope Planar resolves for the current cwd, optionally
//! overridden by `--scope <slug>`. Mirrors the cwd-derived view from
//! Go's `cmd/planar/internal/identity/scope.go` (post plan 153 M5).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const scope_mod = @import("../../scope.zig");

const ResolvedScope = struct {
    kind: engine.identity.scope.ScopeKind,
    id: i64 = 0,
};

const Candidate = struct {
    id: i64,
    kind: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "scope", "show" }, args_ptr);
    const ctx = runtime.current();

    const cwd = std.Io.Dir.realPathFileAlloc(.cwd(), ctx.io, ".", ctx.allocator) catch |e| {
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
    const resolved = try resolveForReadSet(ctx, cwd, override);
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
    const resolved = try resolveForReadSet(ctx, cwd, override);
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
    resolved: ResolvedScope,
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

fn resolveForReadSet(
    ctx: *const runtime.Ctx,
    cwd: []const u8,
    override: ?[]const u8,
) ![]ResolvedScope {
    const d = try runtime.ensureDb();
    if (override) |raw_scope| {
        const ref = engine.identity.scope.resolveSlug(d, ctx.allocator, raw_scope) catch |e| switch (e) {
            error.SlugNotFound => {
                try ctx.stderr.print("error: resolving scope: scope slug not found: {s}\n", .{raw_scope});
                return e;
            },
            else => return e,
        };
        const out = try ctx.allocator.alloc(ResolvedScope, 1);
        out[0] = .{ .kind = ref.kind, .id = ref.id orelse 0 };
        return out;
    }

    const candidates = try deriveCandidates(ctx, d, cwd);
    defer {
        for (candidates) |c| ctx.allocator.free(c.kind);
        ctx.allocator.free(candidates);
    }
    if (candidates.len == 0) return try ctx.allocator.alloc(ResolvedScope, 0);

    var best_rank = specificityRank(candidates[0].kind);
    for (candidates[1..]) |c| {
        const r = specificityRank(c.kind);
        if (r < best_rank) best_rank = r;
    }
    var top_count: usize = 0;
    var winner: Candidate = candidates[0];
    for (candidates) |c| {
        if (specificityRank(c.kind) == best_rank) {
            top_count += 1;
            winner = c;
        }
    }
    if (top_count != 1) return try ctx.allocator.alloc(ResolvedScope, 0);

    if (std.mem.eql(u8, winner.kind, "org")) {
        const members = try expandWorkspaceReadSet(ctx, d, winner.id);
        return members;
    }
    const out = try ctx.allocator.alloc(ResolvedScope, 1);
    out[0] = .{ .kind = .association, .id = winner.id };
    return out;
}

fn expandWorkspaceReadSet(
    ctx: *const runtime.Ctx,
    d: anytype,
    org_assoc_id: i64,
) ![]ResolvedScope {
    var out: std.ArrayList(ResolvedScope) = .empty;
    try out.append(ctx.allocator, .{ .kind = .association, .id = org_assoc_id });

    var project_ids: std.ArrayList(i64) = .empty;
    defer project_ids.deinit(ctx.allocator);
    {
        var stmt = try d.prepare(
            \\select project_id from project_associations
            \\where association_id = ?
            \\order by project_id
        );
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = org_assoc_id }});
        while (true) switch (try stmt.step()) {
            .done => break,
            .row => try project_ids.append(ctx.allocator, stmt.columnInt(0)),
        };
    }

    {
        var stmt = try d.prepare(
            \\select distinct a.id
            \\from associations a
            \\join project_associations pa on pa.association_id = a.id
            \\where a.kind = 'project'
            \\  and a.id != ?
            \\  and pa.project_id in (
            \\    select project_id from project_associations where association_id = ?
            \\  )
            \\order by a.id
        );
        defer stmt.finalize();
        try stmt.bind(&.{ .{ .int = org_assoc_id }, .{ .int = org_assoc_id } });
        while (true) switch (try stmt.step()) {
            .done => break,
            .row => try out.append(ctx.allocator, .{ .kind = .association, .id = stmt.columnInt(0) }),
        };
    }
    for (project_ids.items) |pid| try out.append(ctx.allocator, .{ .kind = .repo, .id = pid });
    return try out.toOwnedSlice(ctx.allocator);
}

fn deriveCandidates(
    ctx: *const runtime.Ctx,
    d: anytype,
    cwd: []const u8,
) ![]Candidate {
    var out: std.ArrayList(Candidate) = .empty;
    errdefer {
        for (out.items) |c| ctx.allocator.free(c.kind);
        out.deinit(ctx.allocator);
    }

    {
        var stmt = try d.prepare(
            \\select a.id, a.kind, p.root_path
            \\from associations a
            \\join project_associations pa on pa.association_id = a.id
            \\join projects p on p.id = pa.project_id
            \\where p.root_path is not null
            \\order by a.id, p.root_path
        );
        defer stmt.finalize();
        try stmt.bind(&.{});
        while (true) switch (try stmt.step()) {
            .done => break,
            .row => {
                const assoc_id = stmt.columnInt(0);
                const kind = try stmt.columnTextAlloc(1, ctx.allocator);
                errdefer ctx.allocator.free(kind);
                const root = try stmt.columnTextAlloc(2, ctx.allocator);
                defer ctx.allocator.free(root);
                if (pathHasPrefix(cwd, root)) {
                    try out.append(ctx.allocator, .{
                        .id = assoc_id,
                        .kind = kind,
                    });
                } else {
                    ctx.allocator.free(kind);
                }
            },
        };
    }

    {
        var stmt = try d.prepare(
            \\select id, kind, coalesce(config_json,'')
            \\from associations
            \\where kind = 'org' and config_json is not null and config_json != ''
            \\order by id
        );
        defer stmt.finalize();
        try stmt.bind(&.{});
        while (true) switch (try stmt.step()) {
            .done => break,
            .row => {
                const assoc_id = stmt.columnInt(0);
                const kind = try stmt.columnTextAlloc(1, ctx.allocator);
                errdefer ctx.allocator.free(kind);
                const cfg = try stmt.columnTextAlloc(2, ctx.allocator);
                defer ctx.allocator.free(cfg);
                const root = try rootPathFromConfigJSON(ctx.allocator, cfg) orelse {
                    ctx.allocator.free(kind);
                    continue;
                };
                defer ctx.allocator.free(root);
                if (pathHasPrefix(cwd, root)) {
                    try out.append(ctx.allocator, .{
                        .id = assoc_id,
                        .kind = kind,
                    });
                } else {
                    ctx.allocator.free(kind);
                }
            },
        };
    }

    return try out.toOwnedSlice(ctx.allocator);
}

fn rootPathFromConfigJSON(allocator: std.mem.Allocator, config_json: []const u8) !?[]const u8 {
    if (config_json.len == 0) return null;
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, config_json, .{}) catch return null;
    defer parsed.deinit();
    if (parsed.value != .object) return null;
    const value = parsed.value.object.get("root_path") orelse return null;
    if (value != .string) return null;
    if (value.string.len == 0) return null;
    return try allocator.dupe(u8, value.string);
}

fn pathHasPrefix(target: []const u8, root: []const u8) bool {
    if (std.mem.eql(u8, target, root)) return true;
    if (!std.mem.startsWith(u8, target, root)) return false;
    if (target.len <= root.len) return false;
    return target[root.len] == '/';
}

fn specificityRank(kind: []const u8) u8 {
    if (std.mem.eql(u8, kind, "project")) return 1;
    if (std.mem.eql(u8, kind, "ad-hoc")) return 2;
    if (std.mem.eql(u8, kind, "personal")) return 2;
    if (std.mem.eql(u8, kind, "client")) return 3;
    if (std.mem.eql(u8, kind, "org")) return 4;
    return 5;
}

fn printResolvedTextRows(ctx: *const runtime.Ctx, rows: []const ResolvedScope) !void {
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
