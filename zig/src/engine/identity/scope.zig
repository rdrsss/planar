//! engine/identity/scope — cwd-derived scope resolution.
//!
//! "Scope" in Planar is the association or repo identity that an entity
//! belongs to. Most write verbs target a single scope; reads default to
//! "global" (no scope filter) unless one is requested.
//!
//! The cwd-derive algorithm:
//!   1. Find the project whose `root_path` is the longest prefix of cwd.
//!   2. Look up that project's association memberships.
//!   3. If exactly one membership, return its slug.
//!   4. If zero or many, return null (caller treats as global; ambiguity
//!      surfaces in `Reason` for human-readable diagnostics).
//!
//! This module owns the SQL; the CLI-side `cmd/planar/scope.zig`
//! wrapper applies the `--scope` override precedence and runs the
//! cross-scope guard.

const std = @import("std");
const db = @import("db");

/// A scope is an association slug, or null when the entity is global.
/// Slug strings are borrowed from the allocator passed to `derive`.
pub const Scope = ?[]const u8;

pub const Error = error{
    QueryFailed,
    InvalidPath,
    SlugNotFound,
    UnsupportedScope,
} || std.mem.Allocator.Error;

/// Why the resolver returned the scope it did. Useful for `planar scope
/// show` and for diagnostic messages on scope-guard refusals.
pub const Reason = enum {
    /// No project's root_path was a prefix of cwd.
    no_project_match,
    /// Matching project has zero association memberships.
    project_unassociated,
    /// Matching project has multiple memberships; ambiguous without
    /// an explicit `--scope` override.
    project_multiple_associations,
    /// Matching project has exactly one membership; that slug is used.
    project_single_association,
};

pub const Resolution = struct {
    scope: Scope,
    reason: Reason,
    project_slug: ?[]const u8 = null,
    /// True when `cwd` lives inside a secondary git worktree. Detected
    /// via a two-level check (fast path `.worktrees/<x>/` segment + an
    /// authoritative `git rev-parse --git-common-dir` fallback). When
    /// true, `scope` is still the parent repo's association: reads are
    /// transparent. Writes consult this flag at the runtime gate to
    /// refuse planning verbs from inside worktrees.
    cwd_is_worktree: bool = false,
    /// Absolute path to the worktree root. Owned by the allocator
    /// passed to `deriveFromCwd`. Populated only when `cwd_is_worktree`.
    worktree_root: ?[]const u8 = null,
    /// Absolute path to the parent repo (the canonical checkout the
    /// worktree was branched from). Owned by the allocator passed to
    /// `deriveFromCwd`. Populated only when `cwd_is_worktree`.
    parent_repo_root: ?[]const u8 = null,
};

/// Result of the worktree-detection probe. Caller owns any non-null
/// paths and frees them via `deinitWorktreeDetection`.
pub const WorktreeDetection = struct {
    is_worktree: bool,
    /// The worktree's working-tree root (the dir the operator was in).
    worktree_root: ?[]const u8 = null,
    /// The parent repo's working-tree root (sibling of `.git/`).
    parent_repo_root: ?[]const u8 = null,
};

pub fn deinitWorktreeDetection(allocator: std.mem.Allocator, d: WorktreeDetection) void {
    if (d.worktree_root) |s| allocator.free(s);
    if (d.parent_repo_root) |s| allocator.free(s);
}

/// ScopeRef is returned by resolveSlug: the resolved kind + database id.
pub const ScopeKind = enum {
    global,
    association,
    repo,
};

pub const ScopeRef = struct {
    kind: ScopeKind,
    /// Null for global scope; non-null for association/repo scope.
    id: ?i64,
};

/// Internal: a project row's id + slug. Returned by
/// `lookupProjectByPrefix` so the worktree-aware lookup can try
/// multiple paths without duplicating the SQL.
const ProjectMatch = struct { id: i64, slug: []const u8 };

/// Internal: find the project whose `root_path` is the longest prefix
/// of `path`. Returns null when no row matches. The returned `slug`
/// is owned by the caller.
fn lookupProjectByPrefix(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    path: []const u8,
) Error!?ProjectMatch {
    var stmt = d.prepare(
        "select id, slug, root_path from projects " ++
            "where root_path is not null order by length(root_path) desc",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{}) catch return Error.QueryFailed;

    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return null,
            .row => {
                const rp = try stmt.columnTextOpt(2, allocator);
                if (rp) |root| {
                    defer allocator.free(root);
                    if (pathHasPrefix(path, root)) {
                        const pid = stmt.columnInt(0);
                        const pslug = try stmt.columnTextAlloc(1, allocator);
                        return .{ .id = pid, .slug = pslug };
                    }
                }
            },
        }
    }
}

