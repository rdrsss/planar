//! cmd/planar/scope — handler-facing scope resolution + cross-scope guard.
//!
//! Engine module `engine.identity.scope` owns the cwd-derive SQL and
//! the guard rule itself. This wrapper layers in CLI concerns:
//!
//!   1. `--scope <slug>` override precedence (handler flag beats cwd).
//!   2. cwd acquisition via `std.Io.Dir.realPathFileAlloc(.cwd(), …)`,
//!      since handlers shouldn't reach for IO themselves.
//!   3. Forwarding to `runtime.ensureDb` so help-only paths still
//!      avoid touching the DB.
//!
//! Usage in a write handler:
//!
//!   const ctx = runtime.current();
//!   const write_scope = try scope_mod.resolve(ctx, args.scope);
//!   const entity = try engine.planning.task.show(...);
//!   try scope_mod.guard(entity.scope, write_scope);
//!   try engine.planning.task.update(...);

const std = @import("std");
const engine = @import("engine");
const runtime = @import("runtime");
const exit = @import("exit.zig");

pub const Scope = engine.identity.scope.Scope;
pub const Resolution = engine.identity.scope.Resolution;
pub const Reason = engine.identity.scope.Reason;
pub const ReadScope = struct {
    kind: engine.identity.scope.ScopeKind,
    id: i64 = 0,
};

const Candidate = struct {
    id: i64,
    kind: []const u8,
    root_len: usize,
};

/// Resolve the active scope for this handler call. `override` is the
/// parsed `--scope` flag value (null if not set). Returns the engine
/// resolution unchanged so callers can inspect the reason — handlers
/// that only care about the slug just read `.scope`.
///
/// Note: even when `override` is set the wrapper still probes for
/// worktree-cwd. The runtime worktree gate uses the resolution's
/// `cwd_is_worktree` flag to refuse planning verbs from inside a
/// worktree, and per the spec `--scope` does NOT override that rule.
pub fn resolve(ctx: *const runtime.Ctx, override: ?[]const u8) !Resolution {
    if (override) |slug| {
        // Probe for worktree-cwd so the runtime gate sees it. Failures
        // degrade silently to "not a worktree" — same posture as the
        // engine resolver.
        const cwd = try operatorCwd(ctx.allocator, ctx.io);
        defer ctx.allocator.free(cwd);
        const det = engine.identity.scope.detectWorktree(ctx.io, ctx.allocator, cwd) catch
            engine.identity.scope.WorktreeDetection{ .is_worktree = false };
        return .{
            .scope = slug,
            .reason = .project_single_association,
            .project_slug = null,
            .cwd_is_worktree = det.is_worktree,
            .worktree_root = det.worktree_root,
            .parent_repo_root = det.parent_repo_root,
        };
    }
    // PWD-first cwd acquisition (task 2375): must use the same
    // canonicalization-avoiding path resolution as `init` so that the
    // projects-table key the operator's `assoc add <slug> <path>` wrote
    // matches the key cwd-derive looks up. realPath would canonicalize
    // through macOS's /var → /private/var symlink and miss the binding.
    const cwd = try operatorCwd(ctx.allocator, ctx.io);
    defer ctx.allocator.free(cwd);

    const d = try runtime.ensureDb();
    return try engine.identity.scope.deriveFromCwd(d, ctx.io, ctx.allocator, cwd);
}

/// Resolve scope for a mutating planning verb.
///
/// Reads stay workspace-shaped, but writes inside a meta workspace must land on
/// a concrete repo unless the operator explicitly selects the workspace org.
/// The exact meta root is both the org root and the root repo path, so no
/// default is safe there.
pub fn resolveForWrite(ctx: *const runtime.Ctx, override: ?[]const u8) !Resolution {
    if (override != null) return try resolve(ctx, override);

    const cwd = try operatorCwd(ctx.allocator, ctx.io);
    defer ctx.allocator.free(cwd);

    const d = try runtime.ensureDb();
    const meta = try resolveMetaWorkspaceWriteScope(ctx, d, cwd);
    defer deinitMetaWriteResolution(ctx.allocator, meta);
    switch (meta) {
        .none => return try resolve(ctx, null),
        .repo_scope => |scope| {
            const det = engine.identity.scope.detectWorktree(ctx.io, ctx.allocator, cwd) catch
                engine.identity.scope.WorktreeDetection{ .is_worktree = false };
            return .{
                .scope = try ctx.allocator.dupe(u8, scope),
                .reason = .project_single_association,
                .project_slug = null,
                .cwd_is_worktree = det.is_worktree,
                .worktree_root = det.worktree_root,
                .parent_repo_root = det.parent_repo_root,
            };
        },
        .ambiguous => |choices| {
            exit.die(
                ctx,
                error.ScopeMismatch,
                "ambiguous meta workspace root {s}: choose `--scope {s}` for cross-repo/meta-level work or `--scope {s}` for root-repo work",
                .{ cwd, choices.assoc_scope, choices.repo_scope },
            );
        },
    }
}

