//! engine/local/manifest — sandbox walker + link-manifest IO + migrate.

const std = @import("std");
const operatorpath = @import("../operatorpath.zig");

pub const Kind = enum { skill, agent };

pub const all_vendors = [_][]const u8{ "claude", "codex", "copilot" };
pub const manifest_filename = ".link-manifest.json";

pub const Frontmatter = struct {
    description: []const u8,
    argument_hint: []const u8,
    tier: []const u8,
    model: []const u8,
    shadow: bool,
    vendors: []const []const u8,
    kind: []const u8,
};

pub fn deinitFrontmatter(fm: Frontmatter, allocator: std.mem.Allocator) void {
    allocator.free(fm.description);
    allocator.free(fm.argument_hint);
    allocator.free(fm.tier);
    allocator.free(fm.model);
    allocator.free(fm.kind);
    for (fm.vendors) |v| allocator.free(v);
    allocator.free(fm.vendors);
}

pub fn resolvedVendors(fm: Frontmatter, allocator: std.mem.Allocator) ![]const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (out.items) |v| allocator.free(v);
        out.deinit(allocator);
    }
    if (fm.vendors.len == 0) {
        for (all_vendors) |v| try out.append(allocator, try allocator.dupe(u8, v));
    } else {
        for (fm.vendors) |v| try out.append(allocator, try allocator.dupe(u8, v));
    }
    std.mem.sort([]const u8, out.items, {}, struct {
        fn lt(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.order(u8, a, b) == .lt;
        }
    }.lt);
    return try out.toOwnedSlice(allocator);
}

pub fn lint(fm: Frontmatter, name: []const u8, allocator: std.mem.Allocator) ![]const LintIssue {
    var out: std.ArrayList(LintIssue) = .empty;
    errdefer {
        for (out.items) |issue| {
            allocator.free(issue.field);
            allocator.free(issue.message);
        }
        out.deinit(allocator);
    }
    if (fm.description.len == 0) {
        try out.append(allocator, .{
            .severity = .warning,
            .field = try allocator.dupe(u8, "description"),
            .message = try allocator.dupe(u8, "description is empty; vendors surface this as the skill summary"),
        });
    }
    if (fm.shadow) {
        const msg = try std.fmt.allocPrint(
            allocator,
            "shadow:true — install will land as \"{s}.md\" (no local- prefix) and may replace a canonical install of the same name",
            .{name},
        );
        errdefer allocator.free(msg);
        try out.append(allocator, .{
            .severity = .warning,
            .field = try allocator.dupe(u8, "shadow"),
            .message = msg,
        });
    }
    return try out.toOwnedSlice(allocator);
}

pub const LintSeverity = enum { warning, @"error" };
pub const LintIssue = struct {
    severity: LintSeverity,
    field: []const u8,
    message: []const u8,
};

pub fn deinitLint(issues: []const LintIssue, allocator: std.mem.Allocator) void {
    for (issues) |issue| {
        allocator.free(issue.field);
        allocator.free(issue.message);
    }
    allocator.free(issues);
}

pub const SandboxFile = struct {
    source_path: []const u8,
    name: []const u8,
    kind: Kind,
    frontmatter: Frontmatter,
    body: []const u8,
};

pub fn deinitSandboxFile(file: SandboxFile, allocator: std.mem.Allocator) void {
    allocator.free(file.source_path);
    allocator.free(file.name);
    deinitFrontmatter(file.frontmatter, allocator);
    allocator.free(file.body);
}

pub fn deinitSandboxFiles(files: []const SandboxFile, allocator: std.mem.Allocator) void {
    for (files) |file| deinitSandboxFile(file, allocator);
    allocator.free(files);
}

pub const WalkError = struct {
    path: []const u8,
    message: []const u8,
};

pub fn deinitWalkErrors(errs: []const WalkError, allocator: std.mem.Allocator) void {
    for (errs) |e| {
        allocator.free(e.path);
        allocator.free(e.message);
    }
    allocator.free(errs);
}