/// Derive the scope from `cwd`. Returns `.scope = null` plus a `reason`
/// explaining why when no unambiguous association can be selected.
/// Caller owns any non-null `scope`, `project_slug`, `worktree_root`,
/// and `parent_repo_root` strings.
///
/// When `cwd` lives inside a secondary git worktree (per the two-level
/// detection in `detectWorktree`), the association lookup is performed
/// against the *parent repo's* root, not the worktree's cwd, so reads
/// from inside a worktree transparently return the parent repo's
/// scope. The runtime gate uses `cwd_is_worktree` to refuse planning
/// verbs invoked from within a worktree.
///
/// Algorithm (mirrors Go's `scopearg.DeriveFromCwd` + `scopearg.ResolveForWrite`):
///   1. Probe for worktree-cwd via `detectWorktree`.
///   2. Load all projects with a non-null root_path, ordered longest-first.
///   3. Pick the first (longest) whose root_path is a prefix of (or equals)
///      the lookup path (parent repo root when in a worktree, otherwise cwd).
///   4. Query that project's association memberships.
///   5. Branch on count: 0 → project_unassociated, 1 → project_single_association,
///      2+ → project_multiple_associations.
pub fn deriveFromCwd(
    d: *db.sqlite.Db,
    io: std.Io,
    allocator: std.mem.Allocator,
    cwd: []const u8,
) Error!Resolution {
    if (cwd.len == 0 or cwd[0] != '/') return Error.InvalidPath;

    // Probe for worktree-cwd. Detection failures (git not installed,
    // not a git repo, etc.) degrade silently to "not a worktree" — the
    // caller stays on the normal cwd → project lookup path. This is
    // load-bearing for non-git fixtures (the harness's tmp dirs aren't
    // git repos by default).
    const det = detectWorktree(io, allocator, cwd) catch WorktreeDetection{ .is_worktree = false };
    errdefer deinitWorktreeDetection(allocator, det);

    // Project lookup. When in a worktree, the spec wants the parent
    // repo's association. But we also need to handle the case where
    // the cwd ITSELF is a registered project (e.g. integration test
    // fixtures that incidentally live under a `.worktrees/` path that
    // belongs to an unrelated repo). Strategy: try the literal cwd
    // FIRST; only fall back to parent_repo_root if no registered
    // project matches.
    //
    // The cwd-first ordering means a registered worktree-cwd "wins"
    // its own scope (rare, but matches operator expectation if they
    // explicitly registered the worktree). The fallback handles the
    // common case: cwd is unregistered, but the parent repo is.
    var match: ?ProjectMatch = try lookupProjectByPrefix(d, allocator, cwd);
    if (match == null and det.is_worktree) {
        if (det.parent_repo_root) |parent| {
            match = try lookupProjectByPrefix(d, allocator, parent);
        }
    }

    if (match == null) {
        return .{
            .scope = null,
            .reason = .no_project_match,
            .cwd_is_worktree = det.is_worktree,
            .worktree_root = det.worktree_root,
            .parent_repo_root = det.parent_repo_root,
        };
    }

    const pid = match.?.id;
    // pslug is now owned by this scope; we will transfer it to the returned
    // Resolution.project_slug. We must NOT free it inside this function.
    const pslug = match.?.slug;

    // Step 3: load all association slugs for this project.
    var assoc_slugs: std.ArrayList([]const u8) = .empty;
    // errdefer for the list contents only — pslug is a separate allocation.
    errdefer {
        for (assoc_slugs.items) |s| allocator.free(s);
        assoc_slugs.deinit(allocator);
    }

    {
        var stmt2 = d.prepare(
            \\select a.slug from associations a
            \\join project_associations pa on pa.association_id = a.id
            \\where pa.project_id = ?
            \\order by a.id
        ) catch {
            allocator.free(pslug);
            return Error.QueryFailed;
        };
        defer stmt2.finalize();
        stmt2.bind(&.{.{ .int = pid }}) catch {
            allocator.free(pslug);
            return Error.QueryFailed;
        };

        while (true) {
            switch (stmt2.step() catch {
                allocator.free(pslug);
                return Error.QueryFailed;
            }) {
                .done => break,
                .row => {
                    const s = try stmt2.columnTextAlloc(0, allocator);
                    try assoc_slugs.append(allocator, s);
                },
            }
        }
    }

    // Step 4: branch on count.
    // In all cases pslug is transferred out via project_slug — do NOT free it here.
    switch (assoc_slugs.items.len) {
        0 => {
            assoc_slugs.deinit(allocator);
            return .{
                .scope = null,
                .reason = .project_unassociated,
                .project_slug = pslug,
                .cwd_is_worktree = det.is_worktree,
                .worktree_root = det.worktree_root,
                .parent_repo_root = det.parent_repo_root,
            };
        },
        1 => {
            // Transfer ownership of the one slug; free the (now-empty-but-allocated)
            // list backing array.
            const scope_slug = assoc_slugs.items[0];
            assoc_slugs.deinit(allocator);
            return .{
                .scope = scope_slug,
                .reason = .project_single_association,
                .project_slug = pslug,
                .cwd_is_worktree = det.is_worktree,
                .worktree_root = det.worktree_root,
                .parent_repo_root = det.parent_repo_root,
            };
        },
        else => {
            // Multiple — free all association slugs, return null scope.
            for (assoc_slugs.items) |s| allocator.free(s);
            assoc_slugs.deinit(allocator);
            return .{
                .scope = null,
                .reason = .project_multiple_associations,
                .project_slug = pslug,
                .cwd_is_worktree = det.is_worktree,
                .worktree_root = det.worktree_root,
                .parent_repo_root = det.parent_repo_root,
            };
        },
    }
}

