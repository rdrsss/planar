//! engine/llm/evidence — lightweight source-tree evidence probing.

const std = @import("std");

pub const GitCommitRef = struct {
    sha: []const u8 = "",
    title: []const u8 = "",
    date: []const u8 = "",
};

pub const FeatureArea = struct {
    name: []const u8,
    path: []const u8,
    source_files: i64,
    test_files: i64,
    first_commit: ?GitCommitRef = null,
    last_commit: ?GitCommitRef = null,
    commit_count: i64 = 0,
    signal_strength: f64,
};

pub const EvidenceMap = struct {
    layout: []const u8,
    areas: []const FeatureArea,
    total_files: i64,
    total_lines: i64,
    has_tests: bool,
    has_ci: bool,
    recent_commits: []const GitCommitRef,
};

pub fn deinitEvidence(e: EvidenceMap, allocator: std.mem.Allocator) void {
    allocator.free(e.layout);
    for (e.areas) |a| {
        allocator.free(a.name);
        allocator.free(a.path);
    }
    allocator.free(e.areas);
    allocator.free(e.recent_commits);
}

pub fn probe(repo_root: []const u8, allocator: std.mem.Allocator) !EvidenceMap {
    var area_map = std.StringHashMapUnmanaged(AreaCounter){};
    defer {
        var it = area_map.iterator();
        while (it.next()) |entry| allocator.free(entry.key_ptr.*);
        area_map.deinit(allocator);
    }

    var total_files: i64 = 0;
    var total_lines: i64 = 0;
    var has_tests = false;
    try walk(repo_root, "", allocator, &area_map, &total_files, &total_lines, &has_tests);

    const areas = try materializeAreas(allocator, &area_map, has_tests);
    errdefer {
        for (areas) |a| {
            allocator.free(a.name);
            allocator.free(a.path);
        }
        allocator.free(areas);
    }

    return .{
        .layout = try detectLayout(repo_root, allocator),
        .areas = areas,
        .total_files = total_files,
        .total_lines = total_lines,
        .has_tests = has_tests,
        .has_ci = detectCi(repo_root),
        .recent_commits = &.{},
    };
}

const AreaCounter = struct {
    source_files: i64 = 0,
    test_files: i64 = 0,
};

fn walk(
    root: []const u8,
    rel: []const u8,
    allocator: std.mem.Allocator,
    area_map: *std.StringHashMapUnmanaged(AreaCounter),
    total_files: *i64,
    total_lines: *i64,
    has_tests: *bool,
) !void {
    const dir_path = if (rel.len == 0) try allocator.dupe(u8, root) else try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(dir_path);
    var dir = try std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true });
    defer dir.close(fsIo());
    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        if (std.mem.eql(u8, entry.name, ".git")) continue;
        if (entry.name.len > 0 and entry.name[0] == '.') continue;
        const child_rel = if (rel.len == 0) try allocator.dupe(u8, entry.name) else try std.fs.path.join(allocator, &.{ rel, entry.name });
        defer allocator.free(child_rel);
        switch (entry.kind) {
            .directory => try walk(root, child_rel, allocator, area_map, total_files, total_lines, has_tests),
            .file => {
                if (!isSourceFile(entry.name)) continue;
                const file_path = try std.fs.path.join(allocator, &.{ root, child_rel });
                defer allocator.free(file_path);
                const bytes = try std.Io.Dir.cwd().readFileAlloc(fsIo(), file_path, allocator, std.Io.Limit.limited(512 * 1024));
                defer allocator.free(bytes);
                const lines = countLines(bytes);
                const area_key = try areaKey(allocator, child_rel);
                defer allocator.free(area_key);

                const found = area_map.getOrPut(allocator, area_key) catch return error.OutOfMemory;
                if (!found.found_existing) {
                    found.key_ptr.* = try allocator.dupe(u8, area_key);
                    found.value_ptr.* = .{};
                }
                if (isTestFile(entry.name) or std.mem.indexOf(u8, child_rel, "/test/") != null or std.mem.indexOf(u8, child_rel, "/tests/") != null) {
                    found.value_ptr.test_files += 1;
                    has_tests.* = true;
                } else {
                    found.value_ptr.source_files += 1;
                }
                total_files.* += 1;
                total_lines.* += @as(i64, @intCast(lines));
            },
            else => {},
        }
    }
}

