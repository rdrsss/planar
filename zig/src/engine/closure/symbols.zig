//! engine.closure.symbols — M2.2 symbol resolution (modify-set).
//!
//! Given a seed `.zig` file (the path of a `task_touch_paths` row plus its
//! source text), parse it with the tree-sitter Zig grammar and collect the
//! **modify-set**: the set of *top-level* declarations the file itself
//! defines, expressed as **qualified symbol names**.
//!
//! ## Qualified-name scheme
//!
//! A symbol's qualified name is `<file-stem>.<decl-name>`, where `<file-stem>`
//! is the seed path's basename with the `.zig` extension stripped. So
//! `src/engine/closure/symbols.zig` declaring `pub fn resolve(...)` yields the
//! qualified name `symbols.resolve`. The stem is used (not the full path)
//! because the modify-set is *per file*; the repo-relative path travels
//! alongside on the `Symbol` (and, later, in the `closures.path` column) so a
//! consumer can still disambiguate two files with the same stem.
//!
//! Test declarations have no Zig identifier — their "name" is the test's
//! string literal. They are qualified as `<file-stem>.test:<string>` so they
//! cannot collide with a real declaration named after the string.
//!
//! ## What counts as a top-level modify-set member
//!
//! Only the **direct named children of `source_file`** that introduce a
//! declaration: `variable_declaration` (covers `const`/`var`, including
//! `const X = struct/enum/union/opaque {…}` type decls), `function_declaration`
//! (`pub`/non-`pub` `fn`), and `test_declaration`.
//!
//! Nested declarations — e.g. methods and constants inside a `const X =
//! struct {…}` body — are deliberately **NOT** separate modify-set members.
//! They belong to their enclosing type symbol: editing a struct method is an
//! edit *of that struct*. Collapsing the struct to one unit keeps the closure
//! at the granularity the seed (`task_touch_paths`) operates on — a path, not
//! a sub-symbol — and avoids double-counting a type's surface area when the
//! reference walk (M2.3) and weighting (M2.4) run. The `kind` field still
//! records whether a `variable_declaration` resolves to a container type
//! (struct/enum/union/opaque) vs. a plain binding, so a later milestone can
//! refine granularity without re-parsing.

const std = @import("std");
const ts = @import("treesitter");

/// The category of a top-level declaration. Mirrors the tree-sitter-zig
/// node types we key on, collapsed to the distinctions the closure cares
/// about. `container_*` variants are `variable_declaration`s whose RHS is a
/// type declaration; `binding` is a plain `const`/`var`.
pub const Kind = enum {
    function,
    binding,
    container_struct,
    container_enum,
    container_union,
    container_opaque,
    test_block,
};

/// One member of the modify-set: a top-level declaration of the seed file.
///
/// `name` and `qualified` are owned by the `Symbols` arena that produced
/// this value; they live until the owning `Symbols.deinit`. Byte/line
/// positions index back into the source buffer the caller passed to
/// `resolve` and are retained for later token weighting (M2.4).
pub const Symbol = struct {
    /// The bare declaration name (e.g. `resolve`). For a test block this is
    /// the test's string literal text (without quotes).
    name: []const u8,
    /// The qualified name (e.g. `symbols.resolve`). See the module doc for
    /// the scheme.
    qualified: []const u8,
    /// Declaration category.
    kind: Kind,
    /// Whether the declaration is `pub`. Tests and private decls are `false`.
    is_pub: bool,
    /// Byte offset of the declaration's first byte in the source buffer.
    start_byte: u32,
    /// Byte offset one past the declaration's last byte.
    end_byte: u32,
    /// 1-based line number of the declaration's first byte.
    start_line: u32,
};

/// The resolved modify-set plus the arena backing its strings. Call
/// `deinit` to free both the `list` slice and every name it points at.
pub const Symbols = struct {
    arena: std.heap.ArenaAllocator,
    list: []const Symbol,

    /// Frees the arena (and therefore the symbol list and all its strings).
    pub fn deinit(self: *Symbols) void {
        self.arena.deinit();
    }
};

/// Errors `resolve` can surface beyond the underlying parse errors.
pub const Error = ts.ParseError || std.mem.Allocator.Error;