/// Detect whether `cwd` lives inside a secondary git worktree using a
/// two-level check:
///
/// 1. **Fast path (convention):** `cwd` contains a `.worktrees/<x>/`
///    path segment. Cheap; no subprocess. Matches the orchestrator's
///    convention from `agents/methodology.md`.
/// 2. **Authoritative fallback:** when the fast path missed, shell out
///    to `git rev-parse --path-format=absolute --show-toplevel
///    --git-common-dir --absolute-git-dir`. `--path-format=absolute`
///    (git >= 2.31) is required so `--git-common-dir` comes back
///    already-resolved — left in its default relative form it is
///    relative to the invocation cwd (NOT to `--show-toplevel`), and
///    naively joining it against `--show-toplevel` silently misresolves
///    for any cwd nested two or more levels below the repo root. The cwd
///    is a secondary worktree exactly when
///    the per-worktree git dir differs from the repository's common dir
///    (a linked worktree's git dir lives under `<common>/worktrees/<n>`).
///    A submodule checkout also has a common dir that is not
///    `<toplevel>/.git` (its gitfile points at the superproject's
///    `.git/modules/<path>`), but it is a PRIMARY checkout — its git
///    dir and common dir are the same directory — so comparing
///    git-dir vs common-dir classifies it correctly where the older
///    `common-dir != <toplevel>/.git` rule misfired.
///
/// On any failure (git not installed, cwd not in a git repo, subprocess
/// timeout) the function returns `is_worktree=false` — the caller stays
/// on the normal cwd → project path. Worktree detection is best-effort
/// enrichment, not a hard precondition.
///
/// Caller owns the returned path strings; release via
/// `deinitWorktreeDetection`.
pub fn detectWorktree(
    io: std.Io,
    allocator: std.mem.Allocator,
    cwd: []const u8,
) !WorktreeDetection {
    // ---- Fast path: scan for `.worktrees/<segment>/`. -------------
    if (fastPathWorktreeRoot(cwd)) |wt_root| {
        // `wt_root` is a slice into `cwd`; for the worktree path we
        // descend down to and including the segment immediately after
        // `.worktrees/`. The parent repo root is the directory just
        // above `.worktrees/`.
        const parent = parentOfDotWorktrees(wt_root);
        return .{
            .is_worktree = true,
            .worktree_root = try allocator.dupe(u8, wt_root),
            .parent_repo_root = try allocator.dupe(u8, parent),
        };
    }

    // ---- Authoritative fallback: shell to `git rev-parse`. --------
    // Capture both --git-common-dir and --show-toplevel in a single
    // invocation. Output is two lines on stdout. The cwd is a
    // secondary worktree when the common dir is NOT
    // `<toplevel>/.git`.
    var arena_state = std.heap.ArenaAllocator.init(allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const argv = [_][]const u8{
        "git",
        "-C",
        cwd,
        "rev-parse",
        "--path-format=absolute",
        "--show-toplevel",
        "--git-common-dir",
        "--absolute-git-dir",
    };

    const result = std.process.run(arena, io, .{ .argv = &argv }) catch
        return WorktreeDetection{ .is_worktree = false };

    switch (result.term) {
        .exited => |code| if (code != 0) return WorktreeDetection{ .is_worktree = false },
        else => return WorktreeDetection{ .is_worktree = false },
    }

    // Parse three lines.
    var lines = std.mem.tokenizeScalar(u8, result.stdout, '\n');
    const toplevel_raw = lines.next() orelse return WorktreeDetection{ .is_worktree = false };
    const common_dir_raw = lines.next() orelse return WorktreeDetection{ .is_worktree = false };
    const git_dir_raw = lines.next() orelse return WorktreeDetection{ .is_worktree = false };
    const toplevel = std.mem.trim(u8, toplevel_raw, " \r\n\t");
    const common_dir = std.mem.trim(u8, common_dir_raw, " \r\n\t");
    const git_dir = std.mem.trim(u8, git_dir_raw, " \r\n\t");
    if (toplevel.len == 0 or common_dir.len == 0 or git_dir.len == 0) return WorktreeDetection{ .is_worktree = false };

    // `--path-format=absolute` (git >= 2.31) makes `--git-common-dir`
    // return an already-canonical absolute path, so no join-against-a-
    // guessed-base step is needed here.
    //
    // Earlier revisions of this function asked for `--git-common-dir`
    // without `--path-format=absolute` and, when git returned it as a
    // path relative to the invocation cwd (which it does — NOT relative
    // to `--show-toplevel`), resolved the relative form against
    // `toplevel`. That silently produced a bogus, unresolved path like
    // `<toplevel>/../../../.git` whenever `cwd` was nested two or more
    // levels below `toplevel` (e.g. `<repo>/.zig-cache/tmp/<rand>`),
    // which then failed the git-dir/common-dir equality check below and
    // misclassified a perfectly ordinary primary checkout as a secondary
    // worktree — refusing planning verbs with a `parent:` path that
    // literally contained `..` segments. Letting git resolve the path
    // itself removes the depth assumption entirely.
    const common_dir_abs: []const u8 = common_dir;

    // A linked worktree is the ONLY case where the per-worktree git
    // dir differs from the repository's common dir: `--absolute-git-dir`
    // then points under `<common>/worktrees/<name>`. For every primary
    // checkout the two are the same directory — `<toplevel>/.git` for a
    // plain repo, and `<superproject>/.git/modules/<path>` for a
    // submodule checkout. The older rule compared common-dir against
    // `<toplevel>/.git`, which misclassified submodules as secondary
    // worktrees (their common dir is never `<toplevel>/.git`) and
    // pointed the refusal's remediation at `.git/modules/...`.
    // `--absolute-git-dir` is already canonical; `common_dir_abs` is
    // canonical too, courtesy of `--path-format=absolute` above.
    if (std.mem.eql(u8, git_dir, common_dir_abs)) {
        // git dir == common dir → primary checkout (plain repo or
        // submodule). Not a worktree.
        return WorktreeDetection{ .is_worktree = false };
    }

    // Secondary worktree. The parent repo root is the directory
    // containing the common-dir's `.git/` (i.e., common_dir's parent).
    const parent_repo_root = std.fs.path.dirname(common_dir_abs) orelse
        return WorktreeDetection{ .is_worktree = false };

    return .{
        .is_worktree = true,
        .worktree_root = try allocator.dupe(u8, toplevel),
        .parent_repo_root = try allocator.dupe(u8, parent_repo_root),
    };
}

/// Scan `cwd` for a `.worktrees/<segment>` boundary. Returns the slice
/// of `cwd` up to and including the segment immediately after
/// `.worktrees/`, treating that as the worktree root. Returns null if
/// there is no such segment, or if `.worktrees/` appears at the end
/// of the path with no segment following it.
fn fastPathWorktreeRoot(cwd: []const u8) ?[]const u8 {
    // Look for `/.worktrees/` anywhere in the path. The fast path needs
    // BOTH a leading slash (so we don't catch a directory literally
    // named `something.worktrees`) and a trailing slash (so we know a
    // segment follows).
    const needle = "/.worktrees/";
    const idx = std.mem.indexOf(u8, cwd, needle) orelse return null;
    const after = idx + needle.len;
    if (after >= cwd.len) return null; // `.worktrees/` with no segment
    // Find the next `/` after the worktree-id segment, or use end of
    // string. The slice we return covers up to AND including the
    // segment after `.worktrees/` (so for the orchestrator convention
    // path `/repo/.worktrees/<plan>/<task>/...`, we return
    // `/repo/.worktrees/<plan>/<task>`). The plan-segment vs task-
    // segment shape is encoded by the methodology, not by the
    // resolver; we don't need to know which is which here.
    //
    // For the methodology's two-level convention (`<plan>/<task>`),
    // the resolver still classifies the *cwd* as a worktree — and
    // that's all the runtime gate needs. The `worktree_root` we return
    // is the path up to the FIRST segment under `.worktrees/`, which
    // covers the hand-picked single-segment case too.
    const tail_start = after;
    const rel = cwd[tail_start..];
    const next_slash = std.mem.indexOfScalar(u8, rel, '/');
    const end = if (next_slash) |s| tail_start + s else cwd.len;
    return cwd[0..end];
}

/// Given a worktree-root path of the form `<parent>/.worktrees/<seg>`,
/// return the `<parent>` slice (the parent repo root).
fn parentOfDotWorktrees(wt_root: []const u8) []const u8 {
    // wt_root ends with `/.worktrees/<seg>`. Strip the trailing
    // `/.worktrees/<seg>` to recover `<parent>`.
    const marker = "/.worktrees/";
    const idx = std.mem.indexOf(u8, wt_root, marker) orelse return wt_root;
    return wt_root[0..idx];
}

/// Resolve a --scope flag value to a (kind, id) pair.
///
/// Accepted forms (mirrors Go's scopearg.Parse / scopearg.Resolve):
///   "global"         → {kind: .global,      id: null}
///   "<slug>"         → {kind: .association, id: <assoc_id>}
///   "assoc:<slug>"   → {kind: .association, id: <assoc_id>}
///   "repo:<slug>"    → {kind: .repo,        id: <project_id>}
///
/// Returns error.SlugNotFound when the association or repo slug does not exist.
pub fn resolveSlug(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    slug: []const u8,
) Error!ScopeRef {
    _ = allocator; // not needed for the lookup, but kept in signature for future use

    // "global" → global scope, no id.
    if (std.mem.eql(u8, slug, "global")) {
        return .{ .kind = .global, .id = null };
    }

    if (std.mem.startsWith(u8, slug, "repo:")) {
        const bare_repo = slug["repo:".len..];
        if (bare_repo.len == 0) return Error.SlugNotFound;
        return .{ .kind = .repo, .id = try projectIdBySlug(d, bare_repo) };
    }

    // Strip optional "assoc:" prefix.
    const bare = if (std.mem.startsWith(u8, slug, "assoc:"))
        slug["assoc:".len..]
    else
        slug;

    if (bare.len == 0) return Error.SlugNotFound;

    return .{ .kind = .association, .id = try associationIdBySlug(d, bare) };
}

/// Reverse of `resolveSlug`: given a stored scope_kind + scope_id,
/// return the slug. Returns null for `.global` (no slug per the rule
/// that global is the absence of scope). Returns SlugNotFound if the
/// id does not exist in the corresponding table.
///
/// Caller owns the returned slice; pass through `allocator.free` once
/// done. Used by the cross-scope write guard to convert an entity's
/// stored scope into a slug it can compare against the resolved
/// operator write scope.
pub fn slugFromRef(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    kind: ScopeKind,
    id: ?i64,
) Error!?[]const u8 {
    return switch (kind) {
        .global => null,
        .association => slugFromAssocId(d, allocator, id orelse return Error.SlugNotFound),
        .repo => slugFromProjectId(d, allocator, id orelse return Error.SlugNotFound),
    };
}

fn slugFromAssocId(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!?[]const u8 {
    var stmt = d.prepare("select slug from associations where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.SlugNotFound,
        .row => try stmt.columnTextAlloc(0, allocator),
    };
}

fn slugFromProjectId(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!?[]const u8 {
    var stmt = d.prepare("select slug from projects where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    const slug = switch (stmt.step() catch return Error.QueryFailed) {
        .done => return Error.SlugNotFound,
        .row => try stmt.columnTextAlloc(0, allocator),
    };
    defer allocator.free(slug);
    return try std.fmt.allocPrint(allocator, "repo:{s}", .{slug});
}

fn associationIdBySlug(d: *db.sqlite.Db, slug: []const u8) Error!i64 {
    var stmt = d.prepare("select id from associations where slug = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return Error.QueryFailed;

    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.SlugNotFound,
        .row => stmt.columnInt(0),
    };
}

fn projectIdBySlug(d: *db.sqlite.Db, slug: []const u8) Error!i64 {
    var stmt = d.prepare("select id from projects where slug = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = slug }}) catch return Error.QueryFailed;

    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.SlugNotFound,
        .row => stmt.columnInt(0),
    };
}

// =========================================================================
// Suggest (cwd-based association proposals from existing memberships)
// =========================================================================

/// One candidate association surfaced by `suggest`. Mirrors the Go side
/// `scope.ScopeProposal`. All string fields are owned by the allocator
/// passed to `suggest` — release via `deinitSuggestion` / `deinitSuggestions`.
pub const Suggestion = struct {
    slug: []const u8,
    association_id: i64,
    /// Human-readable explanation derived from `project_associations.source`.
    reason: []const u8,
};

pub fn deinitSuggestion(s: Suggestion, allocator: std.mem.Allocator) void {
    allocator.free(s.slug);
    allocator.free(s.reason);
}

pub fn deinitSuggestions(items: []const Suggestion, allocator: std.mem.Allocator) void {
    for (items) |s| deinitSuggestion(s, allocator);
    allocator.free(items);
}

/// Inspect `root_path` and return the associations the project at that
/// path is already a member of. Read-only: no rows are created. Returns
/// an empty slice when no project is registered at `root_path` or when
/// the project has no memberships.
///
/// Mirrors the Go side `scope.Suggest` semantics (membership-based;
/// returns proposals ordered by association slug). Note: the lookup
/// uses `root_path = ?` exact match. Sub-paths of a registered project
/// will NOT match here; the caller should pass the project's root.
pub fn suggest(d: *db.sqlite.Db, allocator: std.mem.Allocator, root_path: []const u8) Error![]Suggestion {
    var project_id: i64 = 0;
    {
        var stmt = d.prepare("select id from projects where root_path = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .text = root_path }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return try allocator.alloc(Suggestion, 0),
            .row => project_id = stmt.columnInt(0),
        }
    }

    var stmt2 = d.prepare(
        \\select a.id, a.slug, pa.source
        \\from project_associations pa
        \\join associations a on a.id = pa.association_id
        \\where pa.project_id = ?
        \\order by a.slug
    ) catch return Error.QueryFailed;
    defer stmt2.finalize();
    stmt2.bind(&.{.{ .int = project_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(Suggestion) = .empty;
    errdefer {
        for (out.items) |s| deinitSuggestion(s, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt2.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const aid = stmt2.columnInt(0);
                const slug = try stmt2.columnTextAlloc(1, allocator);
                errdefer allocator.free(slug);
                const source = try stmt2.columnTextAlloc(2, allocator);
                defer allocator.free(source);
                const reason = try allocator.dupe(u8, reasonFromSource(source));
                errdefer allocator.free(reason);
                try out.append(allocator, .{
                    .slug = slug,
                    .association_id = aid,
                    .reason = reason,
                });
            },
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Convert a `project_associations.source` value to the human-readable
/// label `planar scope suggest` prints.
pub fn reasonFromSource(source: []const u8) []const u8 {
    if (std.mem.eql(u8, source, "user")) return "explicit member";
    if (std.mem.eql(u8, source, "auto:git-remote")) return "from git remote";
    if (std.mem.eql(u8, source, "auto:path")) return "from parent directory";
    if (std.mem.eql(u8, source, "auto:lang")) return "from language ecosystem";
    return source;
}

// The cross-scope guard rule moved to `engine.policy.scope_guard.check`
// — every guarded data-plane mutation calls policy directly. Scope
// itself stays here because it's identity-layer vocabulary: the
// "what is a scope" type. Policy enforces rules ABOUT scopes.

// =========================================================================
// Internals
// =========================================================================

/// pathHasPrefix reports whether `target` equals `root` or starts with `root/`.
/// Both arguments must be clean paths (no trailing slash except the filesystem root).
fn pathHasPrefix(target: []const u8, root: []const u8) bool {
    if (std.mem.eql(u8, target, root)) return true;
    if (!std.mem.startsWith(u8, target, root)) return false;
    if (target.len <= root.len) return false;
    return target[root.len] == '/';
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "deriveFromCwd rejects relative paths" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.InvalidPath, deriveFromCwd(&d, std.testing.io, a, "relative"));
}

test "deriveFromCwd on empty DB reports no project match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const res = try deriveFromCwd(&d, std.testing.io, a, "/some/path");
    try std.testing.expect(res.scope == null);
    try std.testing.expectEqual(Reason.no_project_match, res.reason);
}

test "deriveFromCwd: project + 1 association → slug + reason=project_single_association" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('myrepo', 'My Repo', '/work/myrepo')",
        &.{},
    );
    const project_id = try d.intQuery("select id from projects where slug = 'myrepo'");
    _ = try d.execParams(
        "insert into associations (slug, name, kind) values ('myorg', 'My Org', 'org')",
        &.{},
    );
    const assoc_id = try d.intQuery("select id from associations where slug = 'myorg'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = project_id }, .{ .int = assoc_id } },
    );

    const res = try deriveFromCwd(&d, std.testing.io, a, "/work/myrepo/src/foo");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);
    defer if (res.worktree_root) |s| a.free(s);
    defer if (res.parent_repo_root) |s| a.free(s);

    try std.testing.expectEqual(Reason.project_single_association, res.reason);
    try std.testing.expect(res.scope != null);
    try std.testing.expectEqualStrings("myorg", res.scope.?);
    try std.testing.expectEqualStrings("myrepo", res.project_slug.?);
}

test "deriveFromCwd: project + 0 associations → null + reason=project_unassociated" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('lone', 'Lone', '/work/lone')",
        &.{},
    );

    const res = try deriveFromCwd(&d, std.testing.io, a, "/work/lone/src");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);
    defer if (res.worktree_root) |s| a.free(s);
    defer if (res.parent_repo_root) |s| a.free(s);

    try std.testing.expectEqual(Reason.project_unassociated, res.reason);
    try std.testing.expect(res.scope == null);
    try std.testing.expectEqualStrings("lone", res.project_slug.?);
}

