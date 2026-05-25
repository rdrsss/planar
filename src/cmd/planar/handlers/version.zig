//! handlers/version.zig — `planar version`
//!
//! Emits a one-line build-info string compatible in shape with Go's
//! `planar version`. Format:
//!
//!     planar <git-sha-short>[+dirty] <build-date> zig <zig-version>
//!
//! Each component degrades to "unknown" when its build-time resolver
//! returned no value (binary built outside a git checkout). The sha is
//! truncated to 12 hex chars to match Go's `buildVersion()` helper.
//! Scripts may grep for the leading "planar " prefix or split on
//! whitespace; the field order is stable.

const std = @import("std");
const cli = @import("cli");
const build_options = @import("build_options");
const main = @import("../main.zig");
const runtime = @import("../runtime.zig");

pub const verb: cli.Cmd = .{
    .name = "version",
    .desc = "Print the planar version, commit, and zig runtime.",
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = args_ptr;
    const ctx = runtime.current();

    const sha_short = shortenSha(build_options.git_sha);
    const dirty_marker: []const u8 = if (build_options.git_dirty) "+dirty" else "";
    const zig_ver = @import("builtin").zig_version_string;

    try ctx.stdout.print(
        "planar {s}{s} {s} zig {s}\n",
        .{ sha_short, dirty_marker, build_options.build_date, zig_ver },
    );
}

/// shortenSha truncates a 40-char git sha to 12 hex characters to match
/// Go's `buildVersion()`. Short shas, sentinel values like "unknown", and
/// anything else not at least 12 chars wide pass through untouched.
fn shortenSha(sha: []const u8) []const u8 {
    if (sha.len <= 12) return sha;
    return sha[0..12];
}

test "shortenSha truncates 40-char hex to 12" {
    const long = "0123456789abcdef0123456789abcdef01234567";
    try std.testing.expectEqualStrings("0123456789ab", shortenSha(long));
}

test "shortenSha passes short input through" {
    try std.testing.expectEqualStrings("unknown", shortenSha("unknown"));
    try std.testing.expectEqualStrings("abc", shortenSha("abc"));
}
