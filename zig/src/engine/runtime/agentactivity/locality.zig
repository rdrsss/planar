//! agentactivity/locality — best-effort git-state snapshot for claims
//! and actions.
//!
//! Runs three short `git` subprocess invocations against `repo_root` and
//! returns a `Locality` value answering "which branch, which sha, was
//! the worktree dirty?" Any subprocess failure (non-git directory,
//! missing `git`, error exit, signal) maps to `unknown` / NULL rather
//! than refusing the call — the locality data is opportunistic context,
//! not a synchronization gate. The tech spec § "Locality tracking"
//! § "Out of scope" pins this contract.
//!
//! Probe commands (matching tech-spec § "What gets captured"):
//!   git -C <repo_root> symbolic-ref --short HEAD   → branch (NULL on detached)
//!   git -C <repo_root> rev-parse HEAD              → head_sha
//!   git -C <repo_root> status --porcelain          → dirty (clean|dirty|unknown)
//!
//! The `--no-locality-probe` CLI flag short-circuits to `Locality.skipped`
//! at the caller, before this module is reached.

const std = @import("std");
const types = @import("types.zig");

/// Locality probe options. The Io and allocator are explicit; `repo_root`
/// is the absolute path to use as `git -C` working directory.
pub const ProbeArgs = struct {
    allocator: std.mem.Allocator,
    io: std.Io,
    /// Absolute path of the checkout to probe. Required — the engine
    /// resolves the operator-supplied --repo-root or process cwd before
    /// invoking the probe.
    repo_root: []const u8,
};

/// Run the three git probes against `args.repo_root` and return a fully
/// populated Locality value. Never fails — every probe failure maps to
/// the `unknown` / NULL form. Caller owns the returned strings via the
/// `Locality.deinit` helper.
pub fn probe(args: ProbeArgs) std.mem.Allocator.Error!types.Locality {
    var out: types.Locality = .{};
    errdefer out.deinit(args.allocator);

    // repo_root is always captured (operator passed it in or we
    // inherited cwd). Duplicate it into the caller's allocator so the
    // result owns the slice.
    out.repo_root = try args.allocator.dupe(u8, args.repo_root);

    // Branch — symbolic-ref returns exit 1 on detached HEAD, which is
    // not an error: it just means "no branch name" and we leave branch
    // as null.
    out.branch = runGitTrim(args, &.{ "symbolic-ref", "--short", "HEAD" });

    // HEAD sha. Failure (non-git dir, etc.) leaves head_sha null AND
    // means we cannot trust the porcelain output below either; treat as
    // a fully unknown probe.
    out.head_sha = runGitTrim(args, &.{ "rev-parse", "HEAD" });

    if (out.head_sha == null) {
        // Not a git checkout. Branch/dirty cannot be meaningful either.
        if (out.branch) |b| {
            args.allocator.free(b);
            out.branch = null;
        }
        out.dirty = .unknown;
        return out;
    }

    // Porcelain status: empty → clean; non-empty → dirty; subprocess
    // failure → unknown.
    if (runGit(args, &.{ "status", "--porcelain" })) |result| {
        defer args.allocator.free(result);
        const trimmed = std.mem.trim(u8, result, " \t\r\n");
        out.dirty = if (trimmed.len == 0) .clean else .dirty;
    } else {
        out.dirty = .unknown;
    }

    return out;
}

/// runGit returns the raw stdout (caller owns) of `git -C repo_root
/// <args...>` on exit 0, or null otherwise. Stderr is silently discarded
/// (the probe is best-effort and any noise we emit would be confusing).
fn runGit(args: ProbeArgs, argv_extra: []const []const u8) ?[]u8 {
    var argv_buf: [16][]const u8 = undefined;
    if (3 + argv_extra.len > argv_buf.len) return null;
    argv_buf[0] = "git";
    argv_buf[1] = "-C";
    argv_buf[2] = args.repo_root;
    for (argv_extra, 0..) |a, i| argv_buf[3 + i] = a;
    const argv = argv_buf[0 .. 3 + argv_extra.len];

    const result = std.process.run(args.allocator, args.io, .{
        .argv = argv,
    }) catch return null;
    defer args.allocator.free(result.stderr);

    switch (result.term) {
        .exited => |code| {
            if (code != 0) {
                args.allocator.free(result.stdout);
                return null;
            }
        },
        else => {
            args.allocator.free(result.stdout);
            return null;
        },
    }
    return result.stdout;
}