fn materializeAreas(
    allocator: std.mem.Allocator,
    area_map: *std.StringHashMapUnmanaged(AreaCounter),
    has_ci: bool,
) ![]FeatureArea {
    var out: std.ArrayList(FeatureArea) = .empty;
    errdefer {
        for (out.items) |a| {
            allocator.free(a.name);
            allocator.free(a.path);
        }
        out.deinit(allocator);
    }
    var it = area_map.iterator();
    while (it.next()) |entry| {
        const path = entry.key_ptr.*;
        const v = entry.value_ptr.*;
        const signal = scoreSignal(v, has_ci);
        const nm = std.fs.path.basename(path);
        try out.append(allocator, .{
            .name = try allocator.dupe(u8, nm),
            .path = try allocator.dupe(u8, path),
            .source_files = v.source_files,
            .test_files = v.test_files,
            .signal_strength = signal,
        });
    }
    std.mem.sort(FeatureArea, out.items, {}, struct {
        fn less(_: void, a: FeatureArea, b: FeatureArea) bool {
            return std.mem.lessThan(u8, a.path, b.path);
        }
    }.less);
    return try out.toOwnedSlice(allocator);
}

fn scoreSignal(v: AreaCounter, has_ci: bool) f64 {
    if (v.source_files == 0) return 0.0;
    if (v.test_files > 0 and has_ci) return 1.0;
    if (v.test_files > 0) return 0.7;
    return 0.2;
}

fn detectLayout(root: []const u8, allocator: std.mem.Allocator) ![]const u8 {
    if (pathExists(root, "go.mod")) return try allocator.dupe(u8, "go");
    if (pathExists(root, "package.json")) return try allocator.dupe(u8, "node");
    if (pathExists(root, "pyproject.toml") or pathExists(root, "requirements.txt")) return try allocator.dupe(u8, "python");
    if (pathExists(root, "Package.swift")) return try allocator.dupe(u8, "swift");
    return try allocator.dupe(u8, "mixed");
}

fn detectCi(root: []const u8) bool {
    return pathExists(root, ".github/workflows") or
        pathExists(root, ".gitlab-ci.yml") or
        pathExists(root, ".circleci/config.yml");
}

fn pathExists(root: []const u8, rel: []const u8) bool {
    const full = std.fs.path.join(std.heap.page_allocator, &.{ root, rel }) catch return false;
    defer std.heap.page_allocator.free(full);
    std.Io.Dir.cwd().access(fsIo(), full, .{}) catch return false;
    return true;
}

fn areaKey(allocator: std.mem.Allocator, rel: []const u8) ![]const u8 {
    var it = std.mem.splitScalar(u8, rel, '/');
    _ = it.next() orelse return allocator.dupe(u8, rel);
    if (it.next()) |second| {
        const first = std.mem.sliceTo(rel, '/');
        return std.fs.path.join(allocator, &.{ first, second });
    }
    return try allocator.dupe(u8, std.mem.sliceTo(rel, '.'));
}

fn isSourceFile(name: []const u8) bool {
    const lower = std.ascii.allocLowerString(std.heap.page_allocator, name) catch return false;
    defer std.heap.page_allocator.free(lower);
    return std.mem.endsWith(u8, lower, ".go") or
        std.mem.endsWith(u8, lower, ".zig") or
        std.mem.endsWith(u8, lower, ".ts") or
        std.mem.endsWith(u8, lower, ".tsx") or
        std.mem.endsWith(u8, lower, ".js") or
        std.mem.endsWith(u8, lower, ".jsx") or
        std.mem.endsWith(u8, lower, ".py") or
        std.mem.endsWith(u8, lower, ".swift") or
        std.mem.endsWith(u8, lower, ".rs") or
        std.mem.endsWith(u8, lower, ".c") or
        std.mem.endsWith(u8, lower, ".cc") or
        std.mem.endsWith(u8, lower, ".cpp") or
        std.mem.endsWith(u8, lower, ".h") or
        std.mem.endsWith(u8, lower, ".hpp");
}

fn isTestFile(name: []const u8) bool {
    const lower = std.ascii.allocLowerString(std.heap.page_allocator, name) catch return false;
    defer std.heap.page_allocator.free(lower);
    return std.mem.endsWith(u8, lower, "_test.go") or
        std.mem.endsWith(u8, lower, ".test.ts") or
        std.mem.endsWith(u8, lower, ".spec.ts") or
        std.mem.endsWith(u8, lower, "_test.py") or
        std.mem.endsWith(u8, lower, "tests.swift");
}

fn countLines(bytes: []const u8) usize {
    if (bytes.len == 0) return 0;
    var n: usize = 1;
    for (bytes) |c| {
        if (c == '\n') n += 1;
    }
    return n;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}