test "deriveFromCwd: project + 2 associations → null + reason=project_multiple_associations" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('multi', 'Multi', '/work/multi')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 'multi'");

    _ = try d.execParams("insert into associations (slug, name, kind) values ('a1', 'A1', 'org')", &.{});
    const aid1 = try d.intQuery("select id from associations where slug = 'a1'");
    _ = try d.execParams("insert into associations (slug, name, kind) values ('a2', 'A2', 'org')", &.{});
    const aid2 = try d.intQuery("select id from associations where slug = 'a2'");

    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid1 } },
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid2 } },
    );

    const res = try deriveFromCwd(&d, std.testing.io, a, "/work/multi/lib");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);
    defer if (res.worktree_root) |s| a.free(s);
    defer if (res.parent_repo_root) |s| a.free(s);

    try std.testing.expectEqual(Reason.project_multiple_associations, res.reason);
    try std.testing.expect(res.scope == null);
    try std.testing.expect(res.project_slug != null);
    try std.testing.expectEqualStrings("multi", res.project_slug.?);
}

test "deriveFromCwd: no project prefix match → null + reason=no_project_match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('other', 'Other', '/work/other')",
        &.{},
    );

    const res = try deriveFromCwd(&d, std.testing.io, a, "/home/user/unrelated");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);
    defer if (res.worktree_root) |s| a.free(s);
    defer if (res.parent_repo_root) |s| a.free(s);

    try std.testing.expectEqual(Reason.no_project_match, res.reason);
    try std.testing.expect(res.scope == null);
}

