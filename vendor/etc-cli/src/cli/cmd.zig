//! The `Cmd` tree-node and the comptime generators that turn it into typed
//! result structs.
//!
//! Cmd is pure comptime data: trees are declared as nested struct literals
//! and live in `.rodata`. Children are stored by-value in `[]const Cmd`, so
//! trees freely compose by slicing/concatenating.
//!
//! The handler pointer is type-erased (`?*const anyopaque`) because each
//! command's typed args struct is different. The dispatcher reconstructs
//! the typed function pointer at the call site via `ArgsType(root, path)`
//! and `@ptrCast`.

const std = @import("std");
const flag = @import("flag.zig");
const doc_mod = @import("doc.zig");
const meta_mod = @import("meta.zig");

/// One node in the command tree.
///
/// `cmds` and `flags` are sliced so a sub-tree can be defined as a separate
/// `const` and pulled in by reference (`.cmds = subtree.cmds` etc.).
///
/// `run` is type-erased; use `handler()` below to wrap a typed function.
pub const Cmd = struct {
    name: []const u8,
    aliases: []const []const u8 = &.{},
    hidden: bool = false,
    deprecated: ?meta_mod.Deprecation = null,
    /// Short one-line description used in subcommand listings (the
    /// COMMANDS table on the parent's help page) AND as the fallback
    /// lead-in on the command's own help page when `long_desc` is
    /// empty.
    desc: []const u8 = "",
    /// Optional multi-line prose lead-in printed at the top of this
    /// command's help page. When non-empty, the help renderer prints
    /// `long_desc` instead of `desc` in the lead-in block — `desc`
    /// stays as the one-liner the parent's COMMANDS table shows.
    /// Used to carry verb-level documentation (status lifecycles,
    /// allowed kinds, resolution rules) that doesn't fit a one-liner.
    long_desc: []const u8 = "",
    flags: []const flag.Flag = &.{},
    positionals: []const flag.Positional = &.{},
    /// Manual-only metadata used by documentation generators. This does not
    /// affect parser behavior or generated ArgsType fields.
    doc: doc_mod.Doc = .{},
    /// When true, parseLeaf ignores unknown `-x` / `--long` tokens for this
    /// leaf command (and consumes one following value token when present).
    allow_unknown_flags: bool = false,
    /// When true, parseLeaf ignores extra positional tokens beyond the
    /// declared positionals for this leaf command.
    allow_extra_positionals: bool = false,
    /// When non-null, extra positional tokens beyond the declared
    /// `positionals` are collected into a synthesized Args field named
    /// `rest_field` (type `[]const []const u8`, default empty slice).
    /// The slice borrows directly from argv, so the lifetime matches the
    /// parser's input lifetime (i.e. process argv — valid for the whole run).
    ///
    /// Implies `allow_extra_positionals = true`.
    rest_field: ?[]const u8 = null,
    cmds: []const Cmd = &.{},
    /// Type-erased pointer to a `fn (ArgsType(root, path_to_this)) anyerror!void`.
    /// Use `cli.handler(myFn)` to assign. The dispatcher checks the signature
    /// at comptime via the tree topology.
    run: ?*const anyopaque = null,
};

/// The concrete shape every handler takes: an opaque pointer to the leaf's
/// `ArgsType(root, path)` value plus an error union return. Handlers cast
/// the opaque pointer to the correct typed struct themselves; the typed
/// helper `castArgs(...)` is the canonical way to do that.
///
/// The opaque-pointer signature exists to break the `root → handler →
/// ArgsType(root) → root` dependency loop: with a concrete signature,
/// taking `&handleFn` does not force the compiler to resolve a type that
/// transitively references `root`.
pub const HandlerFn = *const fn (args_ptr: *const anyopaque) anyerror!void;