/// Resolves the modify-set of a seed `.zig` file.
///
/// `path` is the seed file's path (used only to derive the file stem for the
/// qualified-name scheme; not opened). `source` is the file's text and must
/// outlive nothing — the returned `Symbols` copies every string into its own
/// arena, so the caller may free `source` immediately after.
///
/// The caller owns the returned `Symbols` and must `deinit` it.
pub fn resolve(gpa: std.mem.Allocator, path: []const u8, source: []const u8) Error!Symbols {
    var parsed = try ts.parse(source);
    defer parsed.deinit();

    var arena = std.heap.ArenaAllocator.init(gpa);
    errdefer arena.deinit();
    const a = arena.allocator();

    const stem = fileStem(path);

    var out: std.ArrayList(Symbol) = .empty;

    const root = parsed.root();
    const child_count = ts.c.ts_node_child_count(root);
    var i: u32 = 0;
    while (i < child_count) : (i += 1) {
        const node = ts.c.ts_node_child(root, i);
        if (!ts.c.ts_node_is_named(node)) continue;
        const ty = ts.nodeType(node);

        if (std.mem.eql(u8, ty, "function_declaration")) {
            const name = childIdentifier(node, source) orelse continue;
            try appendSym(a, &out, stem, node, name, .function, hasPub(node));
        } else if (std.mem.eql(u8, ty, "variable_declaration")) {
            const name = childIdentifier(node, source) orelse continue;
            const kind = bindingKind(node);
            try appendSym(a, &out, stem, node, name, kind, hasPub(node));
        } else if (std.mem.eql(u8, ty, "test_declaration")) {
            const name = testName(node, source) orelse "";
            try appendTest(a, &out, stem, node, name);
        }
        // comptime_declaration / using_namespace_declaration introduce no
        // named top-level symbol — skipped by design.
    }

    return .{ .arena = arena, .list = try out.toOwnedSlice(a) };
}

/// Returns the basename of `path` with a trailing `.zig` removed.
fn fileStem(path: []const u8) []const u8 {
    const base = std.fs.path.basename(path);
    if (std.mem.endsWith(u8, base, ".zig")) return base[0 .. base.len - ".zig".len];
    return base;
}

/// Finds the declaration's name `identifier` — the first *named* child whose
/// type is `identifier`. Returns the source slice or null if absent.
fn childIdentifier(node: ts.Node, source: []const u8) ?[]const u8 {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        if (std.mem.eql(u8, ts.nodeType(child), "identifier")) {
            return nodeText(child, source);
        }
    }
    return null;
}

/// A `test_declaration`'s name is its `string` literal's `string_content`
/// (the text between the quotes). Returns null for an unnamed test block.
fn testName(node: ts.Node, source: []const u8) ?[]const u8 {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        if (std.mem.eql(u8, ts.nodeType(child), "string")) {
            const cn = ts.c.ts_node_child_count(child);
            var j: u32 = 0;
            while (j < cn) : (j += 1) {
                const gc = ts.c.ts_node_child(child, j);
                if (std.mem.eql(u8, ts.nodeType(gc), "string_content")) {
                    return nodeText(gc, source);
                }
            }
        }
    }
    return null;
}

/// True if the declaration has a `pub` token child.
fn hasPub(node: ts.Node) bool {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (std.mem.eql(u8, ts.nodeType(child), "pub")) return true;
    }
    return false;
}

/// Classifies a `variable_declaration` by its RHS: a container type
/// declaration yields the matching `container_*` kind; anything else is a
/// plain `binding`.
fn bindingKind(node: ts.Node) Kind {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        const ty = ts.nodeType(child);
        if (std.mem.eql(u8, ty, "struct_declaration")) return .container_struct;
        if (std.mem.eql(u8, ty, "enum_declaration")) return .container_enum;
        if (std.mem.eql(u8, ty, "union_declaration")) return .container_union;
        if (std.mem.eql(u8, ty, "opaque_declaration")) return .container_opaque;
    }
    return .binding;
}

fn nodeText(node: ts.Node, source: []const u8) []const u8 {
    const start = ts.c.ts_node_start_byte(node);
    const end = ts.c.ts_node_end_byte(node);
    return source[start..end];
}

fn appendSym(
    a: std.mem.Allocator,
    out: *std.ArrayList(Symbol),
    stem: []const u8,
    node: ts.Node,
    name: []const u8,
    kind: Kind,
    is_pub: bool,
) !void {
    const owned_name = try a.dupe(u8, name);
    const qualified = try std.fmt.allocPrint(a, "{s}.{s}", .{ stem, owned_name });
    try out.append(a, .{
        .name = owned_name,
        .qualified = qualified,
        .kind = kind,
        .is_pub = is_pub,
        .start_byte = ts.c.ts_node_start_byte(node),
        .end_byte = ts.c.ts_node_end_byte(node),
        .start_line = ts.c.ts_node_start_point(node).row + 1,
    });
}

