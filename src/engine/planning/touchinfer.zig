//! engine/planning/touchinfer — propose path-level task touches from a
//! task's own text, resolved against the repo tree.
//!
//! `task_touch_paths` rows are load-bearing twice over: rule 2 of the
//! parallel-eligibility rules reads them (an empty touch set is treated as
//! "touches everything" → serialize), and `engine/closure/store.zig` uses
//! them as the SEEDS for closure extraction (`NoSeeds` without them). Yet
//! nothing populates them but hand declaration, so the overwhelming majority
//! of open tasks are serialized for lack of a declaration rather than for
//! genuine conflict.
//!
//! This module proposes; it never writes. The caller previews and applies.
//!
//! ## The over-declaration bias (decision 906)
//!
//! The two error directions are NOT symmetric:
//!
//!   - OVER-declaring costs throughput. The task serializes when it might
//!     have run in parallel. Recoverable at any time by declaring more
//!     precisely.
//!   - UNDER-declaring costs correctness. Two tasks are marked eligible,
//!     fanned out into separate worktrees, both edit the same file, and the
//!     collision surfaces at fan-in merge — after both burned a full cycle.
//!
//! So every ambiguity resolves WIDE: a directory token expands to its files,
//! a bare basename yields EVERY matching path rather than a guess, and a
//! token we cannot place is reported as `unresolved` rather than dropped
//! silently. Reporting beats guessing; guessing beats nothing only when the
//! guess is safe, and here it is not.
//!
//! ## Scope of v1 — deterministic only
//!
//! No model call. Extraction is literal path-shaped-token detection plus
//! filesystem resolution. Deterministic recall is measurable; if it proves
//! insufficient, an interpretation pass can layer on top with evidence for
//! why. Measure first.

const std = @import("std");
const db = @import("db");

pub const Error = error{
    NotFound,
    QueryFailed,
} || std.mem.Allocator.Error;

/// The canonical blocking `std.Io` for disk reads (mirrors
/// `engine/closure/store.zig`'s `fsIo`). The engine holds no IO writers.
fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

/// Cap on how many files a single directory token may expand to. A token
/// naming a huge tree is reported as `too_broad` rather than silently
/// expanding to hundreds of rows — over-declaration is safe, but an
/// unreviewable preview is not.
pub const max_directory_expansion: usize = 64;

/// Which field of the task produced a candidate. Carried into the preview so
/// the operator can judge the evidence, not just the verdict.
pub const Evidence = enum {
    title,
    body,
    next_action,
    citation,

    pub fn toText(self: Evidence) []const u8 {
        return switch (self) {
            .title => "title",
            .body => "body",
            .next_action => "next_action",
            .citation => "citation",
        };
    }
};

/// How a candidate token resolved against the repo tree.
pub const Classification = enum {
    /// Exact repo-relative path of an existing file.
    resolved,
    /// Token named a directory; expanded to the files beneath it.
    directory,
    /// Bare basename matching one or more files anywhere in the tree. ALL
    /// matches are proposed (over-declaration bias) — never a single guess.
    basename,
    /// Path-shaped but no match in the tree. Reported, never written.
    unresolved,
    /// A directory whose expansion exceeded `max_directory_expansion`.
    /// Reported so the operator can declare a narrower path deliberately.
    too_broad,

    pub fn toText(self: Classification) []const u8 {
        return switch (self) {
            .resolved => "resolved",
            .directory => "directory",
            .basename => "basename",
            .unresolved => "unresolved",
            .too_broad => "too_broad",
        };
    }

    /// Whether a candidate of this classification contributes writable rows.
    /// `unresolved` and `too_broad` are preview-only by construction.
    pub fn isWritable(self: Classification) bool {
        return switch (self) {
            .resolved, .directory, .basename => true,
            .unresolved, .too_broad => false,
        };
    }
};

/// One token lifted from the task text, with how it resolved.
pub const Candidate = struct {
    /// The literal token as it appeared in the task text.
    token: []const u8,
    evidence: Evidence,
    classification: Classification,
    /// Repo-relative paths this candidate proposes. Empty for `unresolved`
    /// and `too_broad`. More than one for `directory` and `basename`.
    paths: []const []const u8,
    /// The repo (projects.id) the paths are relative to.
    repo_id: i64,
};

