//! engine/local/import — copy sandbox skills/agents from external paths.

const std = @import("std");
const manifest = @import("manifest.zig");

pub const Opts = struct {
    home_dir: []const u8,
    source_path: []const u8,
    kind: manifest.Kind,
    force: bool = false,
    dry_run: bool = false,
};

pub const Record = struct {
    name: []const u8,
    source_path: []const u8,
    target_path: []const u8,
    action: []const u8,
    reason: []const u8 = "",
};

pub const Warning = struct {
    name: []const u8,
    field: []const u8,
    message: []const u8,
};

pub const Result = struct {
    imported: []const Record,
    skipped: []const Record,
    warnings: []const Warning,
};

pub fn deinitResult(res: Result, allocator: std.mem.Allocator) void {
    for (res.imported) |r| deinitRecord(r, allocator);
    allocator.free(res.imported);
    for (res.skipped) |r| deinitRecord(r, allocator);
    allocator.free(res.skipped);
    for (res.warnings) |w| {
        allocator.free(w.name);
        allocator.free(w.field);
        allocator.free(w.message);
    }
    allocator.free(res.warnings);
}

pub fn import(opts: Opts, allocator: std.mem.Allocator) !Result {
    if (opts.home_dir.len == 0 or opts.source_path.len == 0) return error.InvalidInput;
    const entries = try collectEntries(opts.source_path, opts.kind, allocator);
    defer deinitSourceEntries(entries, allocator);
    if (entries.len == 0) return error.NotFound;

    const dest_root = try std.fs.path.join(
        allocator,
        &.{ opts.home_dir, ".planar", "local", if (opts.kind == .skill) "skills" else "agents" },
    );
    defer allocator.free(dest_root);
    if (!opts.dry_run) try std.Io.Dir.cwd().createDirPath(fsIo(), dest_root);

    var imported: std.ArrayList(Record) = .empty;
    errdefer {
        for (imported.items) |r| deinitRecord(r, allocator);
        imported.deinit(allocator);
    }
    var skipped: std.ArrayList(Record) = .empty;
    errdefer {
        for (skipped.items) |r| deinitRecord(r, allocator);
        skipped.deinit(allocator);
    }
    var warnings: std.ArrayList(Warning) = .empty;
    errdefer {
        for (warnings.items) |w| {
            allocator.free(w.name);
            allocator.free(w.field);
            allocator.free(w.message);
        }
        warnings.deinit(allocator);
    }

    for (entries) |entry| {
        const parsed = manifest.parseFile(entry.file_source_path, opts.kind, allocator) catch |e| {
            try skipped.append(allocator, .{
                .name = try allocator.dupe(u8, entry.name),
                .source_path = try allocator.dupe(u8, entry.file_source_path),
                .target_path = try allocator.alloc(u8, 0),
                .action = try allocator.dupe(u8, "skipped"),
                .reason = try allocator.dupe(u8, classifyParseError(e)),
            });
            continue;
        };
        defer manifest.deinitSandboxFile(parsed, allocator);

        const issues = try manifest.lint(parsed.frontmatter, entry.name, allocator);
        defer manifest.deinitLint(issues, allocator);
        for (issues) |issue| {
            if (issue.severity != .warning) continue;
            try warnings.append(allocator, .{
                .name = try allocator.dupe(u8, entry.name),
                .field = try allocator.dupe(u8, issue.field),
                .message = try allocator.dupe(u8, issue.message),
            });
        }

        const target = destPath(dest_root, entry.name, opts.kind, allocator) catch |e| return e;
        defer allocator.free(target);
        const exists = pathExists(target);

        if (exists and !opts.force) {
            try skipped.append(allocator, .{
                .name = try allocator.dupe(u8, entry.name),
                .source_path = try allocator.dupe(u8, entry.file_source_path),
                .target_path = try allocator.dupe(u8, target),
                .action = try allocator.dupe(u8, "skipped"),
                .reason = try allocator.dupe(u8, "name-collision"),
            });
            continue;
        }

        const action = if (opts.dry_run) "would-import" else if (exists) "overwrote" else "imported";
        if (!opts.dry_run) try placeEntry(entry, target, opts.kind, allocator);

        try imported.append(allocator, .{
            .name = try allocator.dupe(u8, entry.name),
            .source_path = try allocator.dupe(u8, entry.file_source_path),
            .target_path = try allocator.dupe(u8, target),
            .action = try allocator.dupe(u8, action),
            .reason = try allocator.alloc(u8, 0),
        });
    }

    std.mem.sort(Record, imported.items, {}, byRecordName);
    std.mem.sort(Record, skipped.items, {}, byRecordName);
    std.mem.sort(Warning, warnings.items, {}, struct {
        fn lt(_: void, a: Warning, b: Warning) bool {
            return std.mem.order(u8, a.name, b.name) == .lt;
        }
    }.lt);

    return .{
        .imported = try imported.toOwnedSlice(allocator),
        .skipped = try skipped.toOwnedSlice(allocator),
        .warnings = try warnings.toOwnedSlice(allocator),
    };
}

