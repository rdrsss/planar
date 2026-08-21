//! Runtime parser.
//!
//! `parse(root, path, argv)` walks argv against the tree starting at `root`,
//! returns a `Result` carrying the matched command path and a typed
//! `ArgsType` for whichever leaf matched (via a tagged union over all
//! leaves). Scalar strings in the result are slices into argv; no allocation.
//! Slice fields backed by parser module-static arrays (`rest_field`,
//! repeatable/list flags, and help paths) are valid only until the next
//! `parse`/`dispatch` call. The parser does not provide a reentrant or
//! thread-safe result-buffer contract; callers that need longer-lived results
//! must copy those slices before invoking the parser again.
//!
//! `dispatch(root, argv)` is the callback-mode wrapper: parse + invoke the
//! matched leaf's `run` handler (cast from the type-erased pointer).
//!
//! The parser is specialized on `comptime root: Cmd` so each tree generates
//! its own parsing code — enables comptime-sized buffers, exhaustive
//! switches, and zero-allocation matching.

const std = @import("std");
const cmd_mod = @import("cmd.zig");
const flag_mod = @import("flag.zig");
const err_mod = @import("error.zig");
const help_mod = @import("help.zig");
const duration_mod = @import("duration.zig");

const Cmd = cmd_mod.Cmd;
const Flag = flag_mod.Flag;
const FlagGroup = flag_mod.FlagGroup;
const Positional = flag_mod.Positional;

/// Module-static backing buffer for rest-positional capture (the leaf's
/// `rest_field`). Lives at module scope so the slice returned in Args stays
/// valid after parseLeaf returns. Single-threaded by construction: callers
/// must consume or copy the slice before the next `parse`/`dispatch`
/// invocation, which may overwrite it.
/// 256 is well past any realistic argv positional count.
var rest_buf: [256][]const u8 = undefined;

/// Module-static backing store for repeatable (`list`) flag values. Each list
/// flag in the active leaf gets a row (assigned by its comptime ordinal among
/// list flags); accumulated values land in that row. Like `rest_buf`, the
/// returned slices stay valid only until the next parser invocation, which
/// resets the counts and may overwrite values. Single-threaded by
/// construction. Caps are generous for real CLIs.
const max_list_flags = 16;
const max_list_items = 128;
var list_buf: [max_list_flags][max_list_items][]const u8 = undefined;
var list_counts: [max_list_flags]usize = .{0} ** max_list_flags;

/// Module-static backing buffer for the resolved command path returned in a
/// `.help` result. Like `rest_buf`, it lives at module scope so the slice
/// stays valid after `parseImpl` returns — its elements are comptime
/// command-name literals, so only the array (not the strings) needs the
/// static home. Single-threaded by construction: callers must consume or copy
/// the path before the next `parse`/`dispatch` invocation, which may overwrite
/// it. 256 is well past any realistic tree depth.
var help_path_buf: [256][]const u8 = undefined;

const max_tail_tokens = 512;
const max_group_error_flags = 256;
var group_error_flags_buf: [max_group_error_flags][]const u8 = undefined;

/// Module-static record of which deprecated flags the most recent parse
/// actually matched (by canonical long name). `cli.run` reads this after a
/// successful parse to emit one warning per used deprecated flag; `parse`
/// and `dispatch` stay quiet (the warning is a runner-only concern, like
/// command-level deprecation). Single-threaded by construction, like the
/// other module-static buffers: consume it before the next parser call.
const max_deprecated_flags = 64;
var deprecated_flags_buf: [max_deprecated_flags][]const u8 = undefined;
var deprecated_flags_len: usize = 0;

/// The deprecated flags (canonical long names) matched by the most recent
/// `parse`/`dispatch`/`run`. Valid until the next parser invocation.
pub fn deprecatedFlagsSeen() []const []const u8 {
    return deprecated_flags_buf[0..deprecated_flags_len];
}

fn recordDeprecatedFlag(long: []const u8) void {
    for (deprecated_flags_buf[0..deprecated_flags_len]) |seen_long| {
        if (std.mem.eql(u8, seen_long, long)) return;
    }
    if (deprecated_flags_len >= deprecated_flags_buf.len) return;
    deprecated_flags_buf[deprecated_flags_len] = long;
    deprecated_flags_len += 1;
}

/// Copy the resolved path into the module-static buffer and return it as a
/// `.help` result. Returning a slice into `parseImpl`'s stack-local
/// `path_buf` would dangle once `parseImpl` returns to its caller.
fn helpResult(comptime root: Cmd, path: []const []const u8) Result(root) {
    std.debug.assert(path.len <= help_path_buf.len);
    for (path, 0..) |seg, idx| help_path_buf[idx] = seg;
    return .{ .help = help_path_buf[0..path.len] };
}

/// Outcome of a parse: either a typed `Match` carrying the active leaf's
/// args, or a `Help` request (when `--help` was seen) carrying the path
/// whose help should be rendered.
pub fn Result(comptime root: Cmd) type {
    return union(enum) {
        match: ResultUnion(root),
        help: []const []const u8,
    };
}

/// Tagged union with one variant per leaf command. Variant tag is the
/// path joined with `_`; payload is `ArgsType(root, path)`.
pub fn ResultUnion(comptime root: Cmd) type {
    @setEvalBranchQuota(20_000_000);
    const leaves = comptime cmd_mod.allLeaves(root);
    const N = leaves.len;

    const TagInt = std.math.IntFittingRange(0, @max(N, 1));

    comptime var names: [N][]const u8 = undefined;
    comptime var tag_values: [N]TagInt = undefined;
    comptime var types: [N]type = undefined;
    comptime var union_attrs: [N]std.builtin.Type.UnionField.Attributes = undefined;

    inline for (leaves, 0..) |leaf, i| {
        names[i] = comptime pathToTag(leaf.path);
        tag_values[i] = @intCast(i);
        types[i] = cmd_mod.ArgsType(root, leaf.path);
        union_attrs[i] = .{};
    }

    const Tag = @Enum(TagInt, .exhaustive, &names, &tag_values);

    return @Union(.auto, Tag, &names, &types, &union_attrs);
}

fn pathToTag(comptime path: []const []const u8) []const u8 {
    comptime {
        if (path.len == 0) return "root";
        var joined: []const u8 = path[0];
        for (path[1..]) |seg| {
            joined = joined ++ "_" ++ seg;
        }
        // Replace dashes with underscores so the result is a valid identifier.
        var buf: [joined.len]u8 = undefined;
        for (joined, 0..) |c, i| {
            buf[i] = if (c == '-') '_' else c;
        }
        const final = buf;
        return &final;
    }
}

fn commandPathString(comptime root_name: []const u8, comptime path: []const []const u8) []const u8 {
    comptime {
        var out: []const u8 = root_name;
        for (path) |seg| {
            out = out ++ " " ++ seg;
        }
        return out;
    }
}

/// Parse argv against the comptime tree.
///
/// Argv layout: argv[0] is the program name (ignored for matching but
/// could be used for usage strings; we use the root's `name` instead).
/// All argv[1..] entries are matched against subcommands, flags, and
/// positionals.
///
/// Returns `Result` on success, or a `Parse` error. On error, `err_out` is
/// populated with the detailed bundle; render it with `formatError`.
pub fn parse(
    comptime root: Cmd,
    argv: []const []const u8,
    err_out: *err_mod.Detail,
) err_mod.Parse!Result(root) {
    return parseImpl(root, argv, err_out);
}

