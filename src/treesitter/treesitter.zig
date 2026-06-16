//! tree-sitter C-API bindings (vendored runtime + Zig grammar).
//!
//! This module is the linkage anchor and the single `@cImport` funnel for
//! the vendored tree-sitter C runtime (`vendor/tree-sitter/`) and the
//! vendored Zig grammar (`vendor/tree-sitter-zig/`). Following the lesson
//! from `src/lua/lua.zig`, all C-API access flows through ONE cImport here
//! so the whole project shares a single, consistent set of C types
//! (`TSParser`, `TSTree`, `TSNode`, `TSLanguage`); reaching into the C
//! namespace from multiple modules would mint incompatible duplicate types.
//!
//! Scope (M2.1): vendor + build wiring + a parse smoke test. This module
//! deliberately exposes only a minimal `parse` helper and the grammar
//! handle. Symbol resolution, the reference walk, token weighting, and the
//! `closures` store are later M2 milestones and live elsewhere
//! (`src/engine/closure/`); they will import this module to reach the API.
//!
//! The smoke test below links the static library and parses a fixture Zig
//! snippet, proving the runtime + grammar object code is actually exercised
//! by `zig build test` (not a vacuous compile-only check).

const std = @import("std");

/// The single C-API import. The grammar's `tree_sitter_zig` symbol is
/// declared as an extern fn (it is defined in the vendored grammar's
/// `parser.c`, linked via the `treesitter` static library) rather than
/// imported, since it has no C header.
pub const c = @cImport({
    @cInclude("tree_sitter/api.h");
});

/// The Zig-language grammar entry point, defined in the vendored
/// `vendor/tree-sitter-zig/src/parser.c` and linked into the static lib.
/// Returns the grammar's `TSLanguage` table.
pub extern fn tree_sitter_zig() callconv(.c) *const c.TSLanguage;

/// Re-exported C node type so callers do not have to reach into `c`.
pub const Node = c.TSNode;

/// A parsed tree plus the parser that produced it. `deinit` frees both.
/// The owner must keep the source buffer alive for the tree's lifetime if
/// it intends to read node text (node positions index back into the source).
pub const Parsed = struct {
    parser: *c.TSParser,
    tree: *c.TSTree,

    /// Frees the tree and the parser.
    pub fn deinit(self: *Parsed) void {
        c.ts_tree_delete(self.tree);
        c.ts_parser_delete(self.parser);
    }

    /// The tree's root node.
    pub fn root(self: Parsed) Node {
        return c.ts_tree_root_node(self.tree);
    }
};

/// Errors the minimal parse helper can surface.
pub const ParseError = error{
    /// `ts_parser_new` returned null (allocation failure).
    ParserAllocFailed,
    /// `ts_parser_set_language` rejected the grammar (ABI version mismatch
    /// between the runtime and the grammar).
    LanguageRejected,
    /// `ts_parser_parse_string` returned null (parse produced no tree).
    ParseFailed,
};

/// Parses `source` as Zig using the vendored grammar, returning a `Parsed`
/// the caller must `deinit`. `source` length must fit in u32 (tree-sitter's
/// length type); callers parsing larger buffers are out of scope for M2.1.
pub fn parse(source: []const u8) ParseError!Parsed {
    const parser = c.ts_parser_new() orelse return error.ParserAllocFailed;
    errdefer c.ts_parser_delete(parser);

    if (!c.ts_parser_set_language(parser, tree_sitter_zig())) {
        return error.LanguageRejected;
    }

    const tree = c.ts_parser_parse_string(
        parser,
        null,
        source.ptr,
        @intCast(source.len),
    ) orelse return error.ParseFailed;

    return .{ .parser = parser, .tree = tree };
}

/// The C string node type, as a Zig slice.
pub fn nodeType(node: Node) []const u8 {
    return std.mem.span(c.ts_node_type(node));
}

test "treesitter smoke: parse Zig fixture to a non-trivial tree" {
    // Fixture: a const decl and a function decl — both top-level Zig
    // constructs the grammar must recognise.
    const fixture = "const x = 1;\nfn f() void {}\n";

    var parsed = try parse(fixture);
    defer parsed.deinit();

    const root = parsed.root();
    // The root must not be null and must be the grammar's top node.
    try std.testing.expect(!c.ts_node_is_null(root));
    try std.testing.expectEqualStrings("source_file", nodeType(root));

    // The fixture has two top-level declarations, so the root must have
    // children — a vacuous (empty) parse would have zero.
    try std.testing.expect(c.ts_node_child_count(root) > 0);
}
