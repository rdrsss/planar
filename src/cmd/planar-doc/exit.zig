//! cmd/planar-doc/exit — domain-error → user-message + exit-code path.

const std = @import("std");
const runtime = @import("runtime");

pub fn codeFor(err: anyerror) u8 {
    return switch (err) {
        error.NotImplemented => 64,
        error.InvalidInput => 2,
        else => 1,
    };
}

pub fn die(
    ctx: *const runtime.Ctx,
    err: anyerror,
    comptime fmt: []const u8,
    args: anytype,
) noreturn {
    ctx.stderr.print("error: " ++ fmt ++ "\n", args) catch {};
    runtime.shutdown();
    std.process.exit(codeFor(err));
}