/// Dispatch mode: parse + invoke the matched leaf's `run` callback.
/// Returns the leaf's handler error if it errored. `--help` invokes the
/// help renderer to `writer` and returns successfully. Parse errors are
/// formatted (and flushed) to `writer`, then the original `Parse` error is
/// returned so the caller can decide whether to `std.process.exit(2)`.
pub fn dispatch(
    comptime root: Cmd,
    argv: []const []const u8,
    writer: *std.Io.Writer,
) anyerror!void {
    var detail: err_mod.Detail = undefined;
    const result = parse(root, argv, &detail) catch |e| {
        try err_mod.format(detail, writer);
        // Flush so buffered writers surface the diagnostic before we return
        // the error to the caller; mirror app.run's flush-then-report path.
        writer.flush() catch {};
        return e;
    };
    switch (result) {
        .help => |path| {
            const text = helpForAnyPath(root, path) orelse comptime help_mod.helpText(root, &.{});
            try writer.print("{s}", .{text});
            try writer.flush();
            return;
        },
        .match => |u| {
            try invokeMatch(root, u, writer);
        },
    }
}

// Comptime path-to-help dispatcher. We can't compute help text from a
// runtime path slice, so the dispatcher matches the runtime path against
// every known path at comptime via `inline for`.
fn helpForAnyPath(comptime root: Cmd, runtime_path: []const []const u8) ?[]const u8 {
    if (runtime_path.len == 0) return comptime help_mod.helpText(root, &.{});
    // Walk every node (parents AND leaves) so a path like ["plan"] —
    // which is a parent, not a leaf — renders that group's help instead
    // of falling back to root.
    const nodes = comptime cmd_mod.allNodes(root);
    inline for (nodes) |node| {
        if (pathsEqual(node.path, runtime_path)) {
            return comptime help_mod.helpText(root, node.path);
        }
    }
    return null;
}

fn pathsEqual(a: []const []const u8, b: []const []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |aa, bb| {
        if (!std.mem.eql(u8, aa, bb)) return false;
    }
    return true;
}

fn appendTail(
    tail_buf: [][]const u8,
    tail_len: *usize,
    tok: []const u8,
    err_out: *err_mod.Detail,
) err_mod.Parse!void {
    if (tail_len.* >= tail_buf.len) {
        err_out.* = .{ .kind = err_mod.Parse.UnexpectedArgument, .arg = "too many tokens" };
        return err_mod.Parse.UnexpectedArgument;
    }
    tail_buf[tail_len.*] = tok;
    tail_len.* += 1;
}

fn invokeMatch(
    comptime root: Cmd,
    result_union: ResultUnion(root),
    writer: *std.Io.Writer,
) anyerror!void {
    const leaves = comptime cmd_mod.allLeaves(root);
    inline for (leaves) |leaf| {
        const tag_name = comptime pathToTag(leaf.path);
        if (std.mem.eql(u8, @tagName(std.meta.activeTag(result_union)), tag_name)) {
            if (leaf.cmd.run) |handler_ptr| {
                const handler_fn: cmd_mod.HandlerFn = @ptrCast(@alignCast(handler_ptr));
                const args = @field(result_union, tag_name);
                return handler_fn(@ptrCast(&args));
            } else {
                // No handler — print help for this command and return.
                const text = comptime help_mod.helpText(root, leaf.path);
                try writer.print("{s}", .{text});
                try writer.flush();
                return;
            }
        }
    }
    // The result union was built from one of `leaves`, so exactly one tag
    // matches above. Reaching here means the leaf/tag sets drifted apart.
    unreachable;
}

// =========================================================================
// Implementation
// =========================================================================

fn parseImpl(
    comptime root: Cmd,
    argv: []const []const u8,
    err_out: *err_mod.Detail,
) err_mod.Parse!Result(root) {
    if (argv.len == 0) {
        err_out.* = .{ .kind = err_mod.Parse.UnknownSubcommand, .cmd_path = root.name };
        return err_mod.Parse.UnknownSubcommand;
    }

    // Resolve the command path by consuming argv tokens that match
    // sub-command names. Stop at the first token that doesn't match a
    // child (or is a flag); the remaining tokens are flags + positionals
    // for the matched command.
    //
    // We do this with a comptime-known max depth so the path buffer is
    // stack-allocated.
    const max_depth = comptime treeDepth(root);
    var path_buf: [max_depth][]const u8 = undefined;
    var path_len: usize = 0;
    // Ancestor stack — index `path_len` is the most-recently-resolved
    // node. `ancestors[0]` is always root so inherited-flag lookup can
    // walk from the leaf back up.
    var ancestors: [max_depth + 1]Cmd = undefined;
    ancestors[0] = root;

    // Single linear pass: subcommand tokens are folded into path_buf;
    // everything else (flags + their values, positionals, `--`) is copied
    // into tail_buf. We need the two-buffer split because global flags can
    // appear before, between, or after subcommand tokens — but a subcommand
    // is only recognized when its parent node lists it.
    // Tail buffer is heap-free. The cap applies to copied tail tokens, not
    // argv length, so deeply nested command names do not count against it.
    var tail_buf: [max_tail_tokens][]const u8 = undefined;
    var tail_len: usize = 0;

    var current: Cmd = root;
    var i: usize = 1; // skip argv[0] (program name)
    var passthrough = false; // toggled after `--`
    while (i < argv.len) : (i += 1) {
        const tok = argv[i];
        if (tok.len == 0) continue;

        if (passthrough or tok[0] != '-' or std.mem.eql(u8, tok, "-")) {
            // Try subcommand match unless we're in passthrough or this
            // already looks like a positional (lone `-`).
            if (!passthrough) {
                var matched = false;
                for (current.cmds) |c| {
                    if (commandMatches(c, tok)) {
                        path_buf[path_len] = c.name;
                        path_len += 1;
                        ancestors[path_len] = c;
                        current = c;
                        matched = true;
                        break;
                    }
                }
                if (matched) continue;
            }
            // Not a subcommand → positional/passthrough token for the leaf.
            try appendTail(tail_buf[0..], &tail_len, tok, err_out);
            continue;
        }

        // Flag-shaped token.
        if (std.mem.eql(u8, tok, "--")) {
            try appendTail(tail_buf[0..], &tail_len, tok, err_out);
            passthrough = true;
            continue;
        }

        // Help is handled below; route it through tail like any other flag.
        try appendTail(tail_buf[0..], &tail_len, tok, err_out);

        // If the flag belongs to a known scope and is non-bool, swallow
        // its value so it doesn't get mistaken for a subcommand on the
        // next iteration. Unknown flags will fail later in parseLeaf.
        if (flagWantsValue(ancestors[0 .. path_len + 1], tok)) {
            i += 1;
            if (i < argv.len) {
                try appendTail(tail_buf[0..], &tail_len, argv[i], err_out);
            }
        }
    }

    // Pre-scan tail for --help / -h. Help is global; bail early with the
    // resolved subcommand path so the dispatcher renders the right page.
    // Stop at the `--` terminator: tokens after it are positionals, so
    // `tool take -- --help` passes `--help` through as a value rather than
    // requesting help.
    for (tail_buf[0..tail_len]) |t| {
        if (std.mem.eql(u8, t, "--")) break;
        if (std.mem.eql(u8, t, "--help") or std.mem.eql(u8, t, "-h")) {
            return helpResult(root, path_buf[0..path_len]);
        }
    }

    // Dispatch to a comptime-generated per-leaf parser. We inline-for
    // over every leaf, match the runtime path, then call the leaf's
    // typed parser.
    const leaves = comptime cmd_mod.allLeaves(root);
    inline for (leaves) |leaf| {
        if (pathsEqual(leaf.path, path_buf[0..path_len])) {
            const Args = comptime cmd_mod.ArgsType(root, leaf.path);
            const all_flags = comptime cmd_mod.collectInheritedFlags(root, leaf.path) ++ leaf.cmd.flags;
            const tag_name = comptime pathToTag(leaf.path);
            const cmd_path = comptime commandPathString(root.name, leaf.path);
            const args = parseLeaf(
                Args,
                all_flags,
                leaf.cmd.flag_groups,
                leaf.cmd.positionals,
                tail_buf[0..tail_len],
                leaf.cmd.allow_unknown_flags,
                leaf.cmd.allow_extra_positionals or leaf.cmd.rest_field != null,
                leaf.cmd.rest_field,
                cmd_path,
                err_out,
            ) catch |e| return e;
            var u: ResultUnion(root) = undefined;
            u = @unionInit(ResultUnion(root), tag_name, args);
            return .{ .match = u };
        }
    }

    // No leaf matched. Two sub-cases:
    //
    //   1. `current` is a parent verb (has children) AND the user gave
    //      no extra tokens after it — bare invocation like `tool group`.
    //      Render the parent's help and exit 0, matching Cobra / Go's
    //      convention. Operator decision Q234 (plan 351, 2026-05-26).
    //
    //   2. Otherwise (unrecognized subcommand token, partial-but-typoed
    //      path, etc.) — the existing UnknownSubcommand error stands.
    if (tail_len == 0 and current.cmds.len > 0) {
        return helpResult(root, path_buf[0..path_len]);
    }

    // `i` has reached `argv.len` by the time the resolution loop exits, so the
    // only available token to name is the first tail entry (if any).
    const unknown: ?[]const u8 = if (tail_len > 0) tail_buf[0] else null;
    err_out.* = .{
        .kind = err_mod.Parse.UnknownSubcommand,
        .arg = unknown,
        .cmd_path = current.name,
        .suggestion = if (unknown) |tok| suggestCommandForPath(root, path_buf[0..path_len], tok) else null,
    };
    return err_mod.Parse.UnknownSubcommand;
}