fn appendTest(
    a: std.mem.Allocator,
    out: *std.ArrayList(Symbol),
    stem: []const u8,
    node: ts.Node,
    name: []const u8,
) !void {
    const owned_name = try a.dupe(u8, name);
    const qualified = try std.fmt.allocPrint(a, "{s}.test:{s}", .{ stem, owned_name });
    try out.append(a, .{
        .name = owned_name,
        .qualified = qualified,
        .kind = .test_block,
        .is_pub = false,
        .start_byte = ts.c.ts_node_start_byte(node),
        .end_byte = ts.c.ts_node_end_byte(node),
        .start_line = ts.c.ts_node_start_point(node).row + 1,
    });
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// Returns true if the modify-set contains a symbol with the given qualified
/// name and kind.
fn contains(syms: []const Symbol, qualified: []const u8, kind: Kind) bool {
    for (syms) |s| {
        if (std.mem.eql(u8, s.qualified, qualified) and s.kind == kind) return true;
    }
    return false;
}

test "modify-set: mixed top-level decls, struct methods stay nested" {
    // Fixture: a private import binding, a pub struct type with two methods,
    // a private enum type, a pub fn, a private fn, a top-level var, a test.
    const src =
        \\const std = @import("std");
        \\
        \\pub const Foo = struct {
        \\    x: u32,
        \\    pub fn bar(self: Foo) u32 {
        \\        return self.x;
        \\    }
        \\    fn baz() void {}
        \\};
        \\
        \\const Color = enum { red, green };
        \\
        \\pub fn pubFn() void {}
        \\
        \\fn privFn() void {}
        \\
        \\var counter: u32 = 0;
        \\
        \\test "a behavioural test" {}
        \\
    ;

    var syms = try resolve(std.testing.allocator, "src/engine/closure/symbols.zig", src);
    defer syms.deinit();

    const list = syms.list;

    // The hand-verified modify-set: exactly seven top-level members. The two
    // struct methods (Foo.bar, Foo.baz) are NOT members — they are nested in
    // the Foo struct symbol.
    try std.testing.expectEqual(@as(usize, 7), list.len);

    const stem = "symbols";
    try std.testing.expect(contains(list, stem ++ ".std", .binding));
    try std.testing.expect(contains(list, stem ++ ".Foo", .container_struct));
    try std.testing.expect(contains(list, stem ++ ".Color", .container_enum));
    try std.testing.expect(contains(list, stem ++ ".pubFn", .function));
    try std.testing.expect(contains(list, stem ++ ".privFn", .function));
    try std.testing.expect(contains(list, stem ++ ".counter", .binding));
    try std.testing.expect(contains(list, stem ++ ".test:a behavioural test", .test_block));

    // The nested struct methods must NOT appear as their own qualified names.
    for (list) |s| {
        try std.testing.expect(!std.mem.eql(u8, s.name, "bar"));
        try std.testing.expect(!std.mem.eql(u8, s.name, "baz"));
    }
}

test "pub flag and kinds are recorded correctly" {
    const src =
        \\pub fn exported() void {}
        \\fn internal() void {}
        \\pub const Widget = struct {};
        \\const tag = union { a: u8 };
        \\const Op = opaque {};
        \\
    ;
    var syms = try resolve(std.testing.allocator, "thing.zig", src);
    defer syms.deinit();

    for (syms.list) |s| {
        if (std.mem.eql(u8, s.name, "exported")) {
            try std.testing.expect(s.is_pub);
            try std.testing.expectEqual(Kind.function, s.kind);
        } else if (std.mem.eql(u8, s.name, "internal")) {
            try std.testing.expect(!s.is_pub);
        } else if (std.mem.eql(u8, s.name, "Widget")) {
            try std.testing.expect(s.is_pub);
            try std.testing.expectEqual(Kind.container_struct, s.kind);
        } else if (std.mem.eql(u8, s.name, "tag")) {
            try std.testing.expectEqual(Kind.container_union, s.kind);
        } else if (std.mem.eql(u8, s.name, "Op")) {
            try std.testing.expectEqual(Kind.container_opaque, s.kind);
        }
    }
    try std.testing.expectEqual(@as(usize, 5), syms.list.len);
}

test "file stem is derived from basename, qualified name uses it" {
    const src = "pub fn f() void {}\n";
    var syms = try resolve(std.testing.allocator, "/a/b/c/widget.zig", src);
    defer syms.deinit();
    try std.testing.expectEqual(@as(usize, 1), syms.list.len);
    try std.testing.expectEqualStrings("widget.f", syms.list[0].qualified);
    try std.testing.expectEqual(@as(u32, 1), syms.list[0].start_line);
}

test "empty source yields empty modify-set" {
    var syms = try resolve(std.testing.allocator, "empty.zig", "");
    defer syms.deinit();
    try std.testing.expectEqual(@as(usize, 0), syms.list.len);
}
