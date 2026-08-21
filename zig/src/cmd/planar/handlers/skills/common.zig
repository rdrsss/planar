const std = @import("std");
const engine = @import("engine");
const runtime = @import("runtime");

pub const ResolvedHomes = struct {
    planar_home: []const u8,
    home: []const u8,
    codex_home: []const u8,

    pub fn deinit(self: ResolvedHomes, allocator: std.mem.Allocator) void {
        allocator.free(self.planar_home);
        allocator.free(self.home);
        allocator.free(self.codex_home);
    }

    pub fn options(self: ResolvedHomes, vendor: ?[]const u8) engine.installedsurface.Options {
        return .{
            .planar_home = self.planar_home,
            .home = self.home,
            .codex_home = self.codex_home,
            .vendor = vendor,
        };
    }
};

pub fn resolveHomes(ctx: *const runtime.Ctx) !ResolvedHomes {
    const home = ctx.environ.getPosix("HOME") orelse return error.HomeNotSet;
    const planar_home = if (ctx.environ.getPosix("PLANAR_HOME")) |value|
        try ctx.allocator.dupe(u8, value)
    else
        try std.fs.path.join(ctx.allocator, &.{ home, ".planar" });
    errdefer ctx.allocator.free(planar_home);
    const codex_home = if (ctx.environ.getPosix("CODEX_HOME")) |value|
        try ctx.allocator.dupe(u8, value)
    else
        try std.fs.path.join(ctx.allocator, &.{ home, ".codex" });
    errdefer ctx.allocator.free(codex_home);
    return .{
        .planar_home = planar_home,
        .home = try ctx.allocator.dupe(u8, home),
        .codex_home = codex_home,
    };
}
