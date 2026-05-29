//! engine/docs/walk — working-tree walker for the repo-state manifest
//! (plan 423 M2).
//!
//! Yields every regular file in the repository that survives the coverage
//! policy. Three exclusion rules apply, in this order of evaluation:
//!
//!   1. `.git/` directory and anything beneath it.
//!   2. Any path whose name (or any ancestor's name) begins with a dot.
//!      Excludes `.claude/`, `.copilot/`, `.sqlfluff`, `.planar-manifest`
//!      itself, etc. The `.gitignore` file is technically a dotfile and
//!      therefore excluded from the manifest's coverage, but we still
//!      consult it to derive the exclusion set.
//!   3. Any path matching a `.gitignore` pattern loaded from the repo root.
//!
//! Inclusions are everything else: `src/`, `migrations/`, `templates/`,
//! `vendor/`, `build.zig`, `build.zig.zon`, and `docs/`.
//!
//! Symlinks are NOT followed (Q330 — the merkle module hashes a symlink by
//! its `readlink` target string when it's inside a directory the walker
//! does descend into). Regular files are emitted to the caller's callback.

const std = @import("std");

const c = @cImport({
    @cInclude("dirent.h");
    @cInclude("sys/stat.h");
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
});

/// One entry yielded by the walker.
pub const Entry = struct {
    /// Path relative to the walk root.
    rel_path: []const u8,
    kind: Kind,

    pub const Kind = enum { file, symlink };
};

pub const Callback = fn (ctx: *anyopaque, entry: Entry) anyerror!void;

/// Walk `root` recursively and invoke `cb` for every regular file or symlink
/// that survives the coverage policy.
pub fn walk(
    allocator: std.mem.Allocator,
    root: []const u8,
    cb_ctx: *anyopaque,
    cb: *const Callback,
) !void {
    var ignore = try loadGitignore(allocator, root);
    defer ignore.deinit(allocator);
    try walkSubtree(allocator, root, "", &ignore, cb_ctx, cb);
}

fn walkSubtree(
    allocator: std.mem.Allocator,
    root: []const u8,
    rel_dir: []const u8,
    ignore: *const Gitignore,
    cb_ctx: *anyopaque,
    cb: *const Callback,
) !void {
    const abs_dir = if (rel_dir.len == 0)
        try allocator.dupe(u8, root)
    else
        try std.fs.path.join(allocator, &.{ root, rel_dir });
    defer allocator.free(abs_dir);

    const dir_z = try allocator.dupeZ(u8, abs_dir);
    defer allocator.free(dir_z);
    const dp = c.opendir(dir_z.ptr) orelse return;
    defer _ = c.closedir(dp);

    while (c.readdir(dp)) |ent| {
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&ent.*.d_name)));
        if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;

        // Rule 2: dotfile (or dotfile ancestor) → skip.
        if (name[0] == '.') continue;

        const child_rel = if (rel_dir.len == 0)
            try allocator.dupe(u8, name)
        else
            try std.fs.path.join(allocator, &.{ rel_dir, name });
        defer allocator.free(child_rel);

        const child_abs = try std.fs.path.join(allocator, &.{ root, child_rel });
        defer allocator.free(child_abs);
        const child_z = try allocator.dupeZ(u8, child_abs);
        defer allocator.free(child_z);

        var st: c.struct_stat = undefined;
        if (c.lstat(child_z.ptr, &st) != 0) continue;

        const is_dir = (st.st_mode & c.S_IFMT) == c.S_IFDIR;
        const is_link = (st.st_mode & c.S_IFMT) == c.S_IFLNK;

        // Rule 3: gitignore match → skip. Directory patterns can shortcut
        // descent for the whole subtree; file patterns drop one entry.
        if (ignore.matches(child_rel, is_dir)) continue;

        if (is_dir) {
            try walkSubtree(allocator, root, child_rel, ignore, cb_ctx, cb);
            continue;
        }

        const kind: Entry.Kind = if (is_link) .symlink else .file;
        try cb(cb_ctx, .{ .rel_path = child_rel, .kind = kind });
    }
}