pub const WalkResult = struct {
    files: []const SandboxFile,
    walk_errors: []const WalkError,
};

pub fn deinitWalkResult(result: WalkResult, allocator: std.mem.Allocator) void {
    deinitSandboxFiles(result.files, allocator);
    deinitWalkErrors(result.walk_errors, allocator);
}

pub fn walkSandbox(root: []const u8, allocator: std.mem.Allocator) !WalkResult {
    if (root.len == 0) return error.InvalidInput;

    var files: std.ArrayList(SandboxFile) = .empty;
    errdefer {
        for (files.items) |file| deinitSandboxFile(file, allocator);
        files.deinit(allocator);
    }
    var errs: std.ArrayList(WalkError) = .empty;
    errdefer {
        for (errs.items) |e| {
            allocator.free(e.path);
            allocator.free(e.message);
        }
        errs.deinit(allocator);
    }

    const skills_dir = try std.fs.path.join(allocator, &.{ root, "skills" });
    defer allocator.free(skills_dir);
    try walkSkillsDir(skills_dir, allocator, &files, &errs);

    const agents_dir = try std.fs.path.join(allocator, &.{ root, "agents" });
    defer allocator.free(agents_dir);
    try walkAgentsDir(agents_dir, allocator, &files, &errs);

    std.mem.sort(SandboxFile, files.items, {}, struct {
        fn lt(_: void, a: SandboxFile, b: SandboxFile) bool {
            const kind_ord = @intFromEnum(a.kind) < @intFromEnum(b.kind);
            if (a.kind != b.kind) return kind_ord;
            return std.mem.order(u8, a.name, b.name) == .lt;
        }
    }.lt);

    return .{
        .files = try files.toOwnedSlice(allocator),
        .walk_errors = try errs.toOwnedSlice(allocator),
    };
}

pub fn parseFile(path: []const u8, kind: Kind, allocator: std.mem.Allocator) !SandboxFile {
    const raw = try readFile(path, allocator, 1024 * 1024);
    defer allocator.free(raw);

    const split = try splitFrontmatter(raw, allocator);
    errdefer {
        deinitFrontmatter(split.frontmatter, allocator);
        allocator.free(split.body);
    }
    try validateFrontmatter(split.frontmatter, kind);

    const source_path = try toAbsolutePath(path, allocator);
    errdefer allocator.free(source_path);
    const base = std.fs.path.basename(source_path);

    const name = blk: {
        if (std.mem.eql(u8, base, "SKILL.md")) {
            const parent = std.fs.path.dirname(source_path) orelse return error.InvalidInput;
            break :blk try allocator.dupe(u8, std.fs.path.basename(parent));
        }
        if (!std.mem.endsWith(u8, base, ".md")) return error.InvalidInput;
        break :blk try allocator.dupe(u8, base[0 .. base.len - 3]);
    };
    errdefer allocator.free(name);

    return .{
        .source_path = source_path,
        .name = name,
        .kind = kind,
        .frontmatter = split.frontmatter,
        .body = split.body,
    };
}

pub const MigrateOpts = struct {
    sandbox_root: []const u8,
    dry_run: bool = false,
};

pub const MigrateRecord = struct {
    name: []const u8,
    old_path: []const u8,
    new_path: []const u8,
    reason: []const u8 = "",
};

pub const MigrateResult = struct {
    migrated: []const MigrateRecord,
    skipped: []const MigrateRecord,
};

pub fn deinitMigrateResult(result: MigrateResult, allocator: std.mem.Allocator) void {
    for (result.migrated) |r| {
        allocator.free(r.name);
        allocator.free(r.old_path);
        allocator.free(r.new_path);
        if (r.reason.len > 0) allocator.free(r.reason);
    }
    allocator.free(result.migrated);
    for (result.skipped) |r| {
        allocator.free(r.name);
        allocator.free(r.old_path);
        allocator.free(r.new_path);
        if (r.reason.len > 0) allocator.free(r.reason);
    }
    allocator.free(result.skipped);
}

