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
const runtime = @import("runtime.zig");

pub const Scope = engine.identity.scope.Scope;
pub const Resolution = engine.identity.scope.Resolution;
pub const Reason = engine.identity.scope.Reason;

/// Resolve the active scope for this handler call. `override` is the
/// parsed `--scope` flag value (null if not set). Returns the engine
/// resolution unchanged so callers can inspect the reason — handlers
/// that only care about the slug just read `.scope`.
pub fn resolve(ctx: *const runtime.Ctx, override: ?[]const u8) !Resolution {
    if (override) |slug| {
        return .{
            .scope = slug,
            .reason = .project_single_association,
            .project_slug = null,
        };
    }
    const cwd = try std.Io.Dir.realPathFileAlloc(.cwd(), ctx.io, ".", ctx.allocator);
    defer ctx.allocator.free(cwd);

    const d = try runtime.ensureDb();
    return try engine.identity.scope.deriveFromCwd(d, ctx.allocator, cwd);
}

/// Refuse to mutate an entity whose stored scope disagrees with
/// `write_scope`. Thin pass-through to the engine policy guard —
/// present here so handlers can call `scope.guard(...)` alongside
/// `scope.resolve(...)` without reaching into engine.policy directly.
pub fn guard(entity_scope: Scope, write_scope: Scope) !void {
    try engine.policy.scope_guard.check(entity_scope, write_scope);
}