// ============================================================================
// Minimal `.gitignore` matcher.
//
// Supports the pattern subset this repo actually uses:
//   - Comment lines starting with `#` and blank lines are ignored.
//   - Trailing `/` constrains a pattern to directory-only.
//   - Leading `/` anchors at the gitignore's directory (we only support
//     gitignore at the repo root, so this means "match at the repo root").
//   - `*` is any sequence of non-`/` characters.
//   - `**` matches any number of path segments.
//   - `?` is exactly one non-`/` character.
//   - Negation patterns (`!foo`) are recorded but applied only by the
//     standard "last matching pattern wins" rule: a later negation flips
//     the earlier match off.
//   - Comments mid-line are NOT supported (matches git's behavior).
//
// Anything outside this subset (e.g. `[abc]` character classes) is treated
// as a literal. Real-world gitignore files in Planar use only the supported
// subset, so this isn't a coverage problem.
// ============================================================================

pub const Gitignore = struct {
    patterns: []Pattern = &.{},

    pub const Pattern = struct {
        glob: []const u8,
        anchored: bool,
        directory_only: bool,
        negate: bool,
    };

    pub fn deinit(self: *Gitignore, allocator: std.mem.Allocator) void {
        for (self.patterns) |p| allocator.free(p.glob);
        allocator.free(self.patterns);
    }

    /// Return true if `rel_path` matches any include pattern AND no later
    /// negation un-matches it.
    pub fn matches(self: *const Gitignore, rel_path: []const u8, is_dir: bool) bool {
        var matched = false;
        for (self.patterns) |p| {
            if (p.directory_only and !is_dir) continue;
            if (matchPattern(p, rel_path)) {
                matched = !p.negate;
            }
        }
        return matched;
    }
};

/// Parse the repo-root `.gitignore` if present; return an empty Gitignore
/// otherwise.
pub fn loadGitignore(allocator: std.mem.Allocator, root: []const u8) !Gitignore {
    const path = try std.fs.path.join(allocator, &.{ root, ".gitignore" });
    defer allocator.free(path);
    const path_z = try allocator.dupeZ(u8, path);
    defer allocator.free(path_z);

    const fd = c.open(path_z.ptr, c.O_RDONLY);
    if (fd < 0) return .{ .patterns = &.{} };
    defer _ = c.close(fd);

    var st: c.struct_stat = undefined;
    if (c.fstat(fd, &st) != 0) return .{ .patterns = &.{} };
    const size: usize = @intCast(st.st_size);
    const body = try allocator.alloc(u8, size);
    defer allocator.free(body);
    var off: usize = 0;
    while (off < size) {
        const n = c.read(fd, @as([*]u8, @ptrCast(body.ptr)) + off, size - off);
        if (n <= 0) break;
        off += @intCast(n);
    }
    return parseGitignore(allocator, body[0..off]);
}

pub fn parseGitignore(allocator: std.mem.Allocator, body: []const u8) !Gitignore {
    var patterns: std.ArrayList(Gitignore.Pattern) = .empty;
    errdefer {
        for (patterns.items) |p| allocator.free(p.glob);
        patterns.deinit(allocator);
    }

    var lines = std.mem.tokenizeScalar(u8, body, '\n');
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \r\t");
        if (line.len == 0) continue;
        if (line[0] == '#') continue;

        var rest: []const u8 = line;
        var negate = false;
        if (rest[0] == '!') {
            negate = true;
            rest = rest[1..];
            if (rest.len == 0) continue;
        }
        var anchored = false;
        if (rest[0] == '/') {
            anchored = true;
            rest = rest[1..];
            if (rest.len == 0) continue;
        }
        var directory_only = false;
        if (rest.len > 0 and rest[rest.len - 1] == '/') {
            directory_only = true;
            rest = rest[0 .. rest.len - 1];
            if (rest.len == 0) continue;
        }

        try patterns.append(allocator, .{
            .glob = try allocator.dupe(u8, rest),
            .anchored = anchored,
            .directory_only = directory_only,
            .negate = negate,
        });
    }
    return .{ .patterns = try patterns.toOwnedSlice(allocator) };
}