const SourceEntry = struct {
    name: []const u8,
    root_source_path: []const u8,
    file_source_path: []const u8,
    is_dir: bool,
};

fn deinitSourceEntries(items: []const SourceEntry, allocator: std.mem.Allocator) void {
    for (items) |e| {
        allocator.free(e.name);
        allocator.free(e.root_source_path);
        allocator.free(e.file_source_path);
    }
    allocator.free(items);
}

fn collectEntries(src: []const u8, kind: manifest.Kind, allocator: std.mem.Allocator) ![]const SourceEntry {
    if (isDir(src)) {
        if (kind == .skill) {
            const skill_md = try std.fs.path.join(allocator, &.{ src, "SKILL.md" });
            defer allocator.free(skill_md);
            if (pathExists(skill_md)) {
                var single = try allocator.alloc(SourceEntry, 1);
                single[0] = .{
                    .name = try allocator.dupe(u8, std.fs.path.basename(src)),
                    .root_source_path = try allocator.dupe(u8, src),
                    .file_source_path = try allocator.dupe(u8, skill_md),
                    .is_dir = true,
                };
                return single;
            }
        }
        return collectFromCollection(src, kind, allocator);
    }

    if (!std.mem.endsWith(u8, src, ".md")) return error.InvalidInput;
    var one = try allocator.alloc(SourceEntry, 1);
    const base = std.fs.path.basename(src);
    one[0] = .{
        .name = try allocator.dupe(u8, base[0 .. base.len - 3]),
        .root_source_path = try allocator.dupe(u8, src),
        .file_source_path = try allocator.dupe(u8, src),
        .is_dir = false,
    };
    return one;
}

fn collectFromCollection(dir: []const u8, kind: manifest.Kind, allocator: std.mem.Allocator) ![]const SourceEntry {
    var d = openDirPath(dir, .{ .iterate = true }) catch |e| return e;
    defer d.close(fsIo());

    var out: std.ArrayList(SourceEntry) = .empty;
    errdefer {
        for (out.items) |e| {
            allocator.free(e.name);
            allocator.free(e.root_source_path);
            allocator.free(e.file_source_path);
        }
        out.deinit(allocator);
    }
    var it = d.iterate();
    while (try it.next(fsIo())) |entry| {
        const name = entry.name;
        if (name.len == 0 or name[0] == '.') continue;
        if (std.mem.eql(u8, name, manifest.manifest_filename)) continue;

        const full = try std.fs.path.join(allocator, &.{ dir, name });
        defer allocator.free(full);

        if (entry.kind == .directory) {
            if (kind != .skill) continue;
            const skill_md = try std.fs.path.join(allocator, &.{ full, "SKILL.md" });
            defer allocator.free(skill_md);
            if (!pathExists(skill_md)) continue;
            try out.append(allocator, .{
                .name = try allocator.dupe(u8, name),
                .root_source_path = try allocator.dupe(u8, full),
                .file_source_path = try allocator.dupe(u8, skill_md),
                .is_dir = true,
            });
            continue;
        }

        if (!std.mem.endsWith(u8, name, ".md")) continue;
        try out.append(allocator, .{
            .name = try allocator.dupe(u8, name[0 .. name.len - 3]),
            .root_source_path = try allocator.dupe(u8, full),
            .file_source_path = try allocator.dupe(u8, full),
            .is_dir = false,
        });
    }

    std.mem.sort(SourceEntry, out.items, {}, struct {
        fn lt(_: void, a: SourceEntry, b: SourceEntry) bool {
            return std.mem.order(u8, a.name, b.name) == .lt;
        }
    }.lt);
    return try out.toOwnedSlice(allocator);
}

fn destPath(dest_root: []const u8, name: []const u8, kind: manifest.Kind, allocator: std.mem.Allocator) ![]u8 {
    if (kind == .skill) return std.fs.path.join(allocator, &.{ dest_root, name, "SKILL.md" });
    const file_name = try std.fmt.allocPrint(allocator, "{s}.md", .{name});
    defer allocator.free(file_name);
    return std.fs.path.join(allocator, &.{ dest_root, file_name });
}