test "deriveFromCwd: longest-prefix wins when multiple projects match" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('parent', 'Parent', '/work')",
        &.{},
    );
    const parent_id = try d.intQuery("select id from projects where slug = 'parent'");

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('child', 'Child', '/work/child')",
        &.{},
    );
    const child_id = try d.intQuery("select id from projects where slug = 'child'");

    _ = try d.execParams("insert into associations (slug, name, kind) values ('parent-org', 'PO', 'org')", &.{});
    const parent_assoc = try d.intQuery("select id from associations where slug = 'parent-org'");
    _ = try d.execParams("insert into associations (slug, name, kind) values ('child-org', 'CO', 'org')", &.{});
    const child_assoc = try d.intQuery("select id from associations where slug = 'child-org'");

    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = parent_id }, .{ .int = parent_assoc } },
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = child_id }, .{ .int = child_assoc } },
    );

    const res = try deriveFromCwd(&d, std.testing.io, a, "/work/child/src");
    defer if (res.scope) |s| a.free(s);
    defer if (res.project_slug) |s| a.free(s);
    defer if (res.worktree_root) |s| a.free(s);
    defer if (res.parent_repo_root) |s| a.free(s);

    // The child project (/work/child) is a longer prefix than parent (/work).
    try std.testing.expectEqual(Reason.project_single_association, res.reason);
    try std.testing.expect(res.scope != null);
    try std.testing.expectEqualStrings("child-org", res.scope.?);
    try std.testing.expectEqualStrings("child", res.project_slug.?);
}