/// PWD-first cwd resolution (matches Go's os.Getwd). Falls back to
/// realPath when PWD is absent. See plan 351 task 2375 for rationale.
pub fn operatorCwd(allocator: std.mem.Allocator, io: std.Io) ![]const u8 {
    const real_cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), io, ".", allocator);
    errdefer allocator.free(real_cwd);
    if (getPosixEnv("PWD")) |pwd| {
        if (std.fs.path.isAbsolute(pwd)) {
            const real_pwd = std.Io.Dir.realPathFileAlloc(.cwd(), io, pwd, allocator) catch null;
            defer if (real_pwd) |p| allocator.free(p);
            if (real_pwd) |p| {
                if (std.mem.eql(u8, p, real_cwd)) {
                    allocator.free(real_cwd);
                    return try allocator.dupe(u8, pwd);
                }
            }
        }
    }
    return real_cwd;
}

/// Resolve the cwd-derived read set. An explicit override returns that single
/// parsed scope; otherwise cwd drives the result set. An empty slice means the
/// caller should refuse rather than falling back to unscoped global reads.
pub fn resolveForReadSet(
    ctx: *const runtime.Ctx,
    cwd: []const u8,
    override: ?[]const u8,
) ![]ReadScope {
    const d = try runtime.ensureDb();
    if (override) |raw_scope| {
        const ref = engine.identity.scope.resolveSlug(d, ctx.allocator, raw_scope) catch |e| switch (e) {
            error.SlugNotFound => {
                try ctx.stderr.print("error: resolving scope: scope slug not found: {s}\n", .{raw_scope});
                return e;
            },
            else => return e,
        };
        const out = try ctx.allocator.alloc(ReadScope, 1);
        out[0] = .{ .kind = ref.kind, .id = ref.id orelse 0 };
        return out;
    }

    if (try resolveMetaWorkspaceReadSet(ctx, d, cwd)) |resolved| {
        return resolved;
    }

    const candidates = try deriveCandidates(ctx, d, cwd);
    defer {
        for (candidates) |c| ctx.allocator.free(c.kind);
        ctx.allocator.free(candidates);
    }
    if (candidates.len == 0) return try ctx.allocator.alloc(ReadScope, 0);

    var best_rank = specificityRank(candidates[0].kind);
    var best_root_len = candidates[0].root_len;
    for (candidates[1..]) |c| {
        const r = specificityRank(c.kind);
        if (r < best_rank) {
            best_rank = r;
            best_root_len = c.root_len;
        } else if (r == best_rank and c.root_len > best_root_len) {
            best_root_len = c.root_len;
        }
    }
    var top_count: usize = 0;
    var winner: Candidate = candidates[0];
    for (candidates) |c| {
        if (specificityRank(c.kind) == best_rank and c.root_len == best_root_len) {
            top_count += 1;
            winner = c;
        }
    }
    if (top_count != 1) return try ctx.allocator.alloc(ReadScope, 0);

    if (std.mem.eql(u8, winner.kind, "repo")) {
        const out = try ctx.allocator.alloc(ReadScope, 1);
        out[0] = .{ .kind = .repo, .id = winner.id };
        return out;
    }
    if (std.mem.eql(u8, winner.kind, "org")) {
        return try expandWorkspaceReadSet(ctx, d, winner.id);
    }
    const out = try ctx.allocator.alloc(ReadScope, 1);
    out[0] = .{ .kind = .association, .id = winner.id };
    return out;
}

/// Convert resolved read scopes to `--scope` filter strings accepted by the
/// planning engine list filters. Caller owns both the slice and each string.
pub fn readScopeFilterSlugs(ctx: *const runtime.Ctx, read_scopes: []const ReadScope) ![][]const u8 {
    const d = try runtime.ensureDb();
    var out = try ctx.allocator.alloc([]const u8, read_scopes.len);
    errdefer {
        for (out[0..]) |s| if (s.len > 0) ctx.allocator.free(s);
        ctx.allocator.free(out);
    }
    for (out) |*s| s.* = "";
    for (read_scopes, 0..) |row, i| {
        out[i] = switch (row.kind) {
            .global => try ctx.allocator.dupe(u8, "global"),
            .association => blk: {
                var stmt = try d.prepare("select coalesce(slug,'') from associations where id = ?");
                defer stmt.finalize();
                try stmt.bind(&.{.{ .int = row.id }});
                if ((try stmt.step()) != .row) return error.SlugNotFound;
                const slug = try stmt.columnTextAlloc(0, ctx.allocator);
                defer ctx.allocator.free(slug);
                break :blk try std.fmt.allocPrint(ctx.allocator, "assoc:{s}", .{slug});
            },
            .repo => blk: {
                var stmt = try d.prepare("select coalesce(slug,'') from projects where id = ?");
                defer stmt.finalize();
                try stmt.bind(&.{.{ .int = row.id }});
                if ((try stmt.step()) != .row) return error.SlugNotFound;
                const slug = try stmt.columnTextAlloc(0, ctx.allocator);
                defer ctx.allocator.free(slug);
                break :blk try std.fmt.allocPrint(ctx.allocator, "repo:{s}", .{slug});
            },
        };
    }
    return out;
}