pub fn migrate(opts: MigrateOpts, allocator: std.mem.Allocator) !MigrateResult {
    if (opts.sandbox_root.len == 0) return error.InvalidInput;

    const skills_dir = try std.fs.path.join(allocator, &.{ opts.sandbox_root, "skills" });
    defer allocator.free(skills_dir);
    var dir = openDirPath(skills_dir, .{ .iterate = true }) catch |e| switch (e) {
        error.FileNotFound => return .{ .migrated = try allocator.alloc(MigrateRecord, 0), .skipped = try allocator.alloc(MigrateRecord, 0) },
        else => return e,
    };
    defer dir.close(fsIo());

    var migrated: std.ArrayList(MigrateRecord) = .empty;
    errdefer {
        for (migrated.items) |r| freeMigrateRecord(r, allocator);
        migrated.deinit(allocator);
    }
    var skipped: std.ArrayList(MigrateRecord) = .empty;
    errdefer {
        for (skipped.items) |r| freeMigrateRecord(r, allocator);
        skipped.deinit(allocator);
    }

    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        if (entry.kind != .file) continue;
        if (entry.name.len == 0 or entry.name[0] == '.') continue;
        if (!std.mem.endsWith(u8, entry.name, ".md")) continue;

        const stem = entry.name[0 .. entry.name.len - 3];
        const old_path = try std.fs.path.join(allocator, &.{ skills_dir, entry.name });
        errdefer allocator.free(old_path);
        const new_dir = try std.fs.path.join(allocator, &.{ skills_dir, stem });
        defer allocator.free(new_dir);
        const new_path = try std.fs.path.join(allocator, &.{ new_dir, "SKILL.md" });
        errdefer allocator.free(new_path);

        var rec = MigrateRecord{
            .name = try allocator.dupe(u8, stem),
            .old_path = old_path,
            .new_path = new_path,
            .reason = "",
        };
        errdefer freeMigrateRecord(rec, allocator);

        if (pathExists(new_dir)) {
            if (pathExists(new_path)) {
                rec.reason = try std.fmt.allocPrint(allocator, "already migrated: {s} exists", .{new_path});
            } else {
                rec.reason = try std.fmt.allocPrint(allocator, "collision: {s} exists but is not a matching skill dir", .{new_dir});
            }
            try skipped.append(allocator, rec);
            continue;
        }

        if (!opts.dry_run) {
            try std.Io.Dir.cwd().createDirPath(fsIo(), new_dir);
            try std.Io.Dir.cwd().rename(old_path, std.Io.Dir.cwd(), new_path, fsIo());
        }
        try migrated.append(allocator, rec);
    }

    std.mem.sort(MigrateRecord, migrated.items, {}, struct {
        fn lt(_: void, a: MigrateRecord, b: MigrateRecord) bool {
            return std.mem.order(u8, a.name, b.name) == .lt;
        }
    }.lt);
    std.mem.sort(MigrateRecord, skipped.items, {}, struct {
        fn lt(_: void, a: MigrateRecord, b: MigrateRecord) bool {
            return std.mem.order(u8, a.name, b.name) == .lt;
        }
    }.lt);

    return .{
        .migrated = try migrated.toOwnedSlice(allocator),
        .skipped = try skipped.toOwnedSlice(allocator),
    };
}

pub const Mode = enum { symlink, copy };

pub const ManifestRecord = struct {
    vendor: []const u8,
    target_path: []const u8,
    source_path: []const u8,
    mode: Mode,
    linked_at: []const u8 = "",
};

pub const ManifestEntry = struct {
    name: []const u8,
    source_path: []const u8,
    links: []const ManifestRecord,
};

pub const Manifest = struct {
    version: i64 = 1,
    entries: []const ManifestEntry,
};