/// Wrap a handler function as a type-erased pointer for `Cmd.run`.
///
/// Takes `anytype` so we can pre-flight the signature with targeted
/// `@compileError` messages for wrong arity / wrong return type / a
/// non-opaque parameter that doesn't trigger the dep loop.
///
/// **Important caveat about typed-args drift.** If you write
/// `fn handle(args: ArgsType(root, …)) anyerror!void` and pass it here,
/// Zig won't reach the checks in this function — resolving `handle`'s
/// signature requires `root`, but evaluating `root.cmds[i].run` requires
/// `&handle`, which requires the signature. The compiler reports this as
/// `error: dependency loop with length 3` naming all three links. The fix
/// is the opaque-pointer signature:
///
///   fn handle(args_ptr: *const anyopaque) anyerror!void {
///       const args = cli.castArgs(root, &.{ "verb", "subverb" }, args_ptr);
///       // …
///   }
///
/// See the negative-case examples at the bottom of cmd.zig.
///
/// **Why opaque-pointer**: see the doc on `HandlerFn`. With a concrete
/// signature, `&handle` resolves without touching `root`; the body's
/// `ArgsType` reference is deferred until the function is actually
/// compiled for execution.
pub fn handler(comptime func: anytype) *const anyopaque {
    const T = @TypeOf(func);
    const info = @typeInfo(T);
    if (info != .@"fn") @compileError(
        "cli.handler: expected a function, got `" ++ @typeName(T) ++ "`.",
    );
    const fn_info = info.@"fn";

    if (fn_info.params.len != 1) @compileError(
        "cli.handler: handler must take exactly one parameter " ++
            "(`*const anyopaque`). Got " ++ std.fmt.comptimePrint("{d}", .{fn_info.params.len}) ++
            " parameters. Recover the typed args via `cli.castArgs(root, path, args_ptr)` " ++
            "inside the body.",
    );

    const param_type = fn_info.params[0].type orelse @compileError(
        "cli.handler: handler parameter must have a concrete type (`*const anyopaque`).",
    );
    if (param_type != *const anyopaque) @compileError(
        "cli.handler: handler parameter must be `*const anyopaque`, got `" ++
            @typeName(param_type) ++ "`. The opaque-pointer indirection breaks the " ++
            "`root → handler → ArgsType(root) → root` dependency loop. Use " ++
            "`fn handleX(args_ptr: *const anyopaque) anyerror!void` and recover " ++
            "the typed args via `cli.castArgs(root, &.{ … }, args_ptr)` inside the body.",
    );

    const ret = fn_info.return_type orelse @compileError(
        "cli.handler: handler must have a return type (`anyerror!void`).",
    );
    if (ret != anyerror!void) @compileError(
        "cli.handler: handler must return `anyerror!void`, got `" ++ @typeName(ret) ++ "`.",
    );

    return @ptrCast(&func);
}

/// Recover the typed args struct from a handler's opaque-pointer arg.
/// Pass the same `(root, path)` you used in `Cmd.run = cli.handler(...)`.
pub inline fn castArgs(
    comptime root: Cmd,
    comptime path: []const []const u8,
    args_ptr: *const anyopaque,
) ArgsType(root, path) {
    const typed: *const ArgsType(root, path) = @ptrCast(@alignCast(args_ptr));
    return typed.*;
}

// =========================================================================
// Comptime tree traversal
// =========================================================================

/// Find a command in the tree by its name path. Returns `null` if the path
/// does not resolve. Both `root` and `path` are comptime-known so the lookup
/// resolves at compile time.
pub fn findCmd(comptime root: Cmd, comptime path: []const []const u8) ?Cmd {
    comptime {
        var current = root;
        for (path) |seg| {
            var matched = false;
            for (current.cmds) |c| {
                if (std.mem.eql(u8, c.name, seg)) {
                    current = c;
                    matched = true;
                    break;
                }
            }
            if (!matched) return null;
        }
        return current;
    }
}

/// A `(path, cmd)` pair returned by `allLeaves`. Declared as a named type
/// so anonymous-struct identity doesn't drift between call sites.
pub const Leaf = struct {
    path: []const []const u8,
    cmd: Cmd,
};

/// Comptime: gather every node in the tree (leaves AND parents),
/// excluding the root itself. Used by help dispatch so `planar plan
/// --help` resolves to the plan-group page, not the root page.
pub fn allNodes(comptime root: Cmd) []const Leaf {
    comptime {
        var out: []const Leaf = &.{};
        out = walkNodes(root, &.{}, out);
        return out;
    }
}

fn walkNodes(
    comptime node: Cmd,
    comptime prefix: []const []const u8,
    comptime acc: []const Leaf,
) []const Leaf {
    comptime {
        var out = acc;
        if (prefix.len > 0) {
            out = out ++ &[_]Leaf{.{ .path = prefix, .cmd = node }};
        }
        for (node.cmds) |child| {
            const new_prefix = prefix ++ &[_][]const u8{child.name};
            out = walkNodes(child, new_prefix, out);
        }
        return out;
    }
}

/// Comptime: gather every leaf command (any cmd with `run != null` OR no
/// children). Returns a slice of `{path, cmd}` pairs.
pub fn allLeaves(comptime root: Cmd) []const Leaf {
    comptime {
        var out: []const Leaf = &.{};
        out = walkLeaves(root, &.{}, out);
        return out;
    }
}

fn walkLeaves(
    comptime node: Cmd,
    comptime prefix: []const []const u8,
    comptime acc: []const Leaf,
) []const Leaf {
    comptime {
        // Treat as leaf when:
        //   - it has a handler, OR
        //   - it has no children (terminal regardless of handler presence).
        const is_leaf = node.run != null or node.cmds.len == 0;
        var out = acc;
        if (is_leaf and prefix.len > 0) {
            out = out ++ &[_]Leaf{.{ .path = prefix, .cmd = node }};
        }
        for (node.cmds) |child| {
            const new_prefix = prefix ++ &[_][]const u8{child.name};
            out = walkLeaves(child, new_prefix, out);
        }
        return out;
    }
}