/// Walks every node in `scope` (root → currently-resolved leaf) checking
/// whether `tok` names a non-bool flag declared at any level. Used by the
/// path-resolution loop to know whether to swallow the next argv token as
/// the flag's value.
fn flagWantsValue(scope: []const Cmd, tok: []const u8) bool {
    // Long form `--name`.
    if (tok.len >= 2 and tok[0] == '-' and tok[1] == '-') {
        for (scope) |node| {
            for (node.flags) |f| {
                if (flagLongMatches(f, tok)) return f.kind != .bool;
            }
        }
        return false;
    }
    // Short form `-x`.
    if (tok.len == 2 and tok[0] == '-') {
        for (scope) |node| {
            for (node.flags) |f| {
                if (f.short) |s| if (s == tok[1]) return f.kind != .bool;
            }
        }
        return false;
    }
    return false;
}

fn treeDepth(comptime root: Cmd) usize {
    comptime {
        var max: usize = 0;
        depthWalk(root, 0, &max);
        return @max(max, 1);
    }
}

fn depthWalk(comptime node: Cmd, comptime current: usize, comptime max: *usize) void {
    comptime {
        if (current > max.*) max.* = current;
        for (node.cmds) |c| depthWalk(c, current + 1, max);
    }
}

/// Parse the flag + positional segment of argv for a specific leaf.
/// `Args` is the comptime-generated typed struct.
/// `all_flags` is the flag spec slice (inherited + own).
/// `positionals` is the leaf's positional spec slice.
/// `tail` is the argv slice starting at the first non-subcommand token.
fn parseLeaf(
    comptime Args: type,
    comptime all_flags: []const Flag,
    comptime flag_groups: []const FlagGroup,
    comptime positionals: []const Positional,
    tail: []const []const u8,
    allow_unknown_flags: bool,
    allow_extra_positionals: bool,
    comptime rest_field: ?[]const u8,
    comptime cmd_path: []const u8,
    err_out: *err_mod.Detail,
) err_mod.Parse!Args {
    // Initialize args with field defaults where available; required fields
    // are left undefined and the required-flag check below guarantees they
    // get filled (or the parser errors out before returning).
    var args: Args = undefined;
    inline for (@typeInfo(Args).@"struct".fields) |fld| {
        if (fld.defaultValue()) |dv| {
            @field(args, fld.name) = dv;
        }
    }

    // Reset the shared list-flag accumulator for this parse.
    list_counts = .{0} ** max_list_flags;

    // Reset the deprecated-flag record for this parse. The runner reads it
    // after a successful parse to warn on used-but-deprecated flags.
    deprecated_flags_len = 0;

    // Track which flags have been set so we can detect dupes and required
    // misses. Comptime-sized bitset (well, bool array) because the flag
    // count is comptime-known.
    var seen: [all_flags.len]bool = .{false} ** all_flags.len;

    // Positional collection: comptime-sized array; the parser only fills
    // as many as the leaf declares.
    var pos_filled: usize = 0;

    // Rest-positional capture: extras beyond the declared positionals land
    // here when the leaf opted in via `rest_field`. Backed by a module-level
    // static buffer so the slice stays valid after parseLeaf returns (parse
    // results are consumed before the next CLI invocation in our flow).
    var rest_count: usize = 0;

    var i: usize = 0;
    var seen_double_dash = false;
    while (i < tail.len) : (i += 1) {
        const tok = tail[i];
        if (tok.len == 0) continue;

        // -- terminator: everything after is positional, regardless of dashes.
        if (!seen_double_dash and std.mem.eql(u8, tok, "--")) {
            seen_double_dash = true;
            continue;
        }

        // A dash-prefixed token that is an all-digit negative number (e.g.
        // `-5`) is treated as a positional when the next positional slot is an
        // integer. Without this, `-5` falls into the flag branch and fails as
        // an unknown flag. Flag *values* like `--count -5` are unaffected:
        // they are consumed by the flag handler before reaching this check.
        const neg_num_positional = !seen_double_dash and
            looksLikeNegativeNumber(tok) and
            pos_filled < positionals.len and
            positionals[pos_filled].kind == .int;

        if (!seen_double_dash and tok.len >= 2 and tok[0] == '-' and !neg_num_positional) {
            if (tok.len > 2 and tok[0] == '-' and tok[1] != '-') {
                if (try parseShortExpansion(Args, &args, all_flags, tok, &seen, err_out)) continue;
            }

            // Flag.
            // Comptime guard: when a leaf has zero flag specs, the entire
            // matched-flag branch is dead — but Zig's sema still tries to
            // type-check `all_flags[idx]`, which fails on an empty slice.
            // Hoisting the check elides the branch at comptime.
            const matched = if (all_flags.len == 0) null else matchFlag(all_flags, tok);
            const matched_idx = if (matched) |m| m.idx else null;
            if (all_flags.len > 0 and matched_idx != null) {
                const idx = matched_idx.?;
                const f = all_flags[idx];
                if (!f.list and !f.count and seen[idx]) {
                    err_out.* = .{ .kind = err_mod.Parse.DuplicateFlag, .flag = f.long };
                    return err_mod.Parse.DuplicateFlag;
                }
                seen[idx] = true;
                if (f.deprecated != null) recordDeprecatedFlag(f.long);

                // Count flags take no value; each occurrence increments. They
                // are declared `kind == .bool`, so this must precede the bool
                // branch below.
                if (f.count) {
                    incrementCountFlag(Args, &args, all_flags, idx);
                    continue;
                }

                if (f.kind == .bool) {
                    const value = if (matched.?.negated)
                        false
                    else if (matched.?.inline_value) |raw|
                        parseBoolValue(raw) orelse {
                            err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .flag = f.long, .arg = raw };
                            return err_mod.Parse.InvalidValue;
                        }
                    else
                        true;
                    setFlagValue(Args, &args, all_flags, idx, .{ .bool = value });
                    continue;
                }

                // Non-bool flags accept either `--flag value` or
                // long-form `--flag=value`.
                const raw = if (matched.?.inline_value) |value|
                    value
                else blk: {
                    i += 1;
                    if (i >= tail.len) {
                        err_out.* = .{ .kind = err_mod.Parse.MissingValue, .flag = f.long };
                        return err_mod.Parse.MissingValue;
                    }
                    break :blk tail[i];
                };
                try coerceAndStore(Args, &args, all_flags, idx, f, raw, err_out);
            } else {
                if (allow_unknown_flags) {
                    // Best-effort swallow of unknown flag + one following
                    // non-flag token so legacy invocations still hit command
                    // handlers that print redirect guidance.
                    if (i + 1 < tail.len) {
                        const maybe_val = tail[i + 1];
                        if (!(maybe_val.len >= 1 and maybe_val[0] == '-')) {
                            i += 1;
                        }
                    }
                    continue;
                }
                err_out.* = .{ .kind = err_mod.Parse.UnknownFlag, .arg = tok, .suggestion = suggestFlag(all_flags, tok) };
                return err_mod.Parse.UnknownFlag;
            }
        } else {
            // Positional.
            if (pos_filled >= positionals.len) {
                if (allow_extra_positionals) {
                    if (rest_field != null) {
                        if (rest_count >= rest_buf.len) {
                            err_out.* = .{ .kind = err_mod.Parse.TooManyPositionals, .arg = tok };
                            return err_mod.Parse.TooManyPositionals;
                        }
                        rest_buf[rest_count] = tok;
                        rest_count += 1;
                    }
                    continue;
                }
                err_out.* = .{ .kind = err_mod.Parse.TooManyPositionals, .arg = tok };
                return err_mod.Parse.TooManyPositionals;
            }
            const p = positionals[pos_filled];
            if (p.validator) |validate_fn| {
                if (validate_fn(tok)) |msg| {
                    err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .positional = p.name, .arg = tok, .message = msg };
                    return err_mod.Parse.InvalidValue;
                }
            }
            switch (p.kind) {
                .bool => {
                    // Accept "true"/"false" for bool positionals; rare but
                    // simpler than refusing the kind entirely.
                    const v = if (std.mem.eql(u8, tok, "true")) true else if (std.mem.eql(u8, tok, "false")) false else {
                        err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .positional = p.name, .arg = tok };
                        return err_mod.Parse.InvalidValue;
                    };
                    setPositionalValue(Args, &args, positionals, pos_filled, .{ .bool = v });
                },
                .string => setPositionalValue(Args, &args, positionals, pos_filled, .{ .string = tok }),
                .int => {
                    const v = std.fmt.parseInt(i64, tok, 10) catch {
                        err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .positional = p.name, .arg = tok };
                        return err_mod.Parse.InvalidValue;
                    };
                    setPositionalValue(Args, &args, positionals, pos_filled, .{ .int = v });
                },
                .float => {
                    const v = std.fmt.parseFloat(f64, tok) catch {
                        err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .positional = p.name, .arg = tok };
                        return err_mod.Parse.InvalidValue;
                    };
                    setPositionalValue(Args, &args, positionals, pos_filled, .{ .float = v });
                },
                .path => setPositionalValue(Args, &args, positionals, pos_filled, .{ .path = tok }),
                .duration => {
                    const v = duration_mod.parseNanos(tok) catch {
                        err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .positional = p.name, .arg = tok };
                        return err_mod.Parse.InvalidValue;
                    };
                    setPositionalValue(Args, &args, positionals, pos_filled, .{ .duration = v });
                },
                // Positionals never carry `.choice` (validate rejects it).
                .choice => unreachable,
            }
            pos_filled += 1;
        }
    }

    // Required-flag check.
    inline for (all_flags, 0..) |f, idx| {
        if (f.required and !seen[idx]) {
            err_out.* = .{ .kind = err_mod.Parse.MissingRequired, .flag = f.long };
            return err_mod.Parse.MissingRequired;
        }
    }

    try enforceFlagGroups(all_flags, flag_groups, seen, cmd_path, err_out);

    // Required-positional check.
    inline for (positionals, 0..) |p, idx| {
        if (p.required and idx >= pos_filled) {
            err_out.* = .{ .kind = err_mod.Parse.MissingRequiredPositional, .positional = p.name };
            return err_mod.Parse.MissingRequiredPositional;
        }
    }

    // Publish rest-positional capture into the synthesized Args field.
    if (rest_field) |fname| {
        @field(args, fname) = rest_buf[0..rest_count];
    }

    return args;
}