/// Test whether `rel_path` matches `pattern`. Implements the subset
/// documented at the top of this module.
fn matchPattern(p: Gitignore.Pattern, rel_path: []const u8) bool {
    if (p.anchored) return globMatch(p.glob, rel_path);

    // Non-anchored: try the full path, AND try each suffix beginning at a
    // `/` boundary. This is the standard gitignore semantic for unanchored
    // patterns ("match anywhere in the tree").
    if (globMatch(p.glob, rel_path)) return true;
    var i: usize = 0;
    while (i < rel_path.len) : (i += 1) {
        if (rel_path[i] != '/') continue;
        if (globMatch(p.glob, rel_path[i + 1 ..])) return true;
    }
    return false;
}

/// Standard wildcard glob: `*` matches zero or more non-`/` chars, `?`
/// matches exactly one non-`/` char, `**` matches any number of path
/// segments (across `/`). Backtracking is bounded by O(|glob| * |path|).
fn globMatch(glob: []const u8, path: []const u8) bool {
    return globMatchInner(glob, 0, path, 0);
}

fn globMatchInner(glob: []const u8, gi_in: usize, path: []const u8, pi_in: usize) bool {
    var gi: usize = gi_in;
    var pi: usize = pi_in;
    while (gi < glob.len) {
        const gc = glob[gi];
        if (gc == '*') {
            // `**` matches across `/`; single `*` does not.
            const double = gi + 1 < glob.len and glob[gi + 1] == '*';
            const next_gi = if (double) gi + 2 else gi + 1;
            // Try matching 0 or more characters at `pi`.
            while (pi <= path.len) : (pi += 1) {
                if (globMatchInner(glob, next_gi, path, pi)) return true;
                if (pi == path.len) return false;
                if (!double and path[pi] == '/') return false;
            }
            return false;
        } else if (gc == '?') {
            if (pi >= path.len or path[pi] == '/') return false;
            gi += 1;
            pi += 1;
        } else {
            if (pi >= path.len or path[pi] != gc) return false;
            gi += 1;
            pi += 1;
        }
    }
    return pi == path.len;
}

// ============================================================================
// Tests
// ============================================================================

const testing = std.testing;

test "globMatch literal" {
    try testing.expect(globMatch("foo.txt", "foo.txt"));
    try testing.expect(!globMatch("foo.txt", "bar.txt"));
}

test "globMatch single star does not cross slash" {
    try testing.expect(globMatch("*.txt", "foo.txt"));
    try testing.expect(globMatch("*.txt", "bar.txt"));
    try testing.expect(!globMatch("*.txt", "foo.png"));
    try testing.expect(!globMatch("*.txt", "subdir/foo.txt"));
}

test "globMatch double star crosses slashes" {
    try testing.expect(globMatch("**.txt", "foo.txt"));
    try testing.expect(globMatch("**.txt", "subdir/foo.txt"));
    try testing.expect(globMatch("**/foo", "a/b/c/foo"));
}

test "globMatch question mark" {
    try testing.expect(globMatch("?.txt", "a.txt"));
    try testing.expect(!globMatch("?.txt", "ab.txt"));
}

test "parseGitignore skips comments and blanks" {
    var ignore = try parseGitignore(testing.allocator,
        \\# a comment
        \\
        \\zig-out/
        \\
        \\# trailing comment
        \\*.o
    );
    defer ignore.deinit(testing.allocator);
    try testing.expectEqual(@as(usize, 2), ignore.patterns.len);
    try testing.expectEqualStrings("zig-out", ignore.patterns[0].glob);
    try testing.expect(ignore.patterns[0].directory_only);
    try testing.expectEqualStrings("*.o", ignore.patterns[1].glob);
    try testing.expect(!ignore.patterns[1].directory_only);
}

test "Gitignore matches directory-only pattern only against dirs" {
    var ignore = try parseGitignore(testing.allocator, "build/");
    defer ignore.deinit(testing.allocator);
    try testing.expect(ignore.matches("build", true));
    try testing.expect(!ignore.matches("build", false));
    try testing.expect(ignore.matches("subdir/build", true));
}

test "Gitignore anchored pattern only matches at root" {
    var ignore = try parseGitignore(testing.allocator, "/bin/");
    defer ignore.deinit(testing.allocator);
    try testing.expect(ignore.matches("bin", true));
    // Anchored: subdir/bin should NOT match because the pattern is rooted.
    try testing.expect(!ignore.matches("subdir/bin", true));
}