pub fn deinitManifest(m: Manifest, allocator: std.mem.Allocator) void {
    for (m.entries) |entry| {
        allocator.free(entry.name);
        allocator.free(entry.source_path);
        for (entry.links) |link| {
            allocator.free(link.vendor);
            allocator.free(link.target_path);
            allocator.free(link.source_path);
            allocator.free(link.linked_at);
        }
        allocator.free(entry.links);
    }
    allocator.free(m.entries);
}

pub fn manifestPath(home_dir: []const u8, kind: Kind, allocator: std.mem.Allocator) ![]u8 {
    return std.fs.path.join(allocator, &.{ home_dir, ".planar", "local", kindDir(kind), manifest_filename });
}

pub fn loadManifest(home_dir: []const u8, kind: Kind, allocator: std.mem.Allocator) !Manifest {
    const path = try manifestPath(home_dir, kind, allocator);
    defer allocator.free(path);

    const raw = readFile(path, allocator, 8 * 1024 * 1024) catch |e| switch (e) {
        error.FileNotFound => {
            return .{ .version = 1, .entries = try allocator.alloc(ManifestEntry, 0) };
        },
        else => return e,
    };
    defer allocator.free(raw);

    var out = try std.json.parseFromSliceLeaky(Manifest, allocator, raw, .{
        .ignore_unknown_fields = true,
        .allocate = .alloc_always,
    });
    if (out.version == 0) out.version = 1;
    return out;
}

pub fn saveManifest(home_dir: []const u8, kind: Kind, m: Manifest, allocator: std.mem.Allocator) !void {
    const path = try manifestPath(home_dir, kind, allocator);
    defer allocator.free(path);
    if (std.fs.path.dirname(path)) |parent| {
        try std.Io.Dir.cwd().createDirPath(fsIo(), parent);
    }

    var body: std.Io.Writer.Allocating = .init(allocator);
    defer body.deinit();
    try std.json.Stringify.value(m, .{ .whitespace = .indent_2 }, &body.writer);
    try body.writer.print("\n", .{});
    try body.writer.flush();

    const tmp = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp);
    try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = tmp, .data = body.written() });
    try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), path, fsIo());
}

fn walkSkillsDir(
    dir_path: []const u8,
    allocator: std.mem.Allocator,
    files: *std.ArrayList(SandboxFile),
    errs: *std.ArrayList(WalkError),
) !void {
    var dir = openDirPath(dir_path, .{ .iterate = true }) catch |e| switch (e) {
        error.FileNotFound => return,
        else => return e,
    };
    defer dir.close(fsIo());

    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        const name = entry.name;
        if (name.len == 0 or name[0] == '.') continue;

        if (entry.kind != .directory) {
            if (std.mem.endsWith(u8, name, ".md")) {
                const p = try std.fs.path.join(allocator, &.{ dir_path, name });
                errdefer allocator.free(p);
                const msg = try std.fmt.allocPrint(
                    allocator,
                    "legacy flat skill file; run `planar local migrate` to convert to {s}/SKILL.md",
                    .{name[0 .. name.len - 3]},
                );
                errdefer allocator.free(msg);
                try errs.append(allocator, .{ .path = p, .message = msg });
            }
            continue;
        }

        const skill_md = try std.fs.path.join(allocator, &.{ dir_path, name, "SKILL.md" });
        defer allocator.free(skill_md);
        if (!pathExists(skill_md)) {
            const p = try allocator.dupe(u8, skill_md);
            errdefer allocator.free(p);
            const msg = try allocator.dupe(u8, "skill directory missing SKILL.md");
            errdefer allocator.free(msg);
            try errs.append(allocator, .{ .path = p, .message = msg });
            continue;
        }

        const f = parseFile(skill_md, .skill, allocator) catch |e| {
            const p = try allocator.dupe(u8, skill_md);
            errdefer allocator.free(p);
            const msg = try std.fmt.allocPrint(allocator, "{s}", .{@errorName(e)});
            errdefer allocator.free(msg);
            try errs.append(allocator, .{ .path = p, .message = msg });
            continue;
        };
        try files.append(allocator, f);
    }
}

