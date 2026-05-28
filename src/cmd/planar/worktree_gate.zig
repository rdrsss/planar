//! worktree_gate — Plan 297 M3 runtime check that refuses planning
//! verbs invoked from inside a git worktree.
//!
//! Wired into `planar/main.zig` immediately before `cli.dispatch`, so
//! the gate fires for every verb invocation except `--help`-only
//! traversals (the parser short-circuits help itself).
//!
//! Hook ordering:
//!
//!   1. Parser resolves the verb path from argv.       (this module)
//!   2. Worktree gate checks classification + cwd.     (this module)
//!   3. cli.dispatch enters the leaf handler.          (etc-cli)
//!   4. Handler calls scope.resolve which runs the
//!      existing cross-scope guard if applicable.       (handlers/*)
//!
//! Exit-code choice: `8`. Codes 0–7 are taken by `exit.codeFor`
//! (0/success, 1/generic, 2/input, 3/conflict, 5/scope, 6/already-
//! exists, 7/schema). Picking 8 gives scripts a distinct value to
//! disambiguate "you ran a planning verb from a worktree" from "your
//! scope was wrong" (5). The exit code does NOT flow through
//! `exit.codeFor` — the gate raises and exits directly to keep the
//! mapping load-bearing in one place.

const std = @import("std");
const cli = @import("cli");
const runtime = @import("runtime");
const engine = @import("engine");
const classification = @import("verb_classification.zig");
const scope_mod = @import("scope.zig");

/// Exit code for the worktree refusal. Picked above the existing
/// `exit.codeFor` range (0–7) so scripts can disambiguate from the
/// scope-mismatch exit (5) and the schema-version exit (7).
pub const exit_code_worktree_refusal: u8 = 8;

/// Inspect argv, resolve the verb path it points at, and refuse if the
/// path classifies as `planning` AND the cwd lives inside a worktree.
/// Returns normally on allow; calls `std.process.exit` on refusal.
///
/// `--scope <slug>` does NOT override this check — the spec is explicit
/// that the rule is about *where the verb runs*, not which scope it
/// targets. The detection consults cwd-derived state only.
pub fn check(comptime root: cli.Cmd, argv: []const []const u8) void {
    // Test / harness escape: `PLANAR_DISABLE_WORKTREE_GATE=1`
    // skips the check entirely. Used by the integration harness,
    // whose tmp-dir fixtures inevitably live under this repo's
    // `.worktrees/` when Planar is itself being developed inside a
    // worktree. NOT a documented operator-facing flag — it's a test
    // affordance only.
    if (getPosixEnv("PLANAR_DISABLE_WORKTREE_GATE")) |v| {
        if (v.len > 0 and v[0] != '0') return;
    }

    // Resolve verb path from argv. We do this without invoking the
    // full parser because the parser is comptime-specialized per leaf;
    // we only need the path tokens.
    var path_buf: [16][]const u8 = undefined;
    const path = resolveVerbPath(root, argv, &path_buf);
    if (path.len == 0) return; // No verb matched — let the parser
    // surface the error.

    const class = classification.classify(path);
    if (class == .execution_or_read) return;

    // Planning verb. Check for worktree-cwd. We use the engine
    // detector directly (no DB needed) so that the gate fires even
    // before any handler has called scope.resolve.
    const ctx = runtime.current();
    const cwd = ctx.allocator.dupe(u8, currentWorkingDir(ctx) orelse return) catch return;
    defer ctx.allocator.free(cwd);

    const det = engine.identity.scope.detectWorktree(ctx.io, ctx.allocator, cwd) catch return;
    defer engine.identity.scope.deinitWorktreeDetection(ctx.allocator, det);

    if (!det.is_worktree) return;

    // Refused. Print the canonical 4-line error and exit.
    refuse(ctx, path, det.worktree_root orelse cwd, det.parent_repo_root orelse "(unknown)");
}