test "Gitignore unanchored pattern matches at any depth" {
    var ignore = try parseGitignore(testing.allocator, "*.o");
    defer ignore.deinit(testing.allocator);
    try testing.expect(ignore.matches("foo.o", false));
    try testing.expect(ignore.matches("subdir/foo.o", false));
    try testing.expect(ignore.matches("deeply/nested/foo.o", false));
    try testing.expect(!ignore.matches("foo.txt", false));
}

test "Gitignore negation: later negation flips earlier match" {
    var ignore = try parseGitignore(testing.allocator,
        \\target/
        \\!target/keep
    );
    defer ignore.deinit(testing.allocator);
    try testing.expect(ignore.matches("target", true));
    // The negation pattern only matches when the subpath equals "target/keep"
    // — in directory terms, "target/keep". The simpler test: a file that
    // exactly matches the negation pattern is no longer ignored.
    try testing.expect(!ignore.matches("target/keep", false));
}

test "walk emits regular files only; skips dotfiles and gitignore matches" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();

    var buf: [std.fs.max_path_bytes]u8 = undefined;
    const root_len = try tmp.dir.realPath(std.testing.io, &buf);
    const root = buf[0..root_len];

    // Seed: src/main.zig, src/.hidden, build.zig, .secret, .git/HEAD,
    // zig-out/binary, .gitignore.
    try seedFile(testing.allocator, root, "src/main.zig", "code");
    try seedFile(testing.allocator, root, "src/.hidden", "no");
    try seedFile(testing.allocator, root, "build.zig", "build");
    try seedFile(testing.allocator, root, ".secret", "no");
    try seedFile(testing.allocator, root, ".git/HEAD", "no");
    try seedFile(testing.allocator, root, "zig-out/binary", "no");
    try seedFile(testing.allocator, root, ".gitignore", "zig-out/\n");

    var collector = Collector{};
    try walk(testing.allocator, root, &collector, Collector.cb);
    defer collector.deinit();

    // Expect: src/main.zig and build.zig only.
    try testing.expectEqual(@as(usize, 2), collector.items.items.len);
    var seen_main = false;
    var seen_build = false;
    for (collector.items.items) |path| {
        if (std.mem.eql(u8, path, "src/main.zig")) seen_main = true;
        if (std.mem.eql(u8, path, "build.zig")) seen_build = true;
    }
    try testing.expect(seen_main);
    try testing.expect(seen_build);
}

// ----------------------------------------------------------------------
// Test helpers

const Collector = struct {
    items: std.ArrayList([]const u8) = .empty,

    fn cb(ctx: *anyopaque, entry: Entry) anyerror!void {
        const self: *Collector = @ptrCast(@alignCast(ctx));
        try self.items.append(testing.allocator, try testing.allocator.dupe(u8, entry.rel_path));
    }

    fn deinit(self: *Collector) void {
        for (self.items.items) |p| testing.allocator.free(p);
        self.items.deinit(testing.allocator);
    }
};

fn seedFile(allocator: std.mem.Allocator, root: []const u8, rel: []const u8, content: []const u8) !void {
    const abs = try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(abs);
    if (std.fs.path.dirname(abs)) |parent| {
        const parent_z = try allocator.dupeZ(u8, parent);
        defer allocator.free(parent_z);
        _ = c.mkdir(parent_z.ptr, 0o755);
        // Best effort recursive: mkdir for each ancestor.
        var i: usize = 0;
        while (i < parent.len) : (i += 1) {
            if (parent[i] != '/') continue;
            const part_z = try allocator.dupeZ(u8, parent[0..i]);
            defer allocator.free(part_z);
            if (part_z.len > 0) _ = c.mkdir(part_z.ptr, 0o755);
        }
        _ = c.mkdir(parent_z.ptr, 0o755);
    }
    const abs_z = try allocator.dupeZ(u8, abs);
    defer allocator.free(abs_z);
    const fd = c.open(abs_z.ptr, c.O_WRONLY | c.O_CREAT | c.O_TRUNC, @as(c_uint, 0o644));
    if (fd < 0) return error.OpenFailed;
    defer _ = c.close(fd);
    if (content.len > 0) _ = c.write(fd, content.ptr, content.len);
}

extern "c" fn mkdir(path: [*:0]const u8, mode: c.mode_t) c_int;