fn enforceFlagGroups(
    comptime all_flags: []const Flag,
    comptime flag_groups: []const FlagGroup,
    seen: [all_flags.len]bool,
    comptime cmd_path: []const u8,
    err_out: *err_mod.Detail,
) err_mod.Parse!void {
    inline for (flag_groups) |group| {
        var selected_count: usize = 0;
        inline for (group.flags) |member| {
            const idx = comptime flagIndexByLong(all_flags, member) orelse
                @compileError("parser: flag group member was not validated: " ++ member);
            if (seen[idx]) {
                if (selected_count >= group_error_flags_buf.len) {
                    err_out.* = .{
                        .kind = err_mod.Parse.FlagGroupViolation,
                        .group = group.name,
                        .group_mode = group.mode,
                        .group_flags = group.flags,
                        .cmd_path = cmd_path,
                    };
                    return err_mod.Parse.FlagGroupViolation;
                }
                group_error_flags_buf[selected_count] = all_flags[idx].long;
                selected_count += 1;
            }
        }

        const violation = switch (group.mode) {
            .mutually_exclusive => selected_count > 1,
            .required_one => selected_count == 0,
            .required_exactly_one => selected_count != 1,
        };
        if (violation) {
            err_out.* = .{
                .kind = err_mod.Parse.FlagGroupViolation,
                .group = group.name,
                .group_mode = group.mode,
                .group_flags = if (selected_count == 0) group.flags else group_error_flags_buf[0..selected_count],
                .cmd_path = cmd_path,
            };
            return err_mod.Parse.FlagGroupViolation;
        }
    }
}