pub fn deinitReadScopeFilterSlugs(allocator: std.mem.Allocator, slugs: []const []const u8) void {
    for (slugs) |s| allocator.free(s);
    allocator.free(slugs);
}

fn resolveMetaWorkspaceReadSet(
    ctx: *const runtime.Ctx,
    d: anytype,
    cwd: []const u8,
) !?[]ReadScope {
    var stmt = try d.prepare(
        \\select p.id, p.root_path, json_extract(a.config_json, '$.root_path'), a.id
        \\from projects p
        \\join project_associations pa on pa.project_id = p.id
        \\join associations a on a.id = pa.association_id
        \\where a.kind = 'org'
        \\  and p.root_path is not null
        \\  and json_extract(a.config_json, '$.workspace_shape') = 'meta-repo'
        \\  and json_extract(a.config_json, '$.root_path') is not null
        \\order by length(p.root_path) desc, p.id
    );
    defer stmt.finalize();
    try stmt.bind(&.{});
    while (true) switch (try stmt.step()) {
        .done => return null,
        .row => {
            const project_id = stmt.columnInt(0);
            const project_root = try stmt.columnTextAlloc(1, ctx.allocator);
            defer ctx.allocator.free(project_root);
            const org_root = try stmt.columnTextAlloc(2, ctx.allocator);
            defer ctx.allocator.free(org_root);
            if (!pathHasPrefix(cwd, org_root) or !pathHasPrefix(cwd, project_root)) continue;
            if (std.mem.eql(u8, cwd, org_root)) return try expandWorkspaceReadSet(ctx, d, stmt.columnInt(3));
            const out = try ctx.allocator.alloc(ReadScope, 1);
            out[0] = .{ .kind = .repo, .id = project_id };
            return out;
        },
    };
}

fn expandWorkspaceReadSet(
    ctx: *const runtime.Ctx,
    d: anytype,
    org_assoc_id: i64,
) ![]ReadScope {
    var out: std.ArrayList(ReadScope) = .empty;
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
            \\select id, root_path
            \\from projects
            \\where root_path is not null
            \\order by root_path
        );
        defer stmt.finalize();
        try stmt.bind(&.{});
        while (true) switch (try stmt.step()) {
            .done => break,
            .row => {
                const project_id = stmt.columnInt(0);
                const root = try stmt.columnTextAlloc(1, ctx.allocator);
                defer ctx.allocator.free(root);
                if (pathHasPrefix(cwd, root)) {
                    const kind = try ctx.allocator.dupe(u8, "repo");
                    errdefer ctx.allocator.free(kind);
                    const candidate = Candidate{
                        .id = project_id,
                        .kind = kind,
                        .root_len = root.len,
                    };
                    if (candidateExists(out.items, candidate)) {
                        ctx.allocator.free(candidate.kind);
                    } else {
                        try out.append(ctx.allocator, candidate);
                    }
                }
            },
        };
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
                    const candidate = Candidate{
                        .id = assoc_id,
                        .kind = kind,
                        .root_len = root.len,
                    };
                    if (candidateExists(out.items, candidate)) {
                        ctx.allocator.free(kind);
                    } else {
                        try out.append(ctx.allocator, candidate);
                    }
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
                    const candidate = Candidate{
                        .id = assoc_id,
                        .kind = kind,
                        .root_len = root.len,
                    };
                    if (candidateExists(out.items, candidate)) {
                        ctx.allocator.free(kind);
                    } else {
                        try out.append(ctx.allocator, candidate);
                    }
                } else {
                    ctx.allocator.free(kind);
                }
            },
        };
    }

    return try out.toOwnedSlice(ctx.allocator);
}