pub const Inference = struct {
    task_id: i64,
    candidates: []const Candidate,
    arena: std.heap.ArenaAllocator,

    pub fn deinit(self: *Inference) void {
        self.arena.deinit();
    }

    /// Count of candidates that would produce `task_touch_paths` rows.
    pub fn writableCount(self: Inference) usize {
        var n: usize = 0;
        for (self.candidates) |c| {
            if (c.classification.isWritable()) n += c.paths.len;
        }
        return n;
    }

    /// Count of candidates surfaced for review but not written.
    pub fn reviewCount(self: Inference) usize {
        var n: usize = 0;
        for (self.candidates) |c| {
            if (!c.classification.isWritable()) n += 1;
        }
        return n;
    }
};

// =========================================================================
// Token extraction — pure, no filesystem, no database
// =========================================================================

/// Characters stripped from either end of a token before resolution. Task
/// prose wraps paths in backticks, quotes, parens, and trailing sentence
/// punctuation; none of that is part of the path.
const trim_chars = " \t\r\n`'\"()[]{}<>,;:";

/// True when `s` looks like it could name a path worth resolving. This is
/// deliberately permissive — a false candidate costs one `unresolved` row in
/// the preview, while a missed candidate costs a silent under-declaration.
pub fn isPathShaped(s: []const u8) bool {
    if (s.len == 0 or s.len > 512) return false;

    // URLs are never repo paths, and `://` would otherwise read as a
    // separator-bearing token.
    if (std.mem.indexOf(u8, s, "://") != null) return false;

    // Whitespace inside a token means the tokenizer already split wrong.
    for (s) |c| {
        if (c == ' ' or c == '\t' or c == '\n' or c == '\r') return false;
    }

    // Absolute paths and parent traversal are out of scope: touches are
    // repo-relative by definition.
    if (s[0] == '/') return false;
    if (std.mem.startsWith(u8, s, "../")) return false;

    const has_sep = std.mem.indexOfScalar(u8, s, '/') != null;
    const dot = std.mem.lastIndexOfScalar(u8, s, '.');
    const has_ext = dot != null and dot.? > 0 and dot.? + 1 < s.len;

    // `src/engine/foo.zig` or `docs/` — a separator is strong evidence.
    if (has_sep) return true;

    // A bare `strategy.zig` is a candidate; a bare `README` or an English
    // word is not. Requiring an extension keeps prose out.
    return has_ext;
}

/// Split `text` on whitespace and yield trimmed, path-shaped tokens.
/// Duplicates are preserved here; `infer` dedupes across the whole task.
pub fn extractTokens(
    a: std.mem.Allocator,
    text: []const u8,
) std.mem.Allocator.Error![]const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    var it = std.mem.tokenizeAny(u8, text, " \t\r\n");
    while (it.next()) |raw| {
        const tok = std.mem.trim(u8, raw, trim_chars);
        if (!isPathShaped(tok)) continue;
        try out.append(a, tok);
    }
    return out.toOwnedSlice(a);
}

// =========================================================================
// Filesystem resolution
// =========================================================================

const EntryKind = enum { file, directory, missing };

fn statRel(root: []const u8, rel: []const u8) EntryKind {
    const joined = std.fs.path.join(
        std.heap.page_allocator,
        &.{ root, rel },
    ) catch return .missing;
    defer std.heap.page_allocator.free(joined);

    var dir = std.Io.Dir.cwd().openDir(fsIo(), joined, .{}) catch {
        const f = std.Io.Dir.cwd().openFile(fsIo(), joined, .{}) catch return .missing;
        var mf = f;
        mf.close(fsIo());
        return .file;
    };
    dir.close(fsIo());
    return .directory;
}