test "resolveSlug: 'global' → kind=.global, id=null" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const ref = try resolveSlug(&d, a, "global");
    try std.testing.expectEqual(ScopeKind.global, ref.kind);
    try std.testing.expect(ref.id == null);
}

test "resolveSlug: known association slug → kind=.association, id=<assoc_id>" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'acme'");

    const ref = try resolveSlug(&d, a, "acme");
    try std.testing.expectEqual(ScopeKind.association, ref.kind);
    try std.testing.expectEqual(assoc_id, ref.id.?);
}

test "resolveSlug: 'assoc:<slug>' prefix form → same as bare slug" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams("insert into associations (slug, name, kind) values ('myassoc', 'MA', 'org')", &.{});
    const assoc_id = try d.intQuery("select id from associations where slug = 'myassoc'");

    const bare_ref = try resolveSlug(&d, a, "myassoc");
    const prefixed_ref = try resolveSlug(&d, a, "assoc:myassoc");

    try std.testing.expectEqual(ScopeKind.association, bare_ref.kind);
    try std.testing.expectEqual(ScopeKind.association, prefixed_ref.kind);
    try std.testing.expectEqual(assoc_id, bare_ref.id.?);
    try std.testing.expectEqual(assoc_id, prefixed_ref.id.?);
}

test "resolveSlug: unknown slug → error.SlugNotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(Error.SlugNotFound, resolveSlug(&d, a, "no-such-slug"));
}

test "resolveSlug: 'repo:<slug>' prefix form → kind=.repo, id=<project_id>" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('myrepo', 'My Repo', '/work/myrepo')",
        &.{},
    );
    const project_id = try d.intQuery("select id from projects where slug = 'myrepo'");

    const ref = try resolveSlug(&d, a, "repo:myrepo");
    try std.testing.expectEqual(ScopeKind.repo, ref.kind);
    try std.testing.expectEqual(project_id, ref.id.?);
}

test "slugFromRef returns prefixed repo scope labels" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('myrepo', 'My Repo', '/work/myrepo')",
        &.{},
    );
    const project_id = try d.intQuery("select id from projects where slug = 'myrepo'");

    const slug = (try slugFromRef(&d, a, .repo, project_id)).?;
    defer a.free(slug);
    try std.testing.expectEqualStrings("repo:myrepo", slug);
}

test "suggest: empty on unregistered cwd" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const items = try suggest(&d, a, "/unregistered");
    defer deinitSuggestions(items, a);
    try std.testing.expectEqual(@as(usize, 0), items.len);
}