fn flagIndexByLong(comptime all_flags: []const Flag, comptime long: []const u8) ?usize {
    inline for (all_flags, 0..) |f, idx| {
        if (std.mem.eql(u8, f.long, long)) return idx;
    }
    return null;
}

const MatchedFlag = struct {
    idx: usize,
    inline_value: ?[]const u8 = null,
    negated: bool = false,
};

fn matchFlag(comptime all_flags: []const Flag, tok: []const u8) ?MatchedFlag {
    // Long form: exact match against f.long, or `--long=value` for non-bool
    // flags. Bool equals syntax remains unsupported and falls through to
    // UnknownFlag.
    if (tok.len >= 2 and tok[0] == '-' and tok[1] == '-') {
        inline for (all_flags, 0..) |f, i| {
            if (flagLongMatches(f, tok)) return .{ .idx = i };
            if (f.kind == .bool and !f.count and flagNegationMatches(f, tok)) return .{ .idx = i, .negated = true };
            if (f.kind != .bool) {
                if (std.mem.startsWith(u8, tok, f.long) and tok.len > f.long.len and tok[f.long.len] == '=') {
                    return .{ .idx = i, .inline_value = tok[f.long.len + 1 ..] };
                }
                inline for (f.aliases) |alias| {
                    if (std.mem.startsWith(u8, tok, alias) and tok.len > alias.len and tok[alias.len] == '=') {
                        return .{ .idx = i, .inline_value = tok[alias.len + 1 ..] };
                    }
                }
            }
            if (f.kind == .bool) {
                if (std.mem.startsWith(u8, tok, f.long) and tok.len > f.long.len and tok[f.long.len] == '=') {
                    return .{ .idx = i, .inline_value = tok[f.long.len + 1 ..] };
                }
                inline for (f.aliases) |alias| {
                    if (std.mem.startsWith(u8, tok, alias) and tok.len > alias.len and tok[alias.len] == '=') {
                        return .{ .idx = i, .inline_value = tok[alias.len + 1 ..] };
                    }
                }
            }
        }
        return null;
    }
    // Short form: single dash + one char.
    if (tok.len == 2 and tok[0] == '-') {
        const c = tok[1];
        for (all_flags, 0..) |f, i| {
            if (f.short) |s| if (s == c) return .{ .idx = i };
        }
        return null;
    }
    return null;
}

fn parseShortExpansion(
    comptime Args: type,
    args: *Args,
    comptime all_flags: []const Flag,
    tok: []const u8,
    seen: *[all_flags.len]bool,
    err_out: *err_mod.Detail,
) err_mod.Parse!bool {
    if (all_flags.len == 0) return false;

    const first_idx = matchShortFlag(all_flags, tok[1]) orelse return false;
    const first_flag = all_flags[first_idx];
    if (first_flag.kind != .bool) {
        const raw = tok[2..];
        if (raw.len == 0) return false;
        if (!first_flag.list and seen[first_idx]) {
            err_out.* = .{ .kind = err_mod.Parse.DuplicateFlag, .flag = first_flag.long };
            return err_mod.Parse.DuplicateFlag;
        }
        seen[first_idx] = true;
        if (first_flag.deprecated != null) recordDeprecatedFlag(first_flag.long);
        try coerceAndStore(Args, args, all_flags, first_idx, first_flag, raw, err_out);
        return true;
    }

    var pos: usize = 1;
    while (pos < tok.len) : (pos += 1) {
        const idx = matchShortFlag(all_flags, tok[pos]) orelse return false;
        if (all_flags[idx].kind != .bool) return false;
    }

    pos = 1;
    while (pos < tok.len) : (pos += 1) {
        const idx = matchShortFlag(all_flags, tok[pos]).?;
        const f = all_flags[idx];
        if (f.kind != .bool) return false;
        if (!f.count and seen[idx]) {
            err_out.* = .{ .kind = err_mod.Parse.DuplicateFlag, .flag = f.long };
            return err_mod.Parse.DuplicateFlag;
        }
        seen[idx] = true;
        if (f.deprecated != null) recordDeprecatedFlag(f.long);
        // A count flag in a bundle (`-vvv`) increments once per occurrence.
        if (f.count) {
            incrementCountFlag(Args, args, all_flags, idx);
        } else {
            setFlagValue(Args, args, all_flags, idx, .{ .bool = true });
        }
    }
    return true;
}

fn matchShortFlag(comptime all_flags: []const Flag, short: u8) ?usize {
    inline for (all_flags, 0..) |f, i| {
        if (f.short) |s| if (s == short) return i;
    }
    return null;
}

fn commandMatches(command: Cmd, tok: []const u8) bool {
    if (std.mem.eql(u8, command.name, tok)) return true;
    for (command.aliases) |alias| {
        if (std.mem.eql(u8, alias, tok)) return true;
    }
    return false;
}

fn flagLongMatches(f: Flag, tok: []const u8) bool {
    if (std.mem.eql(u8, f.long, tok)) return true;
    for (f.aliases) |alias| {
        if (std.mem.eql(u8, alias, tok)) return true;
    }
    return false;
}

fn flagNegationMatches(comptime f: Flag, tok: []const u8) bool {
    if (std.mem.startsWith(u8, f.long, "--")) {
        const negated = "--no-" ++ f.long[2..];
        if (std.mem.eql(u8, negated, tok)) return true;
    }
    inline for (f.aliases) |alias| {
        if (std.mem.startsWith(u8, alias, "--")) {
            const negated = "--no-" ++ alias[2..];
            if (std.mem.eql(u8, negated, tok)) return true;
        }
    }
    return false;
}

fn suggestCommandForPath(comptime root: Cmd, path: []const []const u8, tok: []const u8) ?[]const u8 {
    if (path.len == 0) return suggestCommand(root.cmds, tok);
    const nodes = comptime cmd_mod.allNodes(root);
    inline for (nodes) |node| {
        if (pathsEqual(node.path, path)) return suggestCommand(node.cmd.cmds, tok);
    }
    return null;
}

fn suggestCommand(comptime cmds: []const Cmd, tok: []const u8) ?[]const u8 {
    var best: ?[]const u8 = null;
    var best_score: usize = std.math.maxInt(usize);
    inline for (cmds) |c| {
        bestCandidate(tok, c.name, &best, &best_score);
        inline for (c.aliases) |alias| bestCandidate(tok, alias, &best, &best_score);
    }
    return if (best_score <= 2) best else null;
}

fn suggestFlag(comptime flags: []const Flag, tok: []const u8) ?[]const u8 {
    const name = flagSuggestionToken(tok);
    var best: ?[]const u8 = null;
    var best_score: usize = std.math.maxInt(usize);
    inline for (flags) |f| {
        bestCandidate(name, f.long, &best, &best_score);
        inline for (f.aliases) |alias| bestCandidate(name, alias, &best, &best_score);
        if (f.short) |s| {
            const short = "-" ++ &[_]u8{s};
            bestCandidate(name, short, &best, &best_score);
        }
    }
    return if (best_score <= 2) best else null;
}

fn flagSuggestionToken(tok: []const u8) []const u8 {
    if (std.mem.indexOfScalar(u8, tok, '=')) |idx| return tok[0..idx];
    return tok;
}