fn walkAgentsDir(
    dir_path: []const u8,
    allocator: std.mem.Allocator,
    files: *std.ArrayList(SandboxFile),
    errs: *std.ArrayList(WalkError),
) !void {
    var dir = openDirPath(dir_path, .{ .iterate = true }) catch |e| switch (e) {
        error.FileNotFound => return,
        else => return e,
    };
    defer dir.close(fsIo());

    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        const name = entry.name;
        if (entry.kind != .file) continue;
        if (name.len == 0 or name[0] == '.') continue;
        if (!std.mem.endsWith(u8, name, ".md")) continue;

        const path = try std.fs.path.join(allocator, &.{ dir_path, name });
        defer allocator.free(path);
        const f = parseFile(path, .agent, allocator) catch |e| {
            const p = try allocator.dupe(u8, path);
            errdefer allocator.free(p);
            const msg = try std.fmt.allocPrint(allocator, "{s}", .{@errorName(e)});
            errdefer allocator.free(msg);
            try errs.append(allocator, .{ .path = p, .message = msg });
            continue;
        };
        try files.append(allocator, f);
    }
}

fn kindDir(kind: Kind) []const u8 {
    return switch (kind) {
        .skill => "skills",
        .agent => "agents",
    };
}

fn validateFrontmatter(fm: Frontmatter, dir_kind: Kind) !void {
    for (fm.vendors) |v| {
        if (!isKnownVendor(v)) return error.InvalidVendor;
    }
    if (fm.kind.len > 0) {
        if (dir_kind == .skill and !std.mem.eql(u8, fm.kind, "skill")) return error.KindMismatch;
        if (dir_kind == .agent and !std.mem.eql(u8, fm.kind, "agent")) return error.KindMismatch;
    }
}

fn isKnownVendor(v: []const u8) bool {
    for (all_vendors) |k| {
        if (std.mem.eql(u8, v, k)) return true;
    }
    return false;
}

const SplitFrontmatter = struct {
    frontmatter: Frontmatter,
    body: []const u8,
};

fn splitFrontmatter(content: []const u8, allocator: std.mem.Allocator) !SplitFrontmatter {
    if (!std.mem.startsWith(u8, content, "---\n")) return error.NoFrontmatter;
    const rest = content[4..];
    var end = std.mem.indexOf(u8, rest, "\n---\n");
    if (end == null and std.mem.endsWith(u8, rest, "\n---")) {
        end = rest.len - 4;
    }
    if (end == null) return error.MalformedFrontmatter;

    const yaml_block = rest[0..end.?];
    const after_close = end.? + 5;
    const body_src = if (after_close < rest.len) rest[after_close..] else "";
    const body_trim = if (std.mem.startsWith(u8, body_src, "\n")) body_src[1..] else body_src;

    const fm = try parseFrontmatter(yaml_block, allocator);
    errdefer deinitFrontmatter(fm, allocator);
    return .{
        .frontmatter = fm,
        .body = try allocator.dupe(u8, body_trim),
    };
}