fn refuse(
    ctx: *const runtime.Ctx,
    path: []const []const u8,
    worktree_root: []const u8,
    parent_repo_root: []const u8,
) noreturn {
    // Compose the verb-path display (`plan add`, `task touches add`).
    // 256 bytes is comfortably more than any composed verb path
    // currently in the tree; truncate on overflow rather than
    // dynamically allocating from an unknown allocator.
    var buf: [256]u8 = undefined;
    var len: usize = 0;
    for (path, 0..) |p, i| {
        if (i > 0) {
            if (len >= buf.len) break;
            buf[len] = ' ';
            len += 1;
        }
        const take = @min(p.len, buf.len - len);
        @memcpy(buf[len .. len + take], p[0..take]);
        len += take;
    }
    const verb = buf[0..len];

    // Four-line refusal block (labels are verbatim per the tech spec).
    ctx.stderr.print(
        "error: planning verb '{s}' may not run from inside a worktree\n" ++
            "  cwd:        {s}\n" ++
            "  parent:     {s}\n" ++
            "  reason:     Worktrees are for code execution, not for planning the work itself.\n" ++
            "  suggestion: cd {s} and re-run.\n",
        .{ verb, worktree_root, parent_repo_root, parent_repo_root },
    ) catch {};

    runtime.shutdown();
    std.process.exit(exit_code_worktree_refusal);
}

/// Walk argv against the comptime command tree, collecting the
/// resolved subcommand-path tokens. Stops at the first non-matching
/// token (a flag, a positional, or `--`). Returns a slice of
/// `path_buf`; if nothing matched, returns an empty slice.
///
/// Intentionally simple — we only need the path, not the flag values.
/// Flags that take values (the parser's `flagWantsValue` rule) are
/// skipped via a two-token consume so a value like `--title foo` does
/// not get mistaken for a subcommand on the next token.
fn resolveVerbPath(
    comptime root: cli.Cmd,
    argv: []const []const u8,
    path_buf: []([]const u8),
) []const []const u8 {
    if (argv.len < 2) return path_buf[0..0];

    var current: cli.Cmd = root;
    var i: usize = 1; // skip argv[0] (program name)
    var path_len: usize = 0;
    while (i < argv.len) : (i += 1) {
        const tok = argv[i];
        if (tok.len == 0) continue;
        if (std.mem.eql(u8, tok, "--")) break;
        if (tok[0] == '-') {
            // Flag-shaped. If it's known to take a value at this
            // scope, skip the next token too.
            if (flagAcceptsValue(current, tok)) i += 1;
            continue;
        }
        // Positional / subcommand candidate.
        var matched = false;
        for (current.cmds) |c| {
            if (commandMatches(c, tok)) {
                if (path_len >= path_buf.len) return path_buf[0..path_len];
                path_buf[path_len] = c.name;
                path_len += 1;
                current = c;
                matched = true;
                break;
            }
        }
        if (!matched) break; // First non-subcommand token ends the path.
    }
    return path_buf[0..path_len];
}

fn commandMatches(c: cli.Cmd, tok: []const u8) bool {
    if (std.mem.eql(u8, c.name, tok)) return true;
    for (c.aliases) |alias| {
        if (std.mem.eql(u8, alias, tok)) return true;
    }
    return false;
}

/// Does the flag named by `tok` (e.g. `--scope`, `--title`) take a
/// value at this command's scope (or any ancestor's)? Conservative:
/// when in doubt, assume yes so we don't accidentally consume a value
/// as a subcommand token.
fn flagAcceptsValue(c: cli.Cmd, tok: []const u8) bool {
    // Strip a leading `--name=value` form into just `--name`.
    var name = tok;
    if (std.mem.indexOfScalar(u8, tok, '=')) |eq_idx| {
        name = tok[0..eq_idx];
        // Inline value — no second token to consume.
        return false;
    }
    for (c.flags) |f| {
        if (std.mem.eql(u8, f.long, name)) {
            return f.kind != .bool;
        }
        if (f.short) |s| {
            // Short flags arrive as `-x`; build a 2-byte expected.
            var short_buf: [2]u8 = .{ '-', s };
            if (std.mem.eql(u8, name, short_buf[0..])) return f.kind != .bool;
        }
    }
    // Unknown flag at this scope — be conservative and assume it has
    // a value. The parser will reject it later if not.
    return true;
}

/// PWD-first cwd resolution, identical posture to `scope.zig`. Returns
/// a slice borrowed from process environ (do not free) or null on
/// unavailable PWD — caller falls back to a libc getcwd.
fn currentWorkingDir(ctx: *const runtime.Ctx) ?[]const u8 {
    _ = ctx;
    return getPosixEnv("PWD");
}

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
