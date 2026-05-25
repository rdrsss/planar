//! handlers/templates/common — shared helpers for the templates command.
//!
//! Mirrors Go `cmd/planar/internal/cli/templates.go` (ResolveTemplatesRoot)
//! and the per-handler boilerplate that loads config + expands ~.

const std = @import("std");
const engine = @import("engine");
const runtime = @import("../../runtime.zig");
const config_path = @import("../config/path.zig");

/// resolveTemplatesRoot returns the effective templates directory, honouring
/// `$PLANAR_TEMPLATES_DIR` (config env override) > `~/.planar/config.toml`
/// (templates.dir) > embedded default. Tildes are expanded against `$HOME`.
/// Caller owns the returned bytes.
pub fn resolveTemplatesRoot(ctx: *const runtime.Ctx) ![]u8 {
    const cfg_path = try config_path.resolveConfigPath(ctx.allocator, ctx.environ);
    defer ctx.allocator.free(cfg_path);

    const file_content: ?[]u8 = std.Io.Dir.cwd().readFileAlloc(
        ctx.io,
        cfg_path,
        ctx.allocator,
        .unlimited,
    ) catch |e| switch (e) {
        error.FileNotFound => null,
        else => return e,
    };
    defer if (file_content) |fc| ctx.allocator.free(fc);

    var resolved = try engine.config.resolve(ctx.allocator, file_content, ctx.environ, null);
    defer resolved.deinit(ctx.allocator);

    return try expandTilde(ctx.allocator, resolved.config.templates.dir, ctx.environ);
}

/// expandTilde expands a leading ~ against `$HOME`. Returns an owned copy.
fn expandTilde(allocator: std.mem.Allocator, path: []const u8, environ: std.process.Environ) ![]u8 {
    if (path.len == 0) return try allocator.dupe(u8, "");
    if (std.mem.eql(u8, path, "~")) {
        const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
        return try allocator.dupe(u8, home);
    }
    if (std.mem.startsWith(u8, path, "~/")) {
        const home = environ.getPosix("HOME") orelse return error.HomeNotSet;
        return try std.fs.path.join(allocator, &.{ home, path[2..] });
    }
    return try allocator.dupe(u8, path);
}
