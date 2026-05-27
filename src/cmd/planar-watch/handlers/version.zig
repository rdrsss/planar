//! planar-watch handlers/version.zig — `planar-watch version`
//!
//! Mirrors `planar version` and `planar-agent version`; emits the
//! `planar-watch` binary name so operators can tell the three binaries
//! apart in scripts. Same field order so a shell grep on the leading
//! "planar-watch " prefix is stable.

const std = @import("std");
const cli = @import("cli");
const build_options = @import("build_options");
const runtime = @import("runtime");

pub const verb: cli.Cmd = .{
    .name = "version",
    .desc = "Print the planar-watch version, commit, and zig runtime.",
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = args_ptr;
    const ctx = runtime.current();

    const sha_short = shortenSha(build_options.git_sha);
    const dirty_marker: []const u8 = if (build_options.git_dirty) "+dirty" else "";
    const zig_ver = @import("builtin").zig_version_string;

    try ctx.stdout.print(
        "planar-watch {s}{s} {s} zig {s}\n",
        .{ sha_short, dirty_marker, build_options.build_date, zig_ver },
    );
}

fn shortenSha(sha: []const u8) []const u8 {
    if (sha.len <= 12) return sha;
    return sha[0..12];
}