fn bestCandidate(tok: []const u8, candidate: []const u8, best: *?[]const u8, best_score: *usize) void {
    const score = editDistanceAtMost(tok, candidate, 3) orelse return;
    if (score < best_score.*) {
        best.* = candidate;
        best_score.* = score;
    }
}

fn editDistanceAtMost(a: []const u8, b: []const u8, max: usize) ?usize {
    if (a.len > b.len + max or b.len > a.len + max) return null;
    var previous: [128]usize = undefined;
    var current: [128]usize = undefined;
    if (b.len + 1 > previous.len) return null;

    for (0..b.len + 1) |j| previous[j] = j;
    for (a, 0..) |ac, i| {
        current[0] = i + 1;
        var row_min = current[0];
        for (b, 0..) |bc, j| {
            const cost: usize = if (ac == bc) 0 else 1;
            const deletion = previous[j + 1] + 1;
            const insertion = current[j] + 1;
            const substitution = previous[j] + cost;
            current[j + 1] = @min(@min(deletion, insertion), substitution);
            row_min = @min(row_min, current[j + 1]);
        }
        if (row_min > max) return null;
        for (0..b.len + 1) |j| previous[j] = current[j];
    }
    return if (previous[b.len] <= max) previous[b.len] else null;
}

fn parseBoolValue(raw: []const u8) ?bool {
    if (std.mem.eql(u8, raw, "true")) return true;
    if (std.mem.eql(u8, raw, "false")) return false;
    return null;
}

/// True when `tok` is a dash followed by one or more ASCII digits (`-5`,
/// `-12`). Used to route bare negative numbers to integer positionals instead
/// of the flag branch.
fn looksLikeNegativeNumber(tok: []const u8) bool {
    if (tok.len < 2 or tok[0] != '-') return false;
    for (tok[1..]) |c| {
        if (c < '0' or c > '9') return false;
    }
    return true;
}

/// Coerce a raw non-bool flag value to the flag's kind and store it. Shared by
/// the `--flag value` / `--flag=value` path and the attached-short `-fvalue`
/// path so int parsing and choice-membership validation live in one place.
fn coerceAndStore(
    comptime Args: type,
    args: *Args,
    comptime all_flags: []const Flag,
    idx: usize,
    f: Flag,
    raw: []const u8,
    err_out: *err_mod.Detail,
) err_mod.Parse!void {
    if (f.validator) |validate_fn| {
        if (validate_fn(raw)) |msg| {
            err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .flag = f.long, .arg = raw, .message = msg };
            return err_mod.Parse.InvalidValue;
        }
    }
    if (f.list) {
        // List elements are []const u8 (string/path/choice). Choice lists still
        // enforce membership per item.
        if (f.kind == .choice and !isChoiceMember(f.choices, raw)) {
            err_out.* = .{
                .kind = err_mod.Parse.InvalidValue,
                .flag = f.long,
                .arg = raw,
                .suggestion = nearestChoice(f.choices, raw),
            };
            return err_mod.Parse.InvalidValue;
        }
        return appendListValue(Args, args, all_flags, idx, raw, err_out);
    }
    switch (f.kind) {
        .bool => unreachable,
        .string => setFlagValue(Args, args, all_flags, idx, .{ .string = raw }),
        .path => setFlagValue(Args, args, all_flags, idx, .{ .path = raw }),
        .duration => {
            const v = duration_mod.parseNanos(raw) catch {
                err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .flag = f.long, .arg = raw };
                return err_mod.Parse.InvalidValue;
            };
            setFlagValue(Args, args, all_flags, idx, .{ .duration = v });
        },
        .choice => {
            if (!isChoiceMember(f.choices, raw)) {
                err_out.* = .{
                    .kind = err_mod.Parse.InvalidValue,
                    .flag = f.long,
                    .arg = raw,
                    .suggestion = nearestChoice(f.choices, raw),
                };
                return err_mod.Parse.InvalidValue;
            }
            setFlagValue(Args, args, all_flags, idx, .{ .choice = raw });
        },
        .int => {
            const v = std.fmt.parseInt(i64, raw, 10) catch {
                err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .flag = f.long, .arg = raw };
                return err_mod.Parse.InvalidValue;
            };
            setFlagValue(Args, args, all_flags, idx, .{ .int = v });
        },
        .float => {
            const v = std.fmt.parseFloat(f64, raw) catch {
                err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .flag = f.long, .arg = raw };
                return err_mod.Parse.InvalidValue;
            };
            setFlagValue(Args, args, all_flags, idx, .{ .float = v });
        },
    }
}

/// Append a value to a repeatable flag's row in the module-static `list_buf`
/// and point the field slice at the accumulated values.
fn appendListValue(
    comptime Args: type,
    args: *Args,
    comptime all_flags: []const Flag,
    idx: usize,
    raw: []const u8,
    err_out: *err_mod.Detail,
) err_mod.Parse!void {
    // Only list flags have a `[]const []const u8` field; gate the body on
    // `lf.list` so the inline-for doesn't type-check this assignment against
    // non-list flags (whose fields are bool/[]const u8/…).
    inline for (all_flags, 0..) |lf, i| {
        if (comptime lf.list) {
            if (i == idx) {
                const row = comptime blk: {
                    var r: usize = 0;
                    for (all_flags[0..i]) |g| {
                        if (g.list) r += 1;
                    }
                    break :blk r;
                };
                if (row >= max_list_flags) @compileError("parser: a command exceeds the list-flag limit");
                if (list_counts[row] >= max_list_items) {
                    err_out.* = .{ .kind = err_mod.Parse.UnexpectedArgument, .flag = lf.long, .arg = raw };
                    return err_mod.Parse.UnexpectedArgument;
                }
                list_buf[row][list_counts[row]] = raw;
                list_counts[row] += 1;
                const field_name = comptime flag_mod.flagFieldName(lf);
                @field(args, field_name) = list_buf[row][0..list_counts[row]];
                return;
            }
        }
    }
}

fn isChoiceMember(choices: []const []const u8, raw: []const u8) bool {
    for (choices) |c| {
        if (std.mem.eql(u8, c, raw)) return true;
    }
    return false;
}

fn nearestChoice(choices: []const []const u8, raw: []const u8) ?[]const u8 {
    var best: ?[]const u8 = null;
    var best_score: usize = std.math.maxInt(usize);
    for (choices) |c| bestCandidate(raw, c, &best, &best_score);
    return if (best_score <= 2) best else null;
}

fn setFlagValue(
    comptime Args: type,
    args: *Args,
    comptime all_flags: []const Flag,
    idx: usize,
    runtime_val: flag_mod.Default,
) void {
    // The flag spec slice is comptime-known but `idx` is runtime; an
    // `inline for` unrolls into a switch on idx so each branch sees a
    // comptime field name AND a comptime-narrowed value type.
    inline for (all_flags, 0..) |f, i| {
        // List flags store via appendListValue and count flags via
        // incrementCountFlag; skip both so this scalar assignment isn't
        // type-checked against their slice / u32 fields.
        if (comptime !f.list and !f.count) {
            if (i == idx) {
                const field_name = comptime flag_mod.flagFieldName(f);
                switch (f.kind) {
                    .bool => @field(args, field_name) = runtime_val.bool,
                    .string => @field(args, field_name) = runtime_val.string,
                    .int => @field(args, field_name) = runtime_val.int,
                    .float => @field(args, field_name) = runtime_val.float,
                    .duration => @field(args, field_name) = runtime_val.duration,
                    .path => @field(args, field_name) = runtime_val.path,
                    .choice => @field(args, field_name) = runtime_val.choice,
                }
                return;
            }
        }
    }
}

