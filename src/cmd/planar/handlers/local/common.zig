const std = @import("std");
const engine = @import("engine");
const runtime = @import("../../runtime.zig");

pub const HomeAndRoot = struct {
    home_dir: []const u8,
    sandbox_root: []const u8,
};

pub fn resolveHomeAndRoot(ctx: *const runtime.Ctx) !HomeAndRoot {
    const home = if (ctx.environ.getPosix("PLANAR_LOCAL_HOME")) |v| v else (ctx.environ.getPosix("HOME") orelse return error.HomeNotSet);
    return .{
        .home_dir = try ctx.allocator.dupe(u8, home),
        .sandbox_root = try std.fs.path.join(ctx.allocator, &.{ home, ".planar", "local" }),
    };
}

pub fn lookupKindForName(sandbox_root: []const u8, name: []const u8, allocator: std.mem.Allocator) !?engine.local.manifest.Kind {
    const skill_path = try std.fs.path.join(allocator, &.{ sandbox_root, "skills", name, "SKILL.md" });
    defer allocator.free(skill_path);
    if (pathExists(skill_path)) return .skill;

    const agent_name = try std.fmt.allocPrint(allocator, "{s}.md", .{name});
    defer allocator.free(agent_name);
    const agent_path = try std.fs.path.join(allocator, &.{ sandbox_root, "agents", agent_name });
    defer allocator.free(agent_path);
    if (pathExists(agent_path)) return .agent;

    return null;
}

pub fn parseKind(kind_flag: ?[]const u8) !engine.local.manifest.Kind {
    const raw = kind_flag orelse "skill";
    if (std.mem.eql(u8, raw, "skill") or std.mem.eql(u8, raw, "skills")) return .skill;
    if (std.mem.eql(u8, raw, "agent") or std.mem.eql(u8, raw, "agents")) return .agent;
    return error.InvalidInput;
}

pub fn emitGoLinkJSON(
    ctx: *const runtime.Ctx,
    file: engine.local.manifest.SandboxFile,
    linked: engine.local.link.LinkResult,
) !void {
    const GoRecord = struct {
        vendor: []const u8,
        target_path: []const u8,
        source_path: []const u8,
        mode: []const u8,
        action: []const u8,
        warning: ?[]const u8,
        linked_at: ?[]const u8,
    };
    var recs: std.ArrayList(GoRecord) = .empty;
    defer recs.deinit(ctx.allocator);
    for (linked.records) |rec| {
        try recs.append(ctx.allocator, .{
            .vendor = rec.vendor,
            .target_path = rec.target_path,
            .source_path = rec.source_path,
            .mode = if (rec.mode) |m| @tagName(m) else "",
            .action = rec.action,
            .warning = if (rec.warning.len > 0) rec.warning else null,
            .linked_at = if (rec.linked_at.len > 0) rec.linked_at else null,
        });
    }

    const payload = .{
        .Source = .{
            .SourcePath = file.source_path,
            .Name = file.name,
            .Kind = @tagName(file.kind),
            .Frontmatter = .{
                .Description = file.frontmatter.description,
                .ArgumentHint = file.frontmatter.argument_hint,
                .Tier = file.frontmatter.tier,
                .Model = file.frontmatter.model,
                .Shadow = file.frontmatter.shadow,
                .Vendors = file.frontmatter.vendors,
                .Kind = file.frontmatter.kind,
            },
            .Body = file.body,
        },
        .Records = recs.items,
    };
    try std.json.Stringify.value(payload, .{ .emit_null_optional_fields = false }, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(fsIo(), path, .{}) catch return false;
    return true;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}
