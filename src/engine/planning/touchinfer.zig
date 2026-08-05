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
//! ## Proposing wide is not the same as writing wide
//!
//! That bias governs what is PROPOSED. What gets WRITTEN is narrower: only
//! `resolved` writes by default, because measurement showed the wide
//! classifications reduce parallel-eligibility rather than increasing it.
//! The reason is rule 2's drop-both-on-tie — an over-declared task removes
//! its PEERS from the eligible set as well as itself, while an undeclared
//! task removes only itself. See `wide_expansion_note` for the numbers.
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

    /// Whether a candidate of this classification contributes writable rows
    /// under the given policy. `unresolved` and `too_broad` are preview-only
    /// by construction, under either policy.
    ///
    /// `resolved` — an exact path the task named — always writes. The wide
    /// classifications (`directory`, `basename`) write only when the caller
    /// opts in, because measurement showed they cost eligibility rather than
    /// adding it. See `wide_expansion_note` below.
    pub fn isWritable(self: Classification, wide: bool) bool {
        return switch (self) {
            .resolved => true,
            .directory, .basename => wide,
            .unresolved, .too_broad => false,
        };
    }
};

/// Why the wide classifications are report-only by default.
///
/// Decision 906 biases *proposal* toward over-declaration, and that still
/// holds: a directory expands rather than being dropped, an ambiguous
/// basename yields every match, and nothing unplaceable is invented. What
/// changed is which proposals are WRITTEN, which is a separable question.
///
/// Measured 2026-08-05 over 46 open tasks in six real plans, applying rule 2
/// offline (empty set never eligible; intersecting sets drop BOTH):
///
///     nothing declared (baseline)   0 / 46 eligible
///     resolved-only                14 / 46
///     resolved + wide              13 / 46
///
/// Wide expansion bought zero additional eligible tasks and cost one. The
/// mechanism is rule 2's drop-both-on-tie: an UNDECLARED task removes only
/// itself from the eligible set, but an OVER-DECLARED one removes its peers
/// too. In plan 344, four tasks each mentioned `skills/src/` in prose;
/// expanding it gave all four the same 35 paths, so they mutually overlapped
/// and also dragged down the one task that had seven genuinely distinct real
/// paths. 1 eligible became 0.
///
/// So over-declaration is not the safely-recoverable direction once the
/// declaration is wide enough to intersect everything — the cost propagates
/// across the plan rather than staying with the declaring task.
pub const wide_expansion_note = {};

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

    /// Count of paths that would be written under the given policy.
    pub fn writableCount(self: Inference, wide: bool) usize {
        var n: usize = 0;
        for (self.candidates) |c| {
            if (c.classification.isWritable(wide)) n += c.paths.len;
        }
        return n;
    }

    /// Count of candidates surfaced for review but not written. Under the
    /// default policy this includes the wide classifications, which is the
    /// point: they stay visible, they just do not write.
    pub fn reviewCount(self: Inference, wide: bool) usize {
        var n: usize = 0;
        for (self.candidates) |c| {
            if (!c.classification.isWritable(wide)) n += 1;
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

/// Strip a trailing `:<line>` or `:<line>-<line>` reference. Task prose
/// cites locations as `docs/cli-reference.md:339` and
/// `src/engine/x.zig:12-40`; the path is the part before the colon. Without
/// this, the single most common way this codebase names a file resolves to
/// `unresolved` — a silent recall gap, which is the dangerous direction.
pub fn stripLineSuffix(s: []const u8) []const u8 {
    const colon = std.mem.lastIndexOfScalar(u8, s, ':') orelse return s;
    if (colon == 0 or colon + 1 >= s.len) return s;
    for (s[colon + 1 ..]) |c| {
        if (!std.ascii.isDigit(c) and c != '-') return s;
    }
    return s[0..colon];
}

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
        var trimmed = std.mem.trim(u8, raw, trim_chars);
        // A sentence-ending period rides along with a path at the end of a
        // clause ("Rework src/alpha.zig."). Strip it from the RIGHT only —
        // a leading dot is meaningful (`./x`), a trailing one never is,
        // since no path component ends in `.`. Without this the most
        // natural way to write a task body under-declares silently.
        while (trimmed.len > 0 and trimmed[trimmed.len - 1] == '.') {
            trimmed = trimmed[0 .. trimmed.len - 1];
        }
        const tok = stripLineSuffix(trimmed);
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
// Whole-task inference
// =========================================================================

/// The task text inference reads. Split out so the resolution pass is
/// testable without a database.
pub const TaskText = struct {
    title: []const u8 = "",
    body: []const u8 = "",
    next_action: []const u8 = "",
};

/// Resolve every path-shaped token in `text` against `root`, deduping
/// repeated tokens. Candidate order follows field order (title, body,
/// next_action) so the preview reads top-down like the task does.
pub fn inferFromText(
    a: std.mem.Allocator,
    text: TaskText,
    repo_id: i64,
    root: []const u8,
) std.mem.Allocator.Error![]const Candidate {
    var out: std.ArrayList(Candidate) = .empty;
    var seen: std.StringHashMapUnmanaged(void) = .empty;

    const fields = [_]struct { ev: Evidence, s: []const u8 }{
        .{ .ev = .title, .s = text.title },
        .{ .ev = .body, .s = text.body },
        .{ .ev = .next_action, .s = text.next_action },
    };

    for (fields) |f| {
        if (f.s.len == 0) continue;
        const toks = try extractTokens(a, f.s);
        for (toks) |tok| {
            // A token repeated across fields is one candidate, attributed to
            // the first field that produced it.
            if (seen.contains(tok)) continue;
            try seen.put(a, tok, {});

            const r = try classifyToken(a, root, tok);
            try out.append(a, .{
                .token = tok,
                .evidence = f.ev,
                .classification = r.classification,
                .paths = r.paths,
                .repo_id = repo_id,
            });
        }
    }

    return out.toOwnedSlice(a);
}

/// Read a task's text from the database and infer its touches against
/// `root` (the repo checkout for `repo_id`).
///
/// Proposes only — no `task_touch_paths` row is written here. The caller
/// previews, and writes on explicit confirmation.
pub fn infer(
    d: *db.sqlite.Db,
    gpa: std.mem.Allocator,
    task_id: i64,
    repo_id: i64,
    root: []const u8,
) Error!Inference {
    var arena = std.heap.ArenaAllocator.init(gpa);
    errdefer arena.deinit();
    const a = arena.allocator();

    var stmt = d.prepare(
        \\select title, coalesce(body, ''), coalesce(next_action, '')
        \\from tasks where id = ?
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;

    const text: TaskText = switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.NotFound,
        .row => .{
            .title = try stmt.columnTextAlloc(0, a),
            .body = try stmt.columnTextAlloc(1, a),
            .next_action = try stmt.columnTextAlloc(2, a),
        },
    };

    const candidates = try inferFromText(a, text, repo_id, root);
    return .{ .task_id = task_id, .candidates = candidates, .arena = arena };
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

test "stripLineSuffix handles the codebase's path:line citation style" {
    try testing.expectEqualStrings("docs/cli-reference.md", stripLineSuffix("docs/cli-reference.md:339"));
    try testing.expectEqualStrings("src/engine/x.zig", stripLineSuffix("src/engine/x.zig:12-40"));

    // Not a line reference — leave it alone.
    try testing.expectEqualStrings("docs/a.md", stripLineSuffix("docs/a.md"));
    try testing.expectEqualStrings("src/a.zig:name", stripLineSuffix("src/a.zig:name"));
    try testing.expectEqualStrings(":", stripLineSuffix(":"));
}

test "extractTokens recovers paths cited with line numbers" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();

    const toks = try extractTokens(a, "See CLAUDE.md:339 and src/engine/planning/strategy.zig:64-66.");
    try testing.expectEqual(@as(usize, 2), toks.len);
    try testing.expectEqualStrings("CLAUDE.md", toks[0]);
    try testing.expectEqualStrings("src/engine/planning/strategy.zig", toks[1]);
}

test "extractTokens recovers a path ending a sentence" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();

    // Regression: the trailing period rode along with the token and the
    // path resolved as unresolved — a silent under-declaration in the most
    // natural phrasing there is.
    const toks = try extractTokens(a, "Rework src/alpha.zig. Then check docs/b.md:12.");
    try testing.expectEqual(@as(usize, 2), toks.len);
    try testing.expectEqualStrings("src/alpha.zig", toks[0]);
    try testing.expectEqualStrings("docs/b.md", toks[1]);
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
    try testing.expect(!r.classification.isWritable(false));
    try testing.expect(!r.classification.isWritable(true));
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

test "inferFromText dedupes a token repeated across fields" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(root);

    try tmp.dir.makePath("src");
    try tmp.dir.writeFile(.{ .sub_path = "src/a.zig", .data = "x" });

    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const cands = try inferFromText(arena.allocator(), .{
        .title = "Fix src/a.zig",
        .body = "The bug is in src/a.zig somewhere.",
        .next_action = "Edit src/a.zig",
    }, 1, root);

    try testing.expectEqual(@as(usize, 1), cands.len);
    // Attributed to the FIRST field that produced it.
    try testing.expectEqual(Evidence.title, cands[0].evidence);
    try testing.expectEqual(Classification.resolved, cands[0].classification);
}

test "inferFromText carries unresolved tokens through for review" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try tmp.dir.realpathAlloc(testing.allocator, ".");
    defer testing.allocator.free(root);

    try tmp.dir.makePath("src");
    try tmp.dir.writeFile(.{ .sub_path = "src/real.zig", .data = "x" });

    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();

    const cands = try inferFromText(arena.allocator(), .{
        .body = "Touch src/real.zig and also src/imaginary.zig here.",
    }, 1, root);

    try testing.expectEqual(@as(usize, 2), cands.len);
    try testing.expectEqual(Classification.resolved, cands[0].classification);
    try testing.expectEqual(Classification.unresolved, cands[1].classification);

    // The unresolved one is surfaced but contributes no writable row.
    var inf = Inference{
        .task_id = 1,
        .candidates = cands,
        .arena = std.heap.ArenaAllocator.init(testing.allocator),
    };
    defer inf.deinit();
    try testing.expectEqual(@as(usize, 1), inf.writableCount(false));
    try testing.expectEqual(@as(usize, 1), inf.reviewCount(false));
}

test "writable classifications are exactly those that produce rows" {
    // Default policy: only an exact path match writes. Directory and
    // basename expansions are proposed and shown, but withheld — measured
    // to REDUCE parallel-eligibility, since a wide set intersects peers and
    // rule 2 drops both sides. See `wide_expansion_note`.
    try testing.expect(Classification.resolved.isWritable(false));
    try testing.expect(!Classification.directory.isWritable(false));
    try testing.expect(!Classification.basename.isWritable(false));
    try testing.expect(!Classification.unresolved.isWritable(false));
    try testing.expect(!Classification.too_broad.isWritable(false));

    // Opt-in policy: the wide classifications become writable; the two that
    // resolve to nothing never do, under either policy.
    try testing.expect(Classification.resolved.isWritable(true));
    try testing.expect(Classification.directory.isWritable(true));
    try testing.expect(Classification.basename.isWritable(true));
    try testing.expect(!Classification.unresolved.isWritable(true));
    try testing.expect(!Classification.too_broad.isWritable(true));
}