/// Collect repo-relative paths of every non-hidden file under `root`/`rel`,
/// stopping once `limit` is exceeded (the caller reports `too_broad`).
fn collectUnder(
    a: std.mem.Allocator,
    root: []const u8,
    rel: []const u8,
    limit: usize,
    out: *std.ArrayList([]const u8),
) std.mem.Allocator.Error!void {
    if (out.items.len > limit) return;

    const dir_path = if (rel.len == 0)
        root
    else
        std.fs.path.join(a, &.{ root, rel }) catch return;

    var dir = std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true }) catch return;
    defer dir.close(fsIo());

    var it = dir.iterate();
    while (it.next(fsIo()) catch null) |entry| {
        // Hidden entries (.git, .zig-cache, …) are never touch targets.
        if (entry.name.len > 0 and entry.name[0] == '.') continue;
        if (out.items.len > limit) return;

        const child_rel = if (rel.len == 0)
            a.dupe(u8, entry.name) catch continue
        else
            std.fs.path.join(a, &.{ rel, entry.name }) catch continue;

        switch (entry.kind) {
            .directory => try collectUnder(a, root, child_rel, limit, out),
            .file => try out.append(a, child_rel),
            else => continue,
        }
    }
}

/// Find every file under `root` whose basename equals `name`. Used for the
/// bare-basename case: ALL matches are returned, never one guess.
fn findByBasename(
    a: std.mem.Allocator,
    root: []const u8,
    rel: []const u8,
    name: []const u8,
    limit: usize,
    out: *std.ArrayList([]const u8),
) std.mem.Allocator.Error!void {
    if (out.items.len > limit) return;

    const dir_path = if (rel.len == 0)
        root
    else
        std.fs.path.join(a, &.{ root, rel }) catch return;

    var dir = std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true }) catch return;
    defer dir.close(fsIo());

    var it = dir.iterate();
    while (it.next(fsIo()) catch null) |entry| {
        if (entry.name.len > 0 and entry.name[0] == '.') continue;
        if (out.items.len > limit) return;

        const child_rel = if (rel.len == 0)
            a.dupe(u8, entry.name) catch continue
        else
            std.fs.path.join(a, &.{ rel, entry.name }) catch continue;

        switch (entry.kind) {
            .directory => try findByBasename(a, root, child_rel, name, limit, out),
            .file => {
                if (std.mem.eql(u8, entry.name, name)) try out.append(a, child_rel);
            },
            else => continue,
        }
    }
}

/// Resolve one token against one repo root. Returns the classification and
/// the repo-relative paths it proposes.
pub fn classifyToken(
    a: std.mem.Allocator,
    root: []const u8,
    token: []const u8,
) std.mem.Allocator.Error!struct {
    classification: Classification,
    paths: []const []const u8,
} {
    // Normalize a trailing slash: `docs/` and `docs` name the same directory.
    const norm = if (token.len > 1 and token[token.len - 1] == '/')
        token[0 .. token.len - 1]
    else
        token;

    switch (statRel(root, norm)) {
        .file => {
            const one = try a.alloc([]const u8, 1);
            one[0] = try a.dupe(u8, norm);
            return .{ .classification = .resolved, .paths = one };
        },
        .directory => {
            var acc: std.ArrayList([]const u8) = .empty;
            try collectUnder(a, root, norm, max_directory_expansion, &acc);
            if (acc.items.len > max_directory_expansion) {
                return .{ .classification = .too_broad, .paths = &.{} };
            }
            if (acc.items.len == 0) {
                return .{ .classification = .unresolved, .paths = &.{} };
            }
            return .{
                .classification = .directory,
                .paths = try acc.toOwnedSlice(a),
            };
        },
        .missing => {},
    }

    // Not a literal path. A separator-free token may still be a basename
    // occurring somewhere in the tree — propose EVERY match.
    if (std.mem.indexOfScalar(u8, norm, '/') == null) {
        var acc: std.ArrayList([]const u8) = .empty;
        try findByBasename(a, root, "", norm, max_directory_expansion, &acc);
        if (acc.items.len > 0 and acc.items.len <= max_directory_expansion) {
            return .{
                .classification = .basename,
                .paths = try acc.toOwnedSlice(a),
            };
        }
        if (acc.items.len > max_directory_expansion) {
            return .{ .classification = .too_broad, .paths = &.{} };
        }
    }

    return .{ .classification = .unresolved, .paths = &.{} };
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "isPathShaped accepts repo-relative paths and rejects prose" {
    try testing.expect(isPathShaped("src/engine/planning/touchinfer.zig"));
    try testing.expect(isPathShaped("docs/cli-reference.md"));
    try testing.expect(isPathShaped("workflows/"));
    try testing.expect(isPathShaped("strategy.zig"));

    // Prose, URLs, absolute paths, traversal — none are repo touches.
    try testing.expect(!isPathShaped("the"));
    try testing.expect(!isPathShaped("serialize"));
    try testing.expect(!isPathShaped("https://example.com/a.zig"));
    try testing.expect(!isPathShaped("/etc/passwd"));
    try testing.expect(!isPathShaped("../outside.zig"));
    try testing.expect(!isPathShaped(""));
}

test "extractTokens lifts paths out of prose and strips punctuation" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();

    const text =
        \\Delete `workflows/status.lua` and workflows/health.lua, then drop
        \\the import at integration_tests/all_test.zig (line 108).
    ;
    const toks = try extractTokens(a, text);

    try testing.expectEqual(@as(usize, 3), toks.len);
    try testing.expectEqualStrings("workflows/status.lua", toks[0]);
    try testing.expectEqualStrings("workflows/health.lua", toks[1]);
    try testing.expectEqualStrings("integration_tests/all_test.zig", toks[2]);
}