fn placeEntry(entry: SourceEntry, target: []const u8, kind: manifest.Kind, allocator: std.mem.Allocator) !void {
    switch (kind) {
        .skill => {
            const dest_dir = std.fs.path.dirname(target) orelse return error.InvalidInput;
            if (entry.is_dir) {
                _ = std.Io.Dir.cwd().deleteTree(fsIo(), dest_dir) catch {};
                try copyTree(entry.root_source_path, dest_dir, allocator);
                return;
            }
            try std.Io.Dir.cwd().createDirPath(fsIo(), dest_dir);
            try copyFile(entry.file_source_path, target, allocator);
        },
        .agent => try copyFile(entry.file_source_path, target, allocator),
    }
}

fn copyTree(src: []const u8, dst: []const u8, allocator: std.mem.Allocator) !void {
    try std.Io.Dir.cwd().createDirPath(fsIo(), dst);
    var d = openDirPath(src, .{ .iterate = true }) catch |e| return e;
    defer d.close(fsIo());
    var it = d.iterate();
    while (try it.next(fsIo())) |entry| {
        const child_src = try std.fs.path.join(allocator, &.{ src, entry.name });
        defer allocator.free(child_src);
        const child_dst = try std.fs.path.join(allocator, &.{ dst, entry.name });
        defer allocator.free(child_dst);
        switch (entry.kind) {
            .directory => try copyTree(child_src, child_dst, allocator),
            .sym_link => {
                var buf: [std.fs.max_path_bytes]u8 = undefined;
                const n = try std.Io.Dir.cwd().readLink(fsIo(), child_src, &buf);
                try std.Io.Dir.cwd().symLink(fsIo(), buf[0..n], child_dst, .{});
            },
            else => try copyFile(child_src, child_dst, allocator),
        }
    }
}

fn copyFile(src: []const u8, dst: []const u8, allocator: std.mem.Allocator) !void {
    const body = try readFile(src, allocator, 16 * 1024 * 1024);
    defer allocator.free(body);
    const tmp = try std.fmt.allocPrint(allocator, "{s}.planar-tmp", .{dst});
    defer allocator.free(tmp);
    try std.Io.Dir.cwd().writeFile(fsIo(), .{ .sub_path = tmp, .data = body });
    try std.Io.Dir.cwd().rename(tmp, std.Io.Dir.cwd(), dst, fsIo());
}

fn classifyParseError(e: anyerror) []const u8 {
    return switch (e) {
        error.NoFrontmatter => "no-frontmatter",
        error.KindMismatch => "kind-mismatch",
        error.InvalidVendor => "invalid-vendor",
        else => "invalid-frontmatter",
    };
}

fn byRecordName(_: void, a: Record, b: Record) bool {
    return std.mem.order(u8, a.name, b.name) == .lt;
}

fn deinitRecord(r: Record, allocator: std.mem.Allocator) void {
    allocator.free(r.name);
    allocator.free(r.source_path);
    allocator.free(r.target_path);
    allocator.free(r.action);
    allocator.free(r.reason);
}

fn pathExists(path: []const u8) bool {
    std.Io.Dir.cwd().access(fsIo(), path, .{}) catch return false;
    return true;
}

fn isDir(path: []const u8) bool {
    if (openDirPath(path, .{})) |d| {
        d.close(fsIo());
        return true;
    } else |_| {
        return false;
    }
}

fn openDirPath(path: []const u8, opts: std.Io.Dir.OpenOptions) !std.Io.Dir {
    return std.Io.Dir.cwd().openDir(fsIo(), path, opts);
}

fn readFile(path: []const u8, allocator: std.mem.Allocator, max: usize) ![]u8 {
    return std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(max));
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "import copies one flat skill file to dir-shape" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const home = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "home" });
    defer gpa.free(home);
    const src = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "src.md" });
    defer gpa.free(src);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src,
        .data =
        \\---
        \\description: s
        \\kind: skill
        \\---
        \\x
        ,
    });

    const res = try import(.{
        .home_dir = home,
        .source_path = src,
        .kind = .skill,
    }, gpa);
    defer deinitResult(res, gpa);
    try std.testing.expectEqual(@as(usize, 1), res.imported.len);

    const want = try std.fs.path.join(gpa, &.{ home, ".planar", "local", "skills", "src", "SKILL.md" });
    defer gpa.free(want);
    try std.Io.Dir.cwd().access(std.testing.io, want, .{});
}