// =========================================================================
// Args struct generation
// =========================================================================

/// Generate the typed argument struct for a command at `path` under `root`,
/// including inherited flags from every ancestor command.
///
/// The struct has one field per flag (typed per `Kind`) and one field per
/// positional. Flag fields use `flagFieldName`; positional fields use
/// `positionalFieldName`. Optional flags without defaults are wrapped in
/// `?T`; flags with defaults or `required = true` use `T` directly with
/// the default populated by the parser.
pub fn ArgsType(comptime root: Cmd, comptime path: []const []const u8) type {
    @setEvalBranchQuota(20_000_000);
    const target = comptime findCmd(root, path) orelse @compileError(
        "ArgsType: no command at path",
    );
    const inherited = comptime collectInheritedFlags(root, path);
    const owned = target.flags;
    const positionals = target.positionals;
    const has_rest = target.rest_field != null;
    const rest_count: usize = if (has_rest) 1 else 0;

    const N = inherited.len + owned.len + positionals.len + rest_count;

    comptime var names: [N][]const u8 = undefined;
    comptime var types: [N]type = undefined;
    comptime var attrs: [N]std.builtin.Type.StructField.Attributes = undefined;

    comptime var idx: usize = 0;
    // Inherited flag fields first (root → leaf order).
    for (inherited) |f| {
        fillFlagField(f, &names[idx], &types[idx], &attrs[idx]);
        idx += 1;
    }
    // Then this command's own flags.
    for (owned) |f| {
        fillFlagField(f, &names[idx], &types[idx], &attrs[idx]);
        idx += 1;
    }
    // Then positionals.
    for (positionals) |p| {
        fillPositionalField(p, &names[idx], &types[idx], &attrs[idx]);
        idx += 1;
    }
    // Finally the rest-capture field (if requested). Default to an empty
    // slice so a leaf with no extras still has a well-formed slice.
    if (has_rest) {
        const empty_slice: []const []const u8 = &.{};
        names[idx] = target.rest_field.?;
        types[idx] = []const []const u8;
        attrs[idx] = .{ .default_value_ptr = @ptrCast(&empty_slice) };
        idx += 1;
    }

    return @Struct(.auto, null, &names, &types, &attrs);
}

fn fillFlagField(
    comptime f: flag.Flag,
    name_out: *[]const u8,
    type_out: *type,
    attrs_out: *std.builtin.Type.StructField.Attributes,
) void {
    const T = flag.ValueType(f.kind);
    const FieldT = if (f.required or f.default != null) T else ?T;
    const default_value: ?*const anyopaque = blk: {
        if (f.default) |d| {
            const v: T = switch (f.kind) {
                .bool => d.bool,
                .string => d.string,
                .int => d.int,
            };
            const wrapped: FieldT = v;
            break :blk @ptrCast(&wrapped);
        }
        if (f.required) break :blk null;
        const null_val: FieldT = null;
        break :blk @ptrCast(&null_val);
    };
    name_out.* = flag.flagFieldName(f);
    type_out.* = FieldT;
    attrs_out.* = .{ .default_value_ptr = default_value };
}

fn fillPositionalField(
    comptime p: flag.Positional,
    name_out: *[]const u8,
    type_out: *type,
    attrs_out: *std.builtin.Type.StructField.Attributes,
) void {
    const T = flag.ValueType(p.kind);
    const FieldT = if (p.required) T else ?T;
    const default_value: ?*const anyopaque = blk: {
        if (p.required) break :blk null;
        const null_val: FieldT = null;
        break :blk @ptrCast(&null_val);
    };
    name_out.* = flag.positionalFieldName(p);
    type_out.* = FieldT;
    attrs_out.* = .{ .default_value_ptr = default_value };
}