fn parseFrontmatter(raw: []const u8, allocator: std.mem.Allocator) !Frontmatter {
    var vendors: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (vendors.items) |v| allocator.free(v);
        vendors.deinit(allocator);
    }

    var description = try allocator.dupe(u8, "");
    errdefer allocator.free(description);
    var argument_hint = try allocator.dupe(u8, "");
    errdefer allocator.free(argument_hint);
    var tier = try allocator.dupe(u8, "");
    errdefer allocator.free(tier);
    var model = try allocator.dupe(u8, "");
    errdefer allocator.free(model);
    var kind = try allocator.dupe(u8, "");
    errdefer allocator.free(kind);
    var shadow = false;

    var mode_vendors = false;
    var lines = std.mem.splitScalar(u8, raw, '\n');
    while (lines.next()) |line_raw| {
        const line = std.mem.trim(u8, line_raw, " \t\r");
        if (line.len == 0) continue;

        if (mode_vendors and std.mem.startsWith(u8, line, "-")) {
            const v = std.mem.trim(u8, stripInlineComment(line[1..]), " \t");
            if (v.len > 0) try vendors.append(allocator, try allocator.dupe(u8, trimQuotes(v)));
            continue;
        }
        mode_vendors = false;

        const colon = std.mem.indexOfScalar(u8, line, ':') orelse continue;
        const key = std.mem.trim(u8, line[0..colon], " \t");
        const val = std.mem.trim(u8, line[colon + 1 ..], " \t");

        if (std.mem.eql(u8, key, "description")) {
            allocator.free(description);
            description = try allocator.dupe(u8, trimQuotes(val));
            continue;
        }
        if (std.mem.eql(u8, key, "argument-hint")) {
            allocator.free(argument_hint);
            argument_hint = try allocator.dupe(u8, trimQuotes(val));
            continue;
        }
        if (std.mem.eql(u8, key, "tier")) {
            allocator.free(tier);
            tier = try allocator.dupe(u8, trimQuotes(val));
            continue;
        }
        if (std.mem.eql(u8, key, "model")) {
            allocator.free(model);
            model = try allocator.dupe(u8, trimQuotes(val));
            continue;
        }
        if (std.mem.eql(u8, key, "kind")) {
            allocator.free(kind);
            kind = try allocator.dupe(u8, trimQuotes(val));
            continue;
        }
        if (std.mem.eql(u8, key, "shadow")) {
            const norm = trimQuotes(val);
            shadow = std.mem.eql(u8, norm, "true");
            continue;
        }
        if (std.mem.eql(u8, key, "vendors")) {
            if (val.len == 0) {
                mode_vendors = true;
                continue;
            }
            if (val[0] == '[' and val.len >= 2 and val[val.len - 1] == ']') {
                var inner = std.mem.splitScalar(u8, val[1 .. val.len - 1], ',');
                while (inner.next()) |piece| {
                    const p = std.mem.trim(u8, stripInlineComment(piece), " \t");
                    if (p.len == 0) continue;
                    try vendors.append(allocator, try allocator.dupe(u8, trimQuotes(p)));
                }
            } else {
                const v = std.mem.trim(u8, stripInlineComment(val), " \t");
                if (v.len > 0) try vendors.append(allocator, try allocator.dupe(u8, trimQuotes(v)));
            }
            continue;
        }
    }

    return .{
        .description = description,
        .argument_hint = argument_hint,
        .tier = tier,
        .model = model,
        .shadow = shadow,
        .vendors = try vendors.toOwnedSlice(allocator),
        .kind = kind,
    };
}

fn trimQuotes(s: []const u8) []const u8 {
    if (s.len >= 2 and ((s[0] == '"' and s[s.len - 1] == '"') or (s[0] == '\'' and s[s.len - 1] == '\''))) {
        return s[1 .. s.len - 1];
    }
    return s;
}

fn stripInlineComment(s: []const u8) []const u8 {
    var in_single = false;
    var in_double = false;
    for (s, 0..) |c, i| {
        if (c == '"' and !in_single) {
            in_double = !in_double;
            continue;
        }
        if (c == '\'' and !in_double) {
            in_single = !in_single;
            continue;
        }
        if (c == '#' and !in_single and !in_double) {
            return s[0..i];
        }
    }
    return s;
}

fn openDirPath(path: []const u8, opts: std.Io.Dir.OpenOptions) !std.Io.Dir {
    return std.Io.Dir.cwd().openDir(fsIo(), path, opts);
}

fn readFile(path: []const u8, allocator: std.mem.Allocator, max: usize) ![]u8 {
    return std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(max));
}