test "suggest: returns the memberships of the project at root_path" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('s', 'S', '/work/s')",
        &.{},
    );
    const pid = try d.intQuery("select id from projects where slug = 's'");
    _ = try d.execParams("insert into associations (slug, name, kind) values ('alpha', 'A', 'org')", &.{});
    const aid1 = try d.intQuery("select id from associations where slug = 'alpha'");
    _ = try d.execParams("insert into associations (slug, name, kind) values ('beta', 'B', 'host')", &.{});
    const aid2 = try d.intQuery("select id from associations where slug = 'beta'");
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'user')",
        &.{ .{ .int = pid }, .{ .int = aid1 } },
    );
    _ = try d.execParams(
        "insert into project_associations (project_id, association_id, source) values (?, ?, 'auto:git-remote')",
        &.{ .{ .int = pid }, .{ .int = aid2 } },
    );

    const items = try suggest(&d, a, "/work/s");
    defer deinitSuggestions(items, a);

    try std.testing.expectEqual(@as(usize, 2), items.len);
    // Ordered by slug → alpha first.
    try std.testing.expectEqualStrings("alpha", items[0].slug);
    try std.testing.expectEqualStrings("explicit member", items[0].reason);
    try std.testing.expectEqualStrings("beta", items[1].slug);
    try std.testing.expectEqualStrings("from git remote", items[1].reason);
}

test "suggest: project with no memberships returns empty" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    _ = try d.execParams(
        "insert into projects (slug, name, root_path) values ('lonely', 'L', '/work/lonely')",
        &.{},
    );
    const items = try suggest(&d, a, "/work/lonely");
    defer deinitSuggestions(items, a);
    try std.testing.expectEqual(@as(usize, 0), items.len);
}

test "reasonFromSource: known + unknown values" {
    try std.testing.expectEqualStrings("explicit member", reasonFromSource("user"));
    try std.testing.expectEqualStrings("from git remote", reasonFromSource("auto:git-remote"));
    try std.testing.expectEqualStrings("from parent directory", reasonFromSource("auto:path"));
    try std.testing.expectEqualStrings("from language ecosystem", reasonFromSource("auto:lang"));
    // Unknown sources pass through.
    try std.testing.expectEqualStrings("auto:custom", reasonFromSource("auto:custom"));
}

// =========================================================================
// Worktree-detection tests
// =========================================================================

test "detectWorktree fast path: matches .worktrees/<seg> segment" {
    const a = std.testing.allocator;
    const cwd = "/work/repo/.worktrees/feature-x/src/foo";
    const det = try detectWorktree(std.testing.io, a, cwd);
    defer deinitWorktreeDetection(a, det);
    try std.testing.expect(det.is_worktree);
    try std.testing.expectEqualStrings("/work/repo/.worktrees/feature-x", det.worktree_root.?);
    try std.testing.expectEqualStrings("/work/repo", det.parent_repo_root.?);
}

test "detectWorktree fast path: orchestrator <plan>/<task> shape" {
    const a = std.testing.allocator;
    const cwd = "/work/repo/.worktrees/p297/m3-engine-scope";
    const det = try detectWorktree(std.testing.io, a, cwd);
    defer deinitWorktreeDetection(a, det);
    try std.testing.expect(det.is_worktree);
    // We return up to the FIRST segment under .worktrees/ — the plan slug.
    try std.testing.expectEqualStrings("/work/repo/.worktrees/p297", det.worktree_root.?);
    try std.testing.expectEqualStrings("/work/repo", det.parent_repo_root.?);
}

test "detectWorktree fast path: ignores .worktrees with no segment" {
    const a = std.testing.allocator;
    // Trailing .worktrees/ with nothing after → no match.
    const det = try detectWorktree(std.testing.io, a, "/work/repo/.worktrees/");
    defer deinitWorktreeDetection(a, det);
    // Fast path bails, then we shell to git (this path likely isn't a
    // repo). Either way: not classified as worktree.
    try std.testing.expect(!det.is_worktree);
}

test "fastPathWorktreeRoot: misses dirs literally named foo.worktrees" {
    // Without the leading slash sentinel the function would incorrectly
    // classify `/work/myrepo.worktrees/x` as a worktree.
    try std.testing.expect(fastPathWorktreeRoot("/work/myrepo.worktrees/x") == null);
}

test "fastPathWorktreeRoot: returns parent path before .worktrees" {
    const got = fastPathWorktreeRoot("/work/repo/.worktrees/foo/bar/baz").?;
    try std.testing.expectEqualStrings("/work/repo/.worktrees/foo", got);
}

test "parentOfDotWorktrees: strips trailing /.worktrees/<seg>" {
    try std.testing.expectEqualStrings(
        "/work/repo",
        parentOfDotWorktrees("/work/repo/.worktrees/foo"),
    );
}

test "detectWorktree: non-worktree cwd in non-git path returns is_worktree=false" {
    const a = std.testing.allocator;
    // /tmp is not under any .worktrees segment and probably not a git
    // repo. The fast path misses; the authoritative fallback returns
    // an error from git, which we degrade to not-a-worktree.
    const det = try detectWorktree(std.testing.io, a, "/tmp");
    defer deinitWorktreeDetection(a, det);
    try std.testing.expect(!det.is_worktree);
}