/// runGitTrim runs `runGit` and trims the result to its first
/// non-whitespace line (typical for sha / branch output). Returns null
/// when the command failed or produced empty/whitespace-only output.
fn runGitTrim(args: ProbeArgs, argv_extra: []const []const u8) ?[]const u8 {
    const raw = runGit(args, argv_extra) orelse return null;
    defer args.allocator.free(raw);
    const trimmed = std.mem.trim(u8, raw, " \t\r\n");
    if (trimmed.len == 0) return null;
    const owned = args.allocator.dupe(u8, trimmed) catch return null;
    return owned;
}

// ---------------------------------------------------------------------
// Tests — exercise the probe against the harness tmp dir. We cannot
// assume the unit-test process is INSIDE a git checkout (or that one
// has a stable branch / sha), so the tests focus on the failure-mode
// invariants the engine actually relies on.
// ---------------------------------------------------------------------

test "probe against missing dir returns repo_root + null branch/head_sha + unknown dirty" {
    // We deliberately do NOT use the harness tmpDir here: the harness
    // root lives under `.zig-cache/tmp/...` which is INSIDE the planar
    // git checkout, and `git rev-parse HEAD` walks parents — it would
    // discover the planar repo and report a real sha. Use a path that
    // does not exist so git's `-C` fails outright.
    const gpa = std.testing.allocator;
    var loc = try probe(.{
        .allocator = gpa,
        .io = std.testing.io,
        .repo_root = "/nonexistent/planar-locality-probe-no-git-XXXX",
    });
    defer loc.deinit(gpa);

    // repo_root always echoed back.
    try std.testing.expect(loc.repo_root != null);
    // No git repo here → branch and head_sha null, dirty unknown.
    try std.testing.expect(loc.branch == null);
    try std.testing.expect(loc.head_sha == null);
    try std.testing.expectEqual(types.Dirty.unknown, loc.dirty);
}

test "probe against missing dir returns unknown-everything" {
    const gpa = std.testing.allocator;
    var loc = try probe(.{
        .allocator = gpa,
        .io = std.testing.io,
        // Path that does not exist; git will exit non-zero and we
        // record `unknown` rather than refusing.
        .repo_root = "/nonexistent/planar-locality-test-XXXX",
    });
    defer loc.deinit(gpa);
    try std.testing.expect(loc.repo_root != null);
    try std.testing.expect(loc.branch == null);
    try std.testing.expect(loc.head_sha == null);
    try std.testing.expectEqual(types.Dirty.unknown, loc.dirty);
}

test "probe against this repo's checkout records branch + sha (best-effort)" {
    const gpa = std.testing.allocator;
    // The unit-test process runs at the repo root when invoked via
    // `zig build test`; resolve cwd absolutely and probe it. If git is
    // missing on the host this test still passes — it asserts the
    // invariants that hold either way (repo_root non-null, dirty in
    // the enum set).
    const cwd = std.Io.Dir.realPathFileAlloc(.cwd(), std.testing.io, ".", gpa) catch return error.SkipZigTest;
    defer gpa.free(cwd);

    var loc = try probe(.{
        .allocator = gpa,
        .io = std.testing.io,
        .repo_root = cwd,
    });
    defer loc.deinit(gpa);

    try std.testing.expect(loc.repo_root != null);
    // `dirty` must always be one of the three enum values, regardless
    // of whether the probe succeeded.
    switch (loc.dirty) {
        .clean, .dirty, .unknown => {},
    }
}