/// Increment a count flag's `u32` field by one. Like `setFlagValue`, the spec
/// slice is comptime-known but `idx` is runtime, so an `inline for` unrolls
/// into a switch on idx to recover the comptime field name.
fn incrementCountFlag(
    comptime Args: type,
    args: *Args,
    comptime all_flags: []const Flag,
    idx: usize,
) void {
    inline for (all_flags, 0..) |f, i| {
        if (comptime f.count) {
            if (i == idx) {
                const field_name = comptime flag_mod.flagFieldName(f);
                @field(args, field_name) +|= 1;
                return;
            }
        }
    }
}

fn setPositionalValue(
    comptime Args: type,
    args: *Args,
    comptime positionals: []const Positional,
    idx: usize,
    runtime_val: flag_mod.Default,
) void {
    inline for (positionals, 0..) |p, i| {
        if (i == idx) {
            const field_name = comptime flag_mod.positionalFieldName(p);
            switch (p.kind) {
                .bool => @field(args, field_name) = runtime_val.bool,
                .string => @field(args, field_name) = runtime_val.string,
                .int => @field(args, field_name) = runtime_val.int,
                .float => @field(args, field_name) = runtime_val.float,
                .duration => @field(args, field_name) = runtime_val.duration,
                .path => @field(args, field_name) = runtime_val.path,
                .choice => unreachable,
            }
            return;
        }
    }
}

// ---- tests ----

const test_root = Cmd{
    .name = "tool",
    .flags = &.{
        .{ .long = "--verbose", .short = 'v', .kind = .bool, .default = .{ .bool = false } },
    },
    .cmds = &.{
        .{
            .name = "task",
            .cmds = &.{
                .{
                    .name = "add",
                    .flags = &.{
                        .{ .long = "--title", .kind = .string, .required = true },
                        .{ .long = "--priority", .kind = .int, .default = .{ .int = 100 } },
                    },
                    .positionals = &.{
                        .{ .name = "scope", .kind = .string, .required = false },
                    },
                },
            },
        },
    },
};

test "parse: basic flag + required + positional" {
    const argv: []const []const u8 = &.{
        "tool", "task", "add", "--title", "Hello", "--priority", "50", "myscope",
    };
    var detail: err_mod.Detail = undefined;
    const result = try parse(test_root, argv, &detail);
    switch (result) {
        .match => |u| {
            const args = u.task_add;
            try std.testing.expectEqualStrings("Hello", args.title);
            try std.testing.expectEqual(@as(i64, 50), args.priority);
            try std.testing.expect(args.scope != null);
            try std.testing.expectEqualStrings("myscope", args.scope.?);
            try std.testing.expectEqual(false, args.verbose);
        },
        .help => return error.UnexpectedHelp,
    }
}

test "parse: default values fill in" {
    const argv: []const []const u8 = &.{
        "tool", "task", "add", "--title", "OnlyTitle",
    };
    var detail: err_mod.Detail = undefined;
    const result = try parse(test_root, argv, &detail);
    const args = result.match.task_add;
    try std.testing.expectEqual(@as(i64, 100), args.priority);
    try std.testing.expectEqual(false, args.verbose);
    try std.testing.expect(args.scope == null);
}

test "parse: inherited flag flows down" {
    const argv: []const []const u8 = &.{
        "tool", "-v", "task", "add", "--title", "Hello",
    };
    var detail: err_mod.Detail = undefined;
    const result = try parse(test_root, argv, &detail);
    try std.testing.expectEqual(true, result.match.task_add.verbose);
}

test "parse: missing required flag errors" {
    const argv: []const []const u8 = &.{ "tool", "task", "add" };
    var detail: err_mod.Detail = undefined;
    const result = parse(test_root, argv, &detail);
    try std.testing.expectError(err_mod.Parse.MissingRequired, result);
    try std.testing.expect(detail.flag != null);
    try std.testing.expectEqualStrings("--title", detail.flag.?);
}

test "parse: unknown flag errors" {
    const argv: []const []const u8 = &.{ "tool", "task", "add", "--title", "x", "--bogus" };
    var detail: err_mod.Detail = undefined;
    const result = parse(test_root, argv, &detail);
    try std.testing.expectError(err_mod.Parse.UnknownFlag, result);
}

test "parse: --help at root returns help intent" {
    const argv: []const []const u8 = &.{ "tool", "--help" };
    var detail: err_mod.Detail = undefined;
    const result = try parse(test_root, argv, &detail);
    switch (result) {
        .help => |path| try std.testing.expectEqual(@as(usize, 0), path.len),
        .match => return error.ExpectedHelp,
    }
}

test "parse: --help under subcommand returns nested help" {
    const argv: []const []const u8 = &.{ "tool", "task", "add", "--help" };
    var detail: err_mod.Detail = undefined;
    const result = try parse(test_root, argv, &detail);
    switch (result) {
        .help => |path| {
            try std.testing.expectEqual(@as(usize, 2), path.len);
            try std.testing.expectEqualStrings("task", path[0]);
            try std.testing.expectEqualStrings("add", path[1]);
        },
        .match => return error.ExpectedHelp,
    }
}

const choice_root = Cmd{
    .name = "tool",
    .cmds = &.{
        .{
            .name = "run",
            .flags = &.{
                .{ .long = "--format", .short = 'f', .kind = .choice, .choices = &.{ "json", "text", "yaml" }, .default = .{ .choice = "text" } },
            },
        },
    },
};

test "parse: choice flag accepts a declared value (long, =, short forms)" {
    var detail: err_mod.Detail = undefined;
    try std.testing.expectEqualStrings("json", (try parse(choice_root, &.{ "tool", "run", "--format", "json" }, &detail)).match.run.format);
    try std.testing.expectEqualStrings("yaml", (try parse(choice_root, &.{ "tool", "run", "--format=yaml" }, &detail)).match.run.format);
    try std.testing.expectEqualStrings("json", (try parse(choice_root, &.{ "tool", "run", "-fjson" }, &detail)).match.run.format);
    try std.testing.expectEqualStrings("text", (try parse(choice_root, &.{ "tool", "run", "-f", "text" }, &detail)).match.run.format);
}

test "parse: choice default applies when the flag is absent" {
    var detail: err_mod.Detail = undefined;
    try std.testing.expectEqualStrings("text", (try parse(choice_root, &.{ "tool", "run" }, &detail)).match.run.format);
}

test "parse: choice flag rejects an undeclared value with a suggestion" {
    var detail: err_mod.Detail = undefined;
    try std.testing.expectError(err_mod.Parse.InvalidValue, parse(choice_root, &.{ "tool", "run", "--format", "jsonn" }, &detail));
    try std.testing.expectEqualStrings("jsonn", detail.arg.?);
    try std.testing.expectEqualStrings("json", detail.suggestion.?);
}