fn candidateExists(items: []const Candidate, candidate: Candidate) bool {
    for (items) |item| {
        if (item.id == candidate.id and
            item.root_len == candidate.root_len and
            std.mem.eql(u8, item.kind, candidate.kind))
        {
            return true;
        }
    }
    return false;
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

fn specificityRank(kind: []const u8) u8 {
    if (std.mem.eql(u8, kind, "project")) return 1;
    if (std.mem.eql(u8, kind, "ad-hoc")) return 2;
    if (std.mem.eql(u8, kind, "personal")) return 2;
    if (std.mem.eql(u8, kind, "client")) return 3;
    if (std.mem.eql(u8, kind, "repo")) return 4;
    if (std.mem.eql(u8, kind, "org")) return 5;
    return 6;
}

/// Read a POSIX env var from std.c.environ. Returns null when unset
/// or empty. The returned slice points into the process environ block
/// and must NOT be freed.
fn getPosixEnv(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (s.len <= key.len + 1) continue;
        if (s[key.len] != '=') continue;
        if (!std.mem.eql(u8, s[0..key.len], key)) continue;
        const val = s[key.len + 1 ..];
        if (val.len == 0) return null;
        return val;
    }
    return null;
}

const MetaWriteResolution = union(enum) {
    none,
    repo_scope: []const u8,
    ambiguous: struct {
        assoc_scope: []const u8,
        repo_scope: []const u8,
    },
};

fn deinitMetaWriteResolution(allocator: std.mem.Allocator, r: MetaWriteResolution) void {
    switch (r) {
        .none => {},
        .repo_scope => |s| allocator.free(s),
        .ambiguous => |choices| {
            allocator.free(choices.assoc_scope);
            allocator.free(choices.repo_scope);
        },
    }
}

fn resolveMetaWorkspaceWriteScope(
    ctx: *const runtime.Ctx,
    d: anytype,
    cwd: []const u8,
) !MetaWriteResolution {
    {
        var stmt = try d.prepare(
            \\select a.slug, p.slug
            \\from associations a
            \\join project_associations pa on pa.association_id = a.id
            \\join projects p on p.id = pa.project_id
            \\where a.kind = 'org'
            \\  and p.root_path = ?
            \\  and json_extract(a.config_json, '$.root_path') = ?
            \\  and json_extract(a.config_json, '$.workspace_shape') = 'meta-repo'
            \\order by a.id
            \\limit 1
        );
        defer stmt.finalize();
        try stmt.bind(&.{ .{ .text = cwd }, .{ .text = cwd } });
        if ((try stmt.step()) == .row) {
            const assoc_slug = try stmt.columnTextAlloc(0, ctx.allocator);
            defer ctx.allocator.free(assoc_slug);
            const repo_slug = try stmt.columnTextAlloc(1, ctx.allocator);
            defer ctx.allocator.free(repo_slug);
            return .{ .ambiguous = .{
                .assoc_scope = try std.fmt.allocPrint(ctx.allocator, "assoc:{s}", .{assoc_slug}),
                .repo_scope = try std.fmt.allocPrint(ctx.allocator, "repo:{s}", .{repo_slug}),
            } };
        }
    }

    var stmt = try d.prepare(
        \\select p.slug, p.root_path, json_extract(a.config_json, '$.root_path')
        \\from projects p
        \\join project_associations pa on pa.project_id = p.id
        \\join associations a on a.id = pa.association_id
        \\where a.kind = 'org'
        \\  and p.root_path is not null
        \\  and json_extract(a.config_json, '$.workspace_shape') = 'meta-repo'
        \\  and json_extract(a.config_json, '$.root_path') is not null
        \\order by length(p.root_path) desc, p.id
    );
    defer stmt.finalize();
    try stmt.bind(&.{});
    while (true) switch (try stmt.step()) {
        .done => return .none,
        .row => {
            const slug = try stmt.columnTextAlloc(0, ctx.allocator);
            defer ctx.allocator.free(slug);
            const project_root = try stmt.columnTextAlloc(1, ctx.allocator);
            defer ctx.allocator.free(project_root);
            const org_root = try stmt.columnTextAlloc(2, ctx.allocator);
            defer ctx.allocator.free(org_root);
            if (pathHasPrefix(cwd, org_root) and pathHasPrefix(cwd, project_root)) {
                return .{ .repo_scope = try std.fmt.allocPrint(ctx.allocator, "repo:{s}", .{slug}) };
            }
        },
    };
}

fn pathHasPrefix(target: []const u8, root: []const u8) bool {
    if (std.mem.eql(u8, target, root)) return true;
    if (!std.mem.startsWith(u8, target, root)) return false;
    if (target.len <= root.len) return false;
    return target[root.len] == '/';
}

/// Refuse to mutate an entity whose stored scope disagrees with
/// `write_scope`. Thin pass-through to the engine policy guard —
/// present here so handlers can call `scope.guard(...)` alongside
/// `scope.resolve(...)` without reaching into engine.policy directly.
pub fn guard(entity_scope: Scope, write_scope: Scope) !void {
    try engine.policy.scope_guard.check(entity_scope, write_scope);
}
