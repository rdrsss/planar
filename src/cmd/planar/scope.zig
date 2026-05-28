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

pub const Scope = engine.identity.scope.Scope;
pub const Resolution = engine.identity.scope.Resolution;
pub const Reason = engine.identity.scope.Reason;

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

/// PWD-first cwd resolution (matches Go's os.Getwd). Falls back to
/// realPath when PWD is absent. See plan 351 task 2375 for rationale.
fn operatorCwd(allocator: std.mem.Allocator, io: std.Io) ![]const u8 {
    if (getPosixEnv("PWD")) |pwd| {
        return try allocator.dupe(u8, pwd);
    }
    return try std.Io.Dir.realPathFileAlloc(.cwd(), io, ".", allocator);
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

/// Refuse to mutate an entity whose stored scope disagrees with
/// `write_scope`. Thin pass-through to the engine policy guard —
/// present here so handlers can call `scope.guard(...)` alongside
/// `scope.resolve(...)` without reaching into engine.policy directly.
pub fn guard(entity_scope: Scope, write_scope: Scope) !void {
    try engine.policy.scope_guard.check(entity_scope, write_scope);
}