fn toAbsolutePath(path: []const u8, allocator: std.mem.Allocator) ![]u8 {
    return operatorpath.absoluteCurrent(allocator, fsIo(), path) catch allocator.dupe(u8, path);
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(fsIo(), path, .{}) catch return false;
    return true;
}

fn freeMigrateRecord(rec: MigrateRecord, allocator: std.mem.Allocator) void {
    allocator.free(rec.name);
    allocator.free(rec.old_path);
    allocator.free(rec.new_path);
    if (rec.reason.len > 0) allocator.free(rec.reason);
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "walkSandbox reads skills + agents and reports legacy flat skills" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "local" });
    defer gpa.free(root);

    const skill_dir = try std.fs.path.join(gpa, &.{ root, "skills", "alpha" });
    defer gpa.free(skill_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, skill_dir);
    const skill_md = try std.fs.path.join(gpa, &.{ skill_dir, "SKILL.md" });
    defer gpa.free(skill_md);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = skill_md,
        .data =
        \\---
        \\description: alpha
        \\kind: skill
        \\---
        \\body
        ,
    });

    const agents_dir = try std.fs.path.join(gpa, &.{ root, "agents" });
    defer gpa.free(agents_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, agents_dir);
    const agent_md = try std.fs.path.join(gpa, &.{ agents_dir, "beta.md" });
    defer gpa.free(agent_md);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = agent_md,
        .data =
        \\---
        \\description: beta
        \\kind: agent
        \\---
        \\agent body
        ,
    });

    const legacy = try std.fs.path.join(gpa, &.{ root, "skills", "legacy.md" });
    defer gpa.free(legacy);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = legacy,
        .data = "---\nkind: skill\n---\n",
    });

    const walked = try walkSandbox(root, gpa);
    defer deinitWalkResult(walked, gpa);
    try std.testing.expectEqual(@as(usize, 2), walked.files.len);
    try std.testing.expectEqual(@as(usize, 1), walked.walk_errors.len);
    try std.testing.expect(std.mem.indexOf(u8, walked.walk_errors[0].message, "planar local migrate") != null);
}

test "migrate moves flat skill into dir-shape" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "local" });
    defer gpa.free(root);
    const skills = try std.fs.path.join(gpa, &.{ root, "skills" });
    defer gpa.free(skills);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, skills);
    const old = try std.fs.path.join(gpa, &.{ skills, "old.md" });
    defer gpa.free(old);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = old,
        .data = "---\nkind: skill\n---\n",
    });

    const result = try migrate(.{ .sandbox_root = root }, gpa);
    defer deinitMigrateResult(result, gpa);
    try std.testing.expectEqual(@as(usize, 1), result.migrated.len);
    try std.testing.expectEqual(@as(usize, 0), result.skipped.len);

    const new_path = try std.fs.path.join(gpa, &.{ skills, "old", "SKILL.md" });
    defer gpa.free(new_path);
    try std.Io.Dir.cwd().access(std.testing.io, new_path, .{});
}

test "frontmatter vendors accepts inline YAML comments" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "local", "skills", "cmt" });
    defer gpa.free(root);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, root);
    const skill = try std.fs.path.join(gpa, &.{ root, "SKILL.md" });
    defer gpa.free(skill);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = skill,
        .data =
        \\---
        \\description: with comments
        \\kind: skill
        \\vendors:
        \\  - claude # docs allow inline comment
        \\  - "codex" # quoted with trailing comment
        \\---
        \\body
        ,
    });

    const parsed = try parseFile(skill, .skill, gpa);
    defer deinitSandboxFile(parsed, gpa);
    try std.testing.expectEqual(@as(usize, 2), parsed.frontmatter.vendors.len);
    try std.testing.expectEqualStrings("claude", parsed.frontmatter.vendors[0]);
    try std.testing.expectEqualStrings("codex", parsed.frontmatter.vendors[1]);
}