/// Collect inherited flag specs from every ancestor of the command at `path`.
/// Root flags come first; immediate parent last. Excludes the target's own
/// flags.
pub fn collectInheritedFlags(comptime root: Cmd, comptime path: []const []const u8) []const flag.Flag {
    comptime {
        if (path.len == 0) return &.{};
        var out: []const flag.Flag = root.flags;
        var current = root;
        // Walk ancestors, stopping BEFORE the leaf.
        for (path[0 .. path.len - 1]) |seg| {
            for (current.cmds) |c| {
                if (std.mem.eql(u8, c.name, seg)) {
                    out = out ++ c.flags;
                    current = c;
                    break;
                }
            }
        }
        return out;
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

test "findCmd resolves nested paths" {
    const found = comptime findCmd(test_root, &.{ "task", "add" });
    try std.testing.expect(found != null);
    try std.testing.expectEqualStrings("add", found.?.name);
}

test "findCmd returns null on miss" {
    const miss = comptime findCmd(test_root, &.{ "task", "delete" });
    try std.testing.expect(miss == null);
}

test "allLeaves enumerates leaf commands with paths" {
    const leaves = comptime allLeaves(test_root);
    try std.testing.expectEqual(@as(usize, 1), leaves.len);
    try std.testing.expectEqualStrings("add", leaves[0].cmd.name);
    try std.testing.expectEqualStrings("task", leaves[0].path[0]);
    try std.testing.expectEqualStrings("add", leaves[0].path[1]);
}

test "ArgsType has flag, positional, and inherited fields" {
    const A = ArgsType(test_root, &.{ "task", "add" });
    const info = @typeInfo(A).@"struct";
    // verbose (inherited) + title + priority + scope = 4 fields
    try std.testing.expectEqual(@as(usize, 4), info.fields.len);
    // Field names present (order: inherited, owned, positionals)
    try std.testing.expectEqualStrings("verbose", info.fields[0].name);
    try std.testing.expectEqualStrings("title", info.fields[1].name);
    try std.testing.expectEqualStrings("priority", info.fields[2].name);
    try std.testing.expectEqualStrings("scope", info.fields[3].name);
}

test "ArgsType field types reflect Kind" {
    const A = ArgsType(test_root, &.{ "task", "add" });
    const info = @typeInfo(A).@"struct";
    try std.testing.expectEqual(bool, info.fields[0].type); // verbose
    try std.testing.expectEqual([]const u8, info.fields[1].type); // title (required → no Optional)
    try std.testing.expectEqual(i64, info.fields[2].type); // priority (has default)
    try std.testing.expectEqual(?[]const u8, info.fields[3].type); // scope (not required)
}

// =========================================================================
// Handler-shape guard tests.
//
// `cli.handler` enforces the opaque-pointer signature so authors don't
// drift back to typed-args handlers — that drift would trigger the
// `root → handler → ArgsType(root) → root` dependency loop and produce
// a cryptic error from the compiler. The compile-time checks in
// `handler` turn that into a targeted message.
//
// These tests cover the positive case (the canonical shape compiles
// and wraps cleanly). The negative cases — typed args, wrong arity,
// wrong return — live in comments below: there is no `@compileFail`
// builtin, but invoking them by hand is the standard way to verify the
// error messages stay actionable.
// =========================================================================

fn goodHandler(args_ptr: *const anyopaque) anyerror!void {
    _ = args_ptr;
}

test "handler: opaque-pointer signature wraps cleanly" {
    const ptr = handler(goodHandler);
    // Round-trip the pointer through the HandlerFn type to confirm the
    // contract holds — same trick the dispatcher uses.
    const back: HandlerFn = @ptrCast(@alignCast(ptr));
    _ = back;
}

test "handler: castArgs recovers the typed args struct" {
    const Args = ArgsType(test_root, &.{ "task", "add" });
    var args: Args = undefined;
    args.title = "hello";
    args.priority = 99;
    args.verbose = true;
    args.scope = null;
    const recovered = castArgs(test_root, &.{ "task", "add" }, @ptrCast(&args));
    try std.testing.expectEqualStrings("hello", recovered.title);
    try std.testing.expectEqual(@as(i64, 99), recovered.priority);
    try std.testing.expectEqual(true, recovered.verbose);
    try std.testing.expect(recovered.scope == null);
}

// NEGATIVE CASES (drift detection) — kept here for human verification.
// There's no `@compileFail` builtin in Zig, so flip an `if (false)` to
// `if (true)` locally to confirm each error message stays clear.
//
//   if (false) {
//       // Wrong arity → our @compileError:
//       const badArity = struct {
//           fn f(a: *const anyopaque, b: u8) anyerror!void { _ = a; _ = b; }
//       }.f;
//       _ = handler(badArity);
//       // → "cli.handler: handler must take exactly one parameter …"
//
//       // Wrong return → our @compileError:
//       const badReturn = struct {
//           fn f(args_ptr: *const anyopaque) void { _ = args_ptr; }
//       }.f;
//       _ = handler(badReturn);
//       // → "cli.handler: handler must return `anyerror!void`, got `void`."
//
//       // Wrong param type (no root ref) → our @compileError:
//       const badParam = struct {
//           fn f(n: u32) anyerror!void { _ = n; }
//       }.f;
//       _ = handler(badParam);
//       // → "cli.handler: handler parameter must be `*const anyopaque`, got `u32`. …"
//   }
//
// The fourth drift mode — typed args via `ArgsType(root, …)` — does NOT
// reach our @compileError. Zig emits `error: dependency loop with length 3`
// naming the (root → verb.run → handler.signature → root) cycle. See the
// handler() doc comment for the canonical fix.