// Authoritative-fallback test that spawns real `git`. Skipped if git
// is not on PATH. Builds a tmp dir with a primary repo and a
// secondary worktree at a path that does NOT contain `.worktrees/`,
// so only the fallback can detect it.
test "detectWorktree authoritative fallback: secondary worktree at non-convention path" {
    const a = std.testing.allocator;

    // Skip if git is unavailable.
    {
        const probe = std.process.run(a, std.testing.io, .{
            .argv = &.{ "git", "--version" },
        }) catch return error.SkipZigTest;
        defer a.free(probe.stdout);
        defer a.free(probe.stderr);
        switch (probe.term) {
            .exited => |code| if (code != 0) return error.SkipZigTest,
            else => return error.SkipZigTest,
        }
    }

    // Use mktemp instead of std.testing.tmpDir so the test path is
    // under /var/folders (or /tmp on Linux), not under
    // .zig-cache/tmp/. The Planar dev tree lives at
    // .../.worktrees/cycle/...; a .zig-cache tmp under it would
    // trigger the fast-path before the authoritative fallback ever
    // runs.
    const mk = try std.process.run(a, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", "planar-scope-test.XXXXXX" },
    });
    defer a.free(mk.stderr);
    switch (mk.term) {
        .exited => |code| if (code != 0) {
            a.free(mk.stdout);
            return error.MktempFailed;
        },
        else => {
            a.free(mk.stdout);
            return error.MktempFailed;
        },
    }
    const tmp_owned = try a.dupe(u8, std.mem.trim(u8, mk.stdout, " \r\n\t"));
    a.free(mk.stdout);
    defer a.free(tmp_owned);
    defer cleanupTmpDir(a, tmp_owned);

    const primary = try std.fs.path.join(a, &.{ tmp_owned, "primary" });
    defer a.free(primary);
    const secondary = try std.fs.path.join(a, &.{ tmp_owned, "secondary-wt" });
    defer a.free(secondary);

    // Init primary repo and a starter commit.
    try runGit(a, tmp_owned, &.{ "init", "primary" });
    try runGit(a, primary, &.{ "config", "user.email", "test@example.com" });
    try runGit(a, primary, &.{ "config", "user.name", "Test" });
    try runGit(a, primary, &.{ "commit", "--allow-empty", "-m", "init" });
    // Create a secondary worktree at a non-`.worktrees/` path.
    try runGit(a, primary, &.{ "worktree", "add", "-b", "feat-branch", secondary });

    // Detect from inside the secondary.
    const det = try detectWorktree(std.testing.io, a, secondary);
    defer deinitWorktreeDetection(a, det);
    try std.testing.expect(det.is_worktree);
    try std.testing.expect(det.parent_repo_root != null);

    // Main checkout: should NOT classify as worktree.
    const main_det = try detectWorktree(std.testing.io, a, primary);
    defer deinitWorktreeDetection(a, main_det);
    try std.testing.expect(!main_det.is_worktree);
}

// Regression test for the M0 zig/-relocation-cycle bug (commit 6423d9b,
// docs/toolchain-parity.md's git-floor row): a PRIMARY checkout whose cwd is
// nested two or more levels below the repo root (e.g. a build-output dir
// like `.zig-cache/tmp/<rand>`) must still classify as NOT a worktree.
// Before `--path-format=absolute` was added to the `--git-common-dir`
// fallback query, git returned that path relative to the invocation cwd
// (not to `--show-toplevel`), so joining it against `toplevel` produced a
// bogus `<toplevel>/../../../.git`-shaped path whenever cwd was nested >=2
// levels deep — misclassifying an ordinary primary checkout as a secondary
// worktree and refusing planning verbs. This pins the fix by exercising the
// authoritative git fallback (no `.worktrees/` segment in the nested path,
// so the fast path cannot short-circuit it) from a real two-level-nested
// cwd inside the primary checkout.
test "detectWorktree authoritative fallback: primary checkout nested >=2 levels below root is not a worktree" {
    const a = std.testing.allocator;

    // Skip if git is unavailable.
    {
        const probe = std.process.run(a, std.testing.io, .{
            .argv = &.{ "git", "--version" },
        }) catch return error.SkipZigTest;
        defer a.free(probe.stdout);
        defer a.free(probe.stderr);
        switch (probe.term) {
            .exited => |code| if (code != 0) return error.SkipZigTest,
            else => return error.SkipZigTest,
        }
    }

    const mk = try std.process.run(a, std.testing.io, .{
        .argv = &.{ "mktemp", "-d", "-t", "planar-scope-nested-test.XXXXXX" },
    });
    defer a.free(mk.stderr);
    switch (mk.term) {
        .exited => |code| if (code != 0) {
            a.free(mk.stdout);
            return error.MktempFailed;
        },
        else => {
            a.free(mk.stdout);
            return error.MktempFailed;
        },
    }
    const tmp_owned = try a.dupe(u8, std.mem.trim(u8, mk.stdout, " \r\n\t"));
    a.free(mk.stdout);
    defer a.free(tmp_owned);
    defer cleanupTmpDir(a, tmp_owned);

    const primary = try std.fs.path.join(a, &.{ tmp_owned, "primary" });
    defer a.free(primary);
    // Two levels below the repo root, deliberately without a `.worktrees/`
    // segment so only the authoritative git fallback can classify it.
    const nested_cwd = try std.fs.path.join(a, &.{ primary, "build-output", "tmp" });
    defer a.free(nested_cwd);

    try runGit(a, tmp_owned, &.{ "init", "primary" });
    try runGit(a, primary, &.{ "config", "user.email", "test@example.com" });
    try runGit(a, primary, &.{ "config", "user.name", "Test" });
    try runGit(a, primary, &.{ "commit", "--allow-empty", "-m", "init" });
    try std.Io.Dir.cwd().createDirPath(std.testing.io, nested_cwd);

    const det = try detectWorktree(std.testing.io, a, nested_cwd);
    defer deinitWorktreeDetection(a, det);
    try std.testing.expect(!det.is_worktree);
}

fn cleanupTmpDir(allocator: std.mem.Allocator, dir: []const u8) void {
    const rm = std.process.run(allocator, std.testing.io, .{
        .argv = &.{ "rm", "-rf", dir },
    }) catch return;
    allocator.free(rm.stdout);
    allocator.free(rm.stderr);
}

fn runGit(allocator: std.mem.Allocator, cwd: []const u8, args: []const []const u8) !void {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(allocator);
    try argv.append(allocator, "git");
    try argv.append(allocator, "-C");
    try argv.append(allocator, cwd);
    for (args) |arg| try argv.append(allocator, arg);

    const result = try std.process.run(allocator, std.testing.io, .{ .argv = argv.items });
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| if (code != 0) return error.GitFailed,
        else => return error.GitFailed,
    }
}