test "extractTokens ignores prose that merely contains dots" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();

    const toks = try extractTokens(a, "Run it. Then verify. No paths here!");
    try testing.expectEqual(@as(usize, 0), toks.len);
}

test "classifyToken: exact file resolves to itself" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(root);

    try tmp.dir.makePath("src/engine");
    try tmp.dir.writeFile(.{ .sub_path = "src/engine/a.zig", .data = "x" });

    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const r = try classifyToken(arena.allocator(), root, "src/engine/a.zig");
    try testing.expectEqual(Classification.resolved, r.classification);
    try testing.expectEqual(@as(usize, 1), r.paths.len);
    try testing.expectEqualStrings("src/engine/a.zig", r.paths[0]);
}

test "classifyToken: a directory expands to its files rather than dropping (decision 906)" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(root);

    try tmp.dir.makePath("docs/sub");
    try tmp.dir.writeFile(.{ .sub_path = "docs/a.md", .data = "x" });
    try tmp.dir.writeFile(.{ .sub_path = "docs/b.md", .data = "x" });
    try tmp.dir.writeFile(.{ .sub_path = "docs/sub/c.md", .data = "x" });

    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const r = try classifyToken(arena.allocator(), root, "docs/");
    try testing.expectEqual(Classification.directory, r.classification);
    // Recursive: the nested file counts too. Wider, not narrower.
    try testing.expectEqual(@as(usize, 3), r.paths.len);
}

test "classifyToken: ambiguous basename yields EVERY match, never one guess (decision 906)" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(root);

    try tmp.dir.makePath("a/deep");
    try tmp.dir.makePath("b");
    try tmp.dir.writeFile(.{ .sub_path = "a/deep/task.zig", .data = "x" });
    try tmp.dir.writeFile(.{ .sub_path = "b/task.zig", .data = "x" });

    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const r = try classifyToken(arena.allocator(), root, "task.zig");
    try testing.expectEqual(Classification.basename, r.classification);
    try testing.expectEqual(@as(usize, 2), r.paths.len);
}

test "classifyToken: unplaceable token is reported, not silently dropped" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(root);

    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const r = try classifyToken(arena.allocator(), root, "src/nope/missing.zig");
    try testing.expectEqual(Classification.unresolved, r.classification);
    try testing.expectEqual(@as(usize, 0), r.paths.len);
    // Not writable — preview surfaces it, apply skips it.
    try testing.expect(!r.classification.isWritable());
}

test "classifyToken: hidden directories are never proposed" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(root);

    try tmp.dir.makePath("pkg/.git");
    try tmp.dir.writeFile(.{ .sub_path = "pkg/.git/config", .data = "x" });
    try tmp.dir.writeFile(.{ .sub_path = "pkg/real.zig", .data = "x" });

    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const r = try classifyToken(arena.allocator(), root, "pkg");
    try testing.expectEqual(Classification.directory, r.classification);
    try testing.expectEqual(@as(usize, 1), r.paths.len);
    try testing.expectEqualStrings("pkg/real.zig", r.paths[0]);
}

test "writable classifications are exactly those that produce rows" {
    try testing.expect(Classification.resolved.isWritable());
    try testing.expect(Classification.directory.isWritable());
    try testing.expect(Classification.basename.isWritable());
    try testing.expect(!Classification.unresolved.isWritable());
    try testing.expect(!Classification.too_broad.isWritable());
}