const list_root = Cmd{
    .name = "tool",
    .cmds = &.{
        .{ .name = "build", .flags = &.{
            .{ .long = "--tag", .kind = .string, .list = true },
            .{ .long = "--mode", .kind = .choice, .choices = &.{ "fast", "slow" }, .list = true },
        } },
    },
};

test "parse: list flag accumulates repeats and defaults to empty" {
    var detail: err_mod.Detail = undefined;
    const r = try parse(list_root, &.{ "tool", "build", "--tag", "a", "--tag", "b", "--mode", "fast" }, &detail);
    try std.testing.expectEqual(@as(usize, 2), r.match.build.tag.len);
    try std.testing.expectEqualStrings("a", r.match.build.tag[0]);
    try std.testing.expectEqualStrings("b", r.match.build.tag[1]);
    try std.testing.expectEqual(@as(usize, 1), r.match.build.mode.len);
    try std.testing.expectEqualStrings("fast", r.match.build.mode[0]);

    const empty = try parse(list_root, &.{ "tool", "build" }, &detail);
    try std.testing.expectEqual(@as(usize, 0), empty.match.build.tag.len);

    // A choice list still enforces membership per item.
    try std.testing.expectError(err_mod.Parse.InvalidValue, parse(list_root, &.{ "tool", "build", "--mode", "nope" }, &detail));
}

fn validatePort(v: []const u8) ?[]const u8 {
    const n = std.fmt.parseInt(u32, v, 10) catch return "must be a number";
    if (n > 65535) return "port out of range";
    return null;
}

const validator_root = Cmd{
    .name = "tool",
    .cmds = &.{
        .{
            .name = "serve",
            .flags = &.{
                .{ .long = "--port", .kind = .int, .validator = validatePort },
            },
        },
    },
};

test "parse: custom validator accepts valid input and rejects with a message" {
    var detail: err_mod.Detail = undefined;
    try std.testing.expectEqual(@as(i64, 8080), (try parse(validator_root, &.{ "tool", "serve", "--port", "8080" }, &detail)).match.serve.port);
    try std.testing.expectError(err_mod.Parse.InvalidValue, parse(validator_root, &.{ "tool", "serve", "--port", "99999" }, &detail));
    try std.testing.expectEqualStrings("port out of range", detail.message.?);
}

const float_root = Cmd{
    .name = "tool",
    .cmds = &.{
        .{
            .name = "scale",
            .flags = &.{
                .{ .long = "--rate", .kind = .float, .default = .{ .float = 1.0 } },
            },
            .positionals = &.{
                .{ .name = "factor", .kind = .float, .required = false },
            },
        },
    },
};

test "parse: float flag and positional accept decimals; default applies" {
    var detail: err_mod.Detail = undefined;
    const r = try parse(float_root, &.{ "tool", "scale", "--rate", "2.5", "3.5" }, &detail);
    try std.testing.expectEqual(@as(f64, 2.5), r.match.scale.rate);
    try std.testing.expectEqual(@as(f64, 3.5), r.match.scale.factor.?);
    try std.testing.expectEqual(@as(f64, 1.0), (try parse(float_root, &.{ "tool", "scale" }, &detail)).match.scale.rate);
    try std.testing.expectError(err_mod.Parse.InvalidValue, parse(float_root, &.{ "tool", "scale", "--rate", "abc" }, &detail));
}

test "parse: positional default applies when the slot is omitted" {
    const pos_def_root = Cmd{
        .name = "tool",
        .cmds = &.{
            .{ .name = "run", .positionals = &.{
                .{ .name = "mode", .kind = .string, .required = false, .default = .{ .string = "fast" } },
            } },
        },
    };
    var detail: err_mod.Detail = undefined;
    try std.testing.expectEqualStrings("fast", (try parse(pos_def_root, &.{ "tool", "run" }, &detail)).match.run.mode);
    try std.testing.expectEqualStrings("slow", (try parse(pos_def_root, &.{ "tool", "run", "slow" }, &detail)).match.run.mode);
}

const dur_path_root = Cmd{
    .name = "tool",
    .cmds = &.{
        .{
            .name = "watch",
            .flags = &.{
                .{ .long = "--interval", .kind = .duration, .default = .{ .duration = 600 * std.time.ns_per_s } },
                .{ .long = "--config", .kind = .path },
            },
        },
    },
};

test "parse: duration parses units with default; path stores the raw value" {
    var detail: err_mod.Detail = undefined;
    const r = try parse(dur_path_root, &.{ "tool", "watch", "--interval", "10m", "--config", "/etc/x.conf" }, &detail);
    try std.testing.expectEqual(@as(u64, 10 * std.time.ns_per_min), r.match.watch.interval);
    try std.testing.expectEqualStrings("/etc/x.conf", r.match.watch.config.?);
    try std.testing.expectEqual(@as(u64, 600 * std.time.ns_per_s), (try parse(dur_path_root, &.{ "tool", "watch" }, &detail)).match.watch.interval);
    try std.testing.expectError(err_mod.Parse.InvalidValue, parse(dur_path_root, &.{ "tool", "watch", "--interval", "nope" }, &detail));
}

const neg_root = Cmd{
    .name = "calc",
    .cmds = &.{
        .{
            .name = "add",
            .positionals = &.{
                .{ .name = "delta", .kind = .int, .required = true },
            },
        },
    },
};

test "parse: bare negative-number positional is accepted" {
    const argv: []const []const u8 = &.{ "calc", "add", "-5" };
    var detail: err_mod.Detail = undefined;
    const result = try parse(neg_root, argv, &detail);
    try std.testing.expectEqual(@as(i64, -5), result.match.add.delta);
}

test "parse: negative number after -- is still a positional" {
    const argv: []const []const u8 = &.{ "calc", "add", "--", "-12" };
    var detail: err_mod.Detail = undefined;
    const result = try parse(neg_root, argv, &detail);
    try std.testing.expectEqual(@as(i64, -12), result.match.add.delta);
}

test "parse: --help after -- is a positional, not a help request" {
    // test_root's `task add` has an optional string positional `scope`.
    const argv: []const []const u8 = &.{ "tool", "task", "add", "--title", "x", "--", "--help" };
    var detail: err_mod.Detail = undefined;
    const result = try parse(test_root, argv, &detail);
    switch (result) {
        .match => |u| {
            try std.testing.expect(u.task_add.scope != null);
            try std.testing.expectEqualStrings("--help", u.task_add.scope.?);
        },
        .help => return error.UnexpectedHelp,
    }
}

// Writes over the stack region a returning `parseImpl` frame would have
// occupied, so a regression to the old stack-local help path (use-after-
// return) corrupts the slice and fails the assertions below.
noinline fn clobberStack() void {
    var buf: [4096]usize = undefined;
    for (&buf, 0..) |*slot, idx| slot.* = idx *% 2654435761;
    std.mem.doNotOptimizeAway(&buf);
}

test "parse: help path survives stack churn (no use-after-return)" {
    const argv: []const []const u8 = &.{ "tool", "task", "add", "--help" };
    var detail: err_mod.Detail = undefined;
    const result = try parse(test_root, argv, &detail);
    clobberStack();
    switch (result) {
        .help => |path| {
            try std.testing.expectEqual(@as(usize, 2), path.len);
            try std.testing.expectEqualStrings("task", path[0]);
            try std.testing.expectEqualStrings("add", path[1]);
        },
        .match => return error.ExpectedHelp,
    }
}
