const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workspace", "doctor" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const orgs = engine.identity.workspace.listOrgs(d, ctx.allocator) catch |e|
        exit.die(ctx, e, "listing org associations failed: {s}", .{@errorName(e)});
    defer engine.identity.workspace.deinitMany(orgs, ctx.allocator);

    var reports = std.ArrayList(OrgReport).empty;
    defer {
        for (reports.items) |report| deinitReport(report, ctx.allocator);
        reports.deinit(ctx.allocator);
    }

    for (orgs) |org| try reports.append(ctx.allocator, try diagnoseOrg(ctx, org));

    if (args.json) {
        try std.json.Stringify.value(.{ .orgs = reports.items }, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }

    for (reports.items) |report| {
        for (report.issues_repaired) |item| {
            try ctx.stdout.print("{s}: {s}\n", .{ item.kind, item.detail });
        }
        if (report.issues_found == 0) {
            try ctx.stdout.print("org:{s} ok\n", .{report.slug});
        } else {
            try ctx.stdout.print("org:{s} repaired {d} issues\n", .{ report.slug, report.issues_found });
        }
    }
}

const Issue = struct {
    kind: []const u8,
    detail: []const u8,
};

const OrgReport = struct {
    slug: []const u8,
    org_id: i64,
    issues_found: i64,
    issues_repaired: []const Issue,
};

fn deinitReport(report: OrgReport, allocator: std.mem.Allocator) void {
    allocator.free(report.slug);
    for (report.issues_repaired) |item| {
        allocator.free(item.kind);
        allocator.free(item.detail);
    }
    allocator.free(report.issues_repaired);
}

fn diagnoseOrg(ctx: *const runtime.Ctx, org: engine.identity.workspace.Workspace) !OrgReport {
    var issues = std.ArrayList(Issue).empty;
    errdefer {
        for (issues.items) |issue| {
            ctx.allocator.free(issue.kind);
            ctx.allocator.free(issue.detail);
        }
        issues.deinit(ctx.allocator);
    }

    const layout = engine.identity.workspace.loadLayout(ctx.allocator, ctx.environ, org.id) catch |e| {
        try issues.append(ctx.allocator, .{
            .kind = try ctx.allocator.dupe(u8, "error"),
            .detail = try std.fmt.allocPrint(ctx.allocator, "computing layout for org {d}: {s}", .{ org.id, @errorName(e) }),
        });
        return .{
            .slug = try ctx.allocator.dupe(u8, org.slug),
            .org_id = org.id,
            .issues_found = @intCast(issues.items.len),
            .issues_repaired = try issues.toOwnedSlice(ctx.allocator),
        };
    };
    defer engine.identity.workspace.deinitLayout(layout, ctx.allocator);

    if (!pathExists(layout.dir)) {
        _ = engine.identity.workspace.ensureLayout(ctx.allocator, ctx.io, ctx.environ, org.id) catch |e| {
            try issues.append(ctx.allocator, .{
                .kind = try ctx.allocator.dupe(u8, "error"),
                .detail = try std.fmt.allocPrint(ctx.allocator, "creating state dir {s}: {s}", .{ layout.dir, @errorName(e) }),
            });
            return .{
                .slug = try ctx.allocator.dupe(u8, org.slug),
                .org_id = org.id,
                .issues_found = @intCast(issues.items.len),
                .issues_repaired = try issues.toOwnedSlice(ctx.allocator),
            };
        };
        try issues.append(ctx.allocator, .{
            .kind = try ctx.allocator.dupe(u8, "fix"),
            .detail = try std.fmt.allocPrint(ctx.allocator, "created state dir {s}", .{layout.dir}),
        });
    }

    if (!pathExists(layout.agents_md)) {
        try issues.append(ctx.allocator, .{
            .kind = try ctx.allocator.dupe(u8, "missing"),
            .detail = try std.fmt.allocPrint(ctx.allocator, "{s} (run `planar workspace regenerate` after M3)", .{layout.agents_md}),
        });
    }
    if (!pathExists(layout.routing_table)) {
        try issues.append(ctx.allocator, .{
            .kind = try ctx.allocator.dupe(u8, "missing"),
            .detail = try std.fmt.allocPrint(ctx.allocator, "{s} (run `planar workspace regenerate` after M3)", .{layout.routing_table}),
        });
    }

    if (org.root_path == null or org.root_path.?.len == 0) {
        try issues.append(ctx.allocator, .{
            .kind = try ctx.allocator.dupe(u8, "warn"),
            .detail = try std.fmt.allocPrint(ctx.allocator, "org:{s} has no root_path in config_json; skipping symlink repair", .{org.slug}),
        });
    } else {
        const dirty = try dirtyLinks(ctx.allocator, org.root_path.?, layout.agents_md);
        defer freeStrings(dirty, ctx.allocator);
        if (dirty.len > 0) {
            const strategy = engine.identity.workspace.installSymlinks(ctx.allocator, ctx.io, org.root_path.?, layout) catch |e| {
                try issues.append(ctx.allocator, .{
                    .kind = try ctx.allocator.dupe(u8, "error"),
                    .detail = try std.fmt.allocPrint(ctx.allocator, "installing symlinks at {s}: {s}", .{ org.root_path.?, @errorName(e) }),
                });
                return .{
                    .slug = try ctx.allocator.dupe(u8, org.slug),
                    .org_id = org.id,
                    .issues_found = @intCast(issues.items.len),
                    .issues_repaired = try issues.toOwnedSlice(ctx.allocator),
                };
            };
            for (dirty) |name| {
                try issues.append(ctx.allocator, .{
                    .kind = try ctx.allocator.dupe(u8, "fix"),
                    .detail = try std.fmt.allocPrint(ctx.allocator, "reinstalled {s} {s}/{s} → {s}", .{
                        strategy,
                        org.root_path.?,
                        name,
                        layout.agents_md,
                    }),
                });
            }
        }
    }

    return .{
        .slug = try ctx.allocator.dupe(u8, org.slug),
        .org_id = org.id,
        .issues_found = @intCast(issues.items.len),
        .issues_repaired = try issues.toOwnedSlice(ctx.allocator),
    };
}

fn dirtyLinks(
    allocator: std.mem.Allocator,
    workspace_root: []const u8,
    target: []const u8,
) ![]const []const u8 {
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        freeStrings(out.items, allocator);
        out.deinit(allocator);
    }
    for ([_][]const u8{ "AGENTS.md", "CLAUDE.md" }) |name| {
        const path = try std.fs.path.join(allocator, &.{ workspace_root, name });
        defer allocator.free(path);
        var buf: [std.fs.max_path_bytes]u8 = undefined;
        if (std.Io.Dir.cwd().readLink(ctxIo(), path, &buf)) |n| {
            if (!std.mem.eql(u8, buf[0..n], target)) try out.append(allocator, try allocator.dupe(u8, name));
            continue;
        } else |_| {
            if (pathExists(path)) {
                try out.append(allocator, try allocator.dupe(u8, name));
            } else {
                try out.append(allocator, try allocator.dupe(u8, name));
            }
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn freeStrings(items: []const []const u8, allocator: std.mem.Allocator) void {
    for (items) |item| allocator.free(item);
    allocator.free(items);
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(ctxIo(), path, .{}) catch return false;
    return true;
}

fn ctxIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}
