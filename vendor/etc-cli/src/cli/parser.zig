//! Runtime parser.
//!
//! `parse(root, path, argv)` walks argv against the tree starting at `root`,
//! returns a `Result` carrying the matched command path and a typed
//! `ArgsType` for whichever leaf matched (via a tagged union over all
//! leaves). Strings in the result are slices into argv; no allocation.
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

const Cmd = cmd_mod.Cmd;
const Flag = flag_mod.Flag;
const Positional = flag_mod.Positional;

/// Module-static backing buffer for rest-positional capture (the leaf's
/// `rest_field`). Lives at module scope so the slice returned in Args stays
/// valid after parseLeaf returns. Single-threaded by construction: each
/// `parse`/`dispatch` call consumes its rest before the next CLI invocation.
/// 256 is well past any realistic argv positional count.
var rest_buf: [256][]const u8 = undefined;

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

/// Parse a single error detail bundle. Returned alongside an error code from
/// `parse`; the caller can render via `err_mod.format`.
pub const ParseErr = struct {
    code: err_mod.Parse,
    detail: err_mod.Detail,
};

/// Parse argv against the comptime tree.
///
/// Argv layout: argv[0] is the program name (ignored for matching but
/// could be used for usage strings; we use the root's `name` instead).
/// All argv[1..] entries are matched against subcommands, flags, and
/// positionals.
///
/// Returns `Result` on success, or a `Parse` error. On error, the caller
/// must check `last_error` for the detailed bundle.
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
/// formatted to `writer` then returned as `error.ParseFailed` so the
/// caller can decide whether to `std.process.exit(2)`.
pub fn dispatch(
    comptime root: Cmd,
    argv: []const []const u8,
    writer: *std.Io.Writer,
) anyerror!void {
    var detail: err_mod.Detail = undefined;
    const result = parse(root, argv, &detail) catch |e| {
        try err_mod.format(detail, writer);
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
    // Tail buffer is heap-free: capped at argv.len because we copy a
    // subset of argv entries. Hard cap of 256 tokens is well past any
    // realistic CLI invocation; exceeding it is a configuration smell.
    const tail_cap = 256;
    if (argv.len > tail_cap) {
        err_out.* = .{ .kind = err_mod.Parse.UnexpectedArgument, .arg = "too many tokens" };
        return err_mod.Parse.UnexpectedArgument;
    }
    var tail_buf: [tail_cap][]const u8 = undefined;
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
            tail_buf[tail_len] = tok;
            tail_len += 1;
            continue;
        }

        // Flag-shaped token.
        if (std.mem.eql(u8, tok, "--")) {
            tail_buf[tail_len] = tok;
            tail_len += 1;
            passthrough = true;
            continue;
        }

        // Help is handled below; route it through tail like any other flag.
        tail_buf[tail_len] = tok;
        tail_len += 1;

        // If the flag belongs to a known scope and is non-bool, swallow
        // its value so it doesn't get mistaken for a subcommand on the
        // next iteration. Unknown flags will fail later in parseLeaf.
        if (flagWantsValue(ancestors[0 .. path_len + 1], tok)) {
            i += 1;
            if (i < argv.len) {
                tail_buf[tail_len] = argv[i];
                tail_len += 1;
            }
        }
    }

    // Pre-scan tail for --help / -h. Help is global; bail early with the
    // resolved subcommand path so the dispatcher renders the right page.
    for (tail_buf[0..tail_len]) |t| {
        if (std.mem.eql(u8, t, "--help") or std.mem.eql(u8, t, "-h")) {
            return .{ .help = path_buf[0..path_len] };
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
            const args = parseLeaf(
                Args,
                all_flags,
                leaf.cmd.positionals,
                tail_buf[0..tail_len],
                leaf.cmd.allow_unknown_flags,
                leaf.cmd.allow_extra_positionals or leaf.cmd.rest_field != null,
                leaf.cmd.rest_field,
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
    //      no extra tokens after it — bare invocation like `planar plan`.
    //      Render the parent's help and exit 0, matching Cobra / Go's
    //      convention. Operator decision Q234 (plan 351, 2026-05-26).
    //
    //   2. Otherwise (unrecognized subcommand token, partial-but-typoed
    //      path, etc.) — the existing UnknownSubcommand error stands.
    if (tail_len == 0 and current.cmds.len > 0) {
        return .{ .help = path_buf[0..path_len] };
    }

    const unknown = if (tail_len > 0) tail_buf[0] else if (i < argv.len) argv[i] else null;
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
    comptime positionals: []const Positional,
    tail: []const []const u8,
    allow_unknown_flags: bool,
    allow_extra_positionals: bool,
    comptime rest_field: ?[]const u8,
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

        if (!seen_double_dash and tok.len >= 2 and tok[0] == '-') {
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
                if (seen[idx]) {
                    err_out.* = .{ .kind = err_mod.Parse.DuplicateFlag, .flag = f.long };
                    return err_mod.Parse.DuplicateFlag;
                }
                seen[idx] = true;

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
                switch (f.kind) {
                    .bool => unreachable,
                    .string => setFlagValue(Args, &args, all_flags, idx, .{ .string = raw }),
                    .int => {
                        const v = std.fmt.parseInt(i64, raw, 10) catch {
                            err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .flag = f.long, .arg = raw };
                            return err_mod.Parse.InvalidValue;
                        };
                        setFlagValue(Args, &args, all_flags, idx, .{ .int = v });
                    },
                }
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
            if (f.kind == .bool and flagNegationMatches(f, tok)) return .{ .idx = i, .negated = true };
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
        if (seen[first_idx]) {
            err_out.* = .{ .kind = err_mod.Parse.DuplicateFlag, .flag = first_flag.long };
            return err_mod.Parse.DuplicateFlag;
        }
        seen[first_idx] = true;
        switch (first_flag.kind) {
            .bool => unreachable,
            .string => setFlagValue(Args, args, all_flags, first_idx, .{ .string = raw }),
            .int => {
                const v = std.fmt.parseInt(i64, raw, 10) catch {
                    err_out.* = .{ .kind = err_mod.Parse.InvalidValue, .flag = first_flag.long, .arg = raw };
                    return err_mod.Parse.InvalidValue;
                };
                setFlagValue(Args, args, all_flags, first_idx, .{ .int = v });
            },
        }
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
        if (seen[idx]) {
            err_out.* = .{ .kind = err_mod.Parse.DuplicateFlag, .flag = f.long };
            return err_mod.Parse.DuplicateFlag;
        }
        seen[idx] = true;
        setFlagValue(Args, args, all_flags, idx, .{ .bool = true });
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
        if (i == idx) {
            const field_name = comptime flag_mod.flagFieldName(f);
            switch (f.kind) {
                .bool => @field(args, field_name) = runtime_val.bool,
                .string => @field(args, field_name) = runtime_val.string,
                .int => @field(args, field_name) = runtime_val.int,
            }
            return;
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
