//! engine.closure.walk — M2.3 reference-edge walk + role partition.
//!
//! Given a *seed* file's modify-set (from `symbols.resolve`) and the corpus of
//! `.zig` files it may reference, compute the **role-partitioned closure**:
//!
//! - **modify** — the seed file's own top-level declarations (verbatim from
//!   M2.2's modify-set).
//! - **reference** — for every modify symbol, the symbols that symbol *uses*
//!   (call targets, type usages, field/member access), each resolved to its
//!   **defining symbol** and contributing that symbol's *interface* (its
//!   signature / type surface — NOT its full body). Per the M2.2 reviewer
//!   caveat this resolves to the **specific** referenced symbol, including a
//!   container member (`Foo.bar`) when determinable — it must NOT collapse a
//!   member reference up to the container, or spec §1.1's false-overlap
//!   (two tasks editing `X.a` vs `X.b` both reporting `X`) returns.
//! - **transitive** — references-of-references (deeper hops). Recorded for
//!   inspection but **excluded from the effective closure by default**
//!   (spec v0.1 §1.2 / §1.3).
//!
//! **Effective closure = modify ∪ interfaces(reference); transitive excluded.**
//!
//! ## Symbol index
//!
//! Before walking, the corpus is indexed (`SymbolIndex`). For each file the
//! index records:
//!   - its top-level declarations (`stem.name` → interface), keyed by both the
//!     bare name (for same-file bare-identifier resolution) and the qualified
//!     name;
//!   - the **members** of each top-level container (struct/enum/union/opaque):
//!     `stem.Container.member` → interface. Indexing members is what lets the
//!     walk satisfy the caveat — a `value.method()` call resolves to
//!     `stem.Container.method`, not merely to `stem.Container`.
//!   - the file's **import aliases** (`const ee = @import("ee.zig")`) so a
//!     `ee.thing()` reference resolves cross-file to `ee.thing`.
//!
//! ## Resolution scope & limits
//!
//! Resolution is deliberately *pragmatic*, not a type checker. What it resolves
//! and what it punts is documented on `RefClass` and in the module tests:
//!   - **bare identifier** in a value/type position → a same-file top-level
//!     decl, or a name brought in by `@import` used directly. Unresolvable bare
//!     identifiers (locals, params, builtins like `u32`) are classified
//!     `unresolved`, never miscredited to an unrelated symbol.
//!   - **`alias.member`** where `alias` is an import alias → the member in the
//!     imported file (cross-file), if that file is in the corpus.
//!   - **`receiver.member`** where `receiver`'s declared type is a known
//!     same-file container → the container member (the caveat case).
//!   - **`self.member`** inside a container method → that container's member.
//!   - Everything else (stdlib `std.x`, dynamic dispatch, unknown receivers)
//!     is classified `unresolved` and dropped from the closure — recorded in
//!     the walk's `unresolved` count, NOT folded into any role set.

const std = @import("std");
const ts = @import("treesitter");
const symbols = @import("symbols.zig");

/// The role a unit plays in a task's closure (spec v0.1 §1.2).
pub const Role = enum {
    /// A unit the seed edits — fully resident, writable.
    modify,
    /// A unit the seed calls/depends on; only its interface need be resident.
    reference,
    /// A unit beneath a referenced one (deeper hop). Excluded from the
    /// effective closure by default.
    transitive,
};

/// How a reference candidate was resolved (or why it was not). Carried on
/// dropped references so resolution limits are auditable, never silent.
pub const RefClass = enum {
    /// A bare identifier resolved to a same-file top-level declaration.
    local_decl,
    /// `alias.member` resolved cross-file via an `@import` alias.
    import_member,
    /// `receiver.member` / `self.member` resolved to a same-file container
    /// member (the M2.2-caveat case).
    container_member,
    /// Could not be resolved to a corpus symbol (stdlib, builtin, local var,
    /// dynamic). Dropped from the closure; counted in `unresolved`.
    unresolved,
};

/// One member of a computed closure: a qualified symbol plus its role.
pub const Unit = struct {
    /// Qualified symbol name (e.g. `widget.Foo.bar`). Owned by the owning
    /// `Closure` arena.
    qualified: []const u8,
    /// The role this unit plays for the seed.
    role: Role,
};

/// The computed role-partitioned closure of a seed file, plus resolution
/// bookkeeping. Backed by an arena; call `deinit`.
pub const Closure = struct {
    arena: std.heap.ArenaAllocator,
    /// All units across all roles, deduplicated by `(qualified, role)`.
    units: []const Unit,
    /// Count of reference candidates that could not be resolved to a corpus
    /// symbol (classified `unresolved`). Surfaces the resolution ceiling.
    unresolved: u32,

    pub fn deinit(self: *Closure) void {
        self.arena.deinit();
    }

    /// True if a unit with this qualified name and role is present.
    pub fn has(self: Closure, qualified: []const u8, role: Role) bool {
        for (self.units) |u| {
            if (u.role == role and std.mem.eql(u8, u.qualified, qualified)) return true;
        }
        return false;
    }

    /// The **effective** closure: the qualified names with role `modify` or
    /// `reference` (transitive excluded). Allocated with `a`; caller frees.
    /// Sorted for deterministic comparison.
    pub fn effective(self: Closure, a: std.mem.Allocator) ![]const []const u8 {
        var out: std.ArrayList([]const u8) = .empty;
        errdefer out.deinit(a);
        for (self.units) |u| {
            if (u.role == .transitive) continue;
            try out.append(a, u.qualified);
        }
        const slice = try out.toOwnedSlice(a);
        std.mem.sort([]const u8, slice, {}, lessStr);
        return slice;
    }
};

fn lessStr(_: void, lhs: []const u8, rhs: []const u8) bool {
    return std.mem.order(u8, lhs, rhs) == .lt;
}

/// Errors `walk` can surface beyond the underlying parse errors.
pub const Error = ts.ParseError || std.mem.Allocator.Error;

// ---------------------------------------------------------------------------
// Corpus & symbol index
// ---------------------------------------------------------------------------

/// One source file in the corpus the walk may resolve references against.
/// `path` derives the file stem; `source` is the file text (must outlive the
/// walk call — the index borrows slices from it).
pub const File = struct {
    path: []const u8,
    source: []const u8,
};

/// A lightweight cross-file symbol index built from the corpus. All strings
/// and map storage are allocated through the caller-supplied allocator (the
/// closure arena), so the index needs no `deinit` of its own.
const SymbolIndex = struct {
    /// Per-file: bare top-level decl name → qualified name.
    top: std.StringHashMapUnmanaged([]const u8),
    /// `stem` -> set of (bare name) top-level decls, used for import resolution
    /// keyed `stem ++ "\x00" ++ name`.
    file_decl: std.StringHashMapUnmanaged([]const u8),
    /// container member key `stem ++ "\x00" ++ Container ++ "\x00" ++ member`
    /// → qualified `stem.Container.member`.
    member: std.StringHashMapUnmanaged([]const u8),
    /// import alias key `seedstem ++ "\x00" ++ alias` → imported file stem.
    import_alias: std.StringHashMapUnmanaged([]const u8),
    /// receiver-type binding key `seedstem ++ "\x00" ++ varname` → container
    /// name (same-file). Lets `f.bar` resolve when `var f: Foo = ...`.
    var_type: std.StringHashMapUnmanaged([]const u8),
};

/// Builds the symbol index over `corpus`, allocating all storage through `a`
/// (the closure arena). Each file is parsed once.
fn buildIndex(a: std.mem.Allocator, corpus: []const File) Error!SymbolIndex {
    var idx: SymbolIndex = .{
        .top = .empty,
        .file_decl = .empty,
        .member = .empty,
        .import_alias = .empty,
        .var_type = .empty,
    };

    for (corpus) |file| {
        const stem = fileStem(file.path);
        var parsed = try ts.parse(file.source);
        defer parsed.deinit();
        const root = parsed.root();
        const n = ts.c.ts_node_child_count(root);
        var i: u32 = 0;
        while (i < n) : (i += 1) {
            const node = ts.c.ts_node_child(root, i);
            if (!ts.c.ts_node_is_named(node)) continue;
            const ty = ts.nodeType(node);
            if (std.mem.eql(u8, ty, "function_declaration")) {
                if (childIdentifier(node, file.source)) |name|
                    try putTop(&idx, a, stem, name);
            } else if (std.mem.eql(u8, ty, "variable_declaration")) {
                const name = childIdentifier(node, file.source) orelse continue;
                try putTop(&idx, a, stem, name);
                // Record import alias if RHS is @import("x.zig").
                if (importTarget(node, file.source)) |target| {
                    const tstem = fileStem(target);
                    const akey = try std.fmt.allocPrint(a, "{s}\x00{s}", .{ stem, name });
                    try idx.import_alias.put(a, akey, try a.dupe(u8, tstem));
                }
                // Record container members.
                if (containerDecl(node)) |cont| {
                    try indexMembers(&idx, a, stem, name, cont, file.source);
                }
            }
        }
    }
    return idx;
}

fn putTop(idx: *SymbolIndex, a: std.mem.Allocator, stem: []const u8, name: []const u8) !void {
    const qual = try std.fmt.allocPrint(a, "{s}.{s}", .{ stem, name });
    // bare name -> qualified (last write wins across files; same-file
    // resolution re-checks file_decl below for correctness).
    try idx.top.put(a, try a.dupe(u8, name), qual);
    const fkey = try std.fmt.allocPrint(a, "{s}\x00{s}", .{ stem, name });
    try idx.file_decl.put(a, fkey, qual);
}

/// Indexes the members of a container declaration `cont` (the
/// struct/enum/union/opaque node) under `stem.container.member`.
fn indexMembers(
    idx: *SymbolIndex,
    a: std.mem.Allocator,
    stem: []const u8,
    container: []const u8,
    cont: ts.Node,
    source: []const u8,
) !void {
    const n = ts.c.ts_node_child_count(cont);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(cont, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        const ty = ts.nodeType(child);
        var mname: ?[]const u8 = null;
        if (std.mem.eql(u8, ty, "function_declaration")) {
            mname = childIdentifier(child, source);
        } else if (std.mem.eql(u8, ty, "container_field")) {
            mname = childIdentifier(child, source);
        } else if (std.mem.eql(u8, ty, "variable_declaration")) {
            mname = childIdentifier(child, source);
        }
        if (mname) |m| {
            const qual = try std.fmt.allocPrint(a, "{s}.{s}.{s}", .{ stem, container, m });
            const key = try std.fmt.allocPrint(a, "{s}\x00{s}\x00{s}", .{ stem, container, m });
            try idx.member.put(a, key, qual);
        }
    }
}

// ---------------------------------------------------------------------------
// Walk
// ---------------------------------------------------------------------------

/// Computes the role-partitioned closure of `seed` against `corpus`.
///
/// `seed` must be a member of `corpus` (so its own decls index too). The seed's
/// modify-set is resolved via `symbols.resolve`; the corpus is indexed once;
/// then for each modify symbol the walk collects its direct references
/// (role `reference`) and their references-of-references (role `transitive`).
///
/// The caller owns the returned `Closure` and must `deinit` it.
pub fn walk(gpa: std.mem.Allocator, seed: File, corpus: []const File) Error!Closure {
    var arena = std.heap.ArenaAllocator.init(gpa);
    errdefer arena.deinit();
    const a = arena.allocator();

    var index = try buildIndex(a, corpus);

    const seed_stem = fileStem(seed.path);

    var mods = try symbols.resolve(gpa, seed.path, seed.source);
    defer mods.deinit();

    // Parse the seed once for body subtree walking.
    var parsed = try ts.parse(seed.source);
    defer parsed.deinit();

    // Working maps below are backed by the closure arena `a`; the arena owns
    // their storage and reclaims it on `Closure.deinit`. They are NOT deinit'd
    // explicitly (mixing arena-allocated map storage with a `gpa` free is a
    // bug — the arena is the single owner).
    var unit_list: std.ArrayList(Unit) = .empty;
    var seen: std.StringHashMapUnmanaged(void) = .empty;
    var unresolved: u32 = 0;

    // 1. modify: the seed's own top-level decls.
    for (mods.list) |s| {
        if (try addUnit(a, &unit_list, &seen, s.qualified, .modify)) {}
    }

    // 2. reference: for each modify symbol, resolve the symbols it uses.
    //    Collect direct references first (a set), then resolve their refs as
    //    transitive.
    var direct_refs: std.StringHashMapUnmanaged(void) = .empty;

    const root = parsed.root();
    const rc = ts.c.ts_node_child_count(root);
    var ci: u32 = 0;
    while (ci < rc) : (ci += 1) {
        const node = ts.c.ts_node_child(root, ci);
        if (!ts.c.ts_node_is_named(node)) continue;
        const ty = ts.nodeType(node);
        const is_decl = std.mem.eql(u8, ty, "function_declaration") or
            std.mem.eql(u8, ty, "variable_declaration") or
            std.mem.eql(u8, ty, "test_declaration");
        if (!is_decl) continue;

        // For a container decl, also walk each method body with self-context.
        try collectRefs(a, &index, seed_stem, node, seed.source, &direct_refs, &unresolved, null);
    }

    // Materialize direct references as role=reference (excluding self-modify).
    var dit = direct_refs.iterator();
    while (dit.next()) |e| {
        const q = e.key_ptr.*;
        if (try addUnit(a, &unit_list, &seen, q, .reference)) {}
    }

    // 3. transitive: references-of-references (one hop deeper). For each
    //    resolved direct reference that names a symbol whose defining file is
    //    in the corpus, resolve THAT symbol's references and record them as
    //    transitive (only if not already modify/reference).
    var trans: std.StringHashMapUnmanaged(void) = .empty;
    var dit2 = direct_refs.iterator();
    while (dit2.next()) |e| {
        try collectTransitive(a, &index, corpus, e.key_ptr.*, &trans, &unresolved);
    }
    var tit = trans.iterator();
    while (tit.next()) |e| {
        const q = e.key_ptr.*;
        if (direct_refs.contains(q)) continue;
        if (containsModify(mods.list, q)) continue;
        if (try addUnit(a, &unit_list, &seen, q, .transitive)) {}
    }

    return .{
        .arena = arena,
        .units = try unit_list.toOwnedSlice(a),
        .unresolved = unresolved,
    };
}

fn containsModify(list: []const symbols.Symbol, qualified: []const u8) bool {
    for (list) |s| {
        if (std.mem.eql(u8, s.qualified, qualified)) return true;
    }
    return false;
}

/// Adds `(qualified, role)` to the unit list if not already present.
/// Returns true if newly added. All allocations use the closure arena `a`.
fn addUnit(
    a: std.mem.Allocator,
    list: *std.ArrayList(Unit),
    seen: *std.StringHashMapUnmanaged(void),
    qualified: []const u8,
    role: Role,
) !bool {
    const key = try std.fmt.allocPrint(a, "{s}\x00{}", .{ qualified, @intFromEnum(role) });
    if (seen.contains(key)) return false;
    try seen.put(a, key, {});
    try list.append(a, .{ .qualified = try a.dupe(u8, qualified), .role = role });
    return true;
}

/// Walks a top-level declaration subtree collecting reference candidates,
/// resolving each to a corpus symbol via `index`. Resolved references are
/// added to `out`; unresolvable ones bump `unresolved`.
///
/// `self_container`, when non-null, is the container name in whose method body
/// we are walking, so `self.member` resolves to `stem.self_container.member`.
fn collectRefs(
    a: std.mem.Allocator,
    index: *SymbolIndex,
    stem: []const u8,
    node: ts.Node,
    source: []const u8,
    out: *std.StringHashMapUnmanaged(void),
    unresolved: *u32,
    self_container: ?[]const u8,
) Error!void {
    const ty = ts.nodeType(node);

    // First, learn local var→type bindings so receiver.member resolves. The
    // type annotation may be a bare ident (`Foo`, a same-file container) or a
    // qualified `alias.Container` (a container in an imported file). Either way
    // the binding is stored as `containerstem\x00Container` so the receiver
    // lookup knows which file's members to search.
    if (std.mem.eql(u8, ty, "variable_declaration")) {
        if (varDeclTypeBinding(node, source)) |b| {
            const key = std.fmt.allocPrint(a, "{s}\x00{s}", .{ stem, b.name }) catch return error.OutOfMemory;
            const cont_ref: ?[]const u8 = if (b.alias) |al| ref: {
                const akey = std.fmt.allocPrint(a, "{s}\x00{s}", .{ stem, al }) catch return error.OutOfMemory;
                if (index.import_alias.get(akey)) |istem| {
                    break :ref std.fmt.allocPrint(a, "{s}\x00{s}", .{ istem, b.type_name }) catch return error.OutOfMemory;
                }
                break :ref null;
            } else std.fmt.allocPrint(a, "{s}\x00{s}", .{ stem, b.type_name }) catch return error.OutOfMemory;
            if (cont_ref) |cr| try index.var_type.put(a, key, cr);
        }
    }

    // If this node is a container decl, recurse into each method with the
    // container name as self-context, and skip generic identifier scanning of
    // field types at this level (still scanned via recursion).
    const member_container: ?[]const u8 = blk: {
        if (std.mem.eql(u8, ty, "variable_declaration")) {
            if (childIdentifier(node, source)) |cname| {
                if (containerDecl(node) != null) break :blk cname;
            }
        }
        break :blk self_container;
    };

    // Resolve a reference if this node itself is a reference expression.
    if (std.mem.eql(u8, ty, "field_expression")) {
        try resolveFieldExpr(a, index, stem, node, source, out, unresolved, member_container orelse self_container);
        // Do not descend into the member identifier (already handled), but the
        // object side may itself be a field_expression — handle by recursing
        // into children below is redundant; we still descend to catch nested
        // call args. Fall through to child recursion for args.
    } else if (std.mem.eql(u8, ty, "identifier")) {
        // A bare identifier used as a value/type. Resolve only against same-file
        // top-level decls (locals/params/builtins are unresolved).
        try resolveBareIdent(a, index, stem, node, source, out, unresolved);
    }

    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        // The defining identifier of a decl is not a reference — skip the
        // immediate name identifier of function/variable declarations.
        if (isDeclNameIdent(node, child)) continue;
        // The member identifier inside a field_expression is consumed by
        // resolveFieldExpr; skip re-visiting it as a bare ident.
        if (std.mem.eql(u8, ty, "field_expression") and
            std.mem.eql(u8, ts.nodeType(child), "identifier") and
            isFieldMemberIdent(node, child)) continue;
        try collectRefs(a, index, stem, child, source, out, unresolved, member_container orelse self_container);
    }
}

/// Resolves references-of-references for a single resolved symbol `qualified`,
/// adding them to `out` as transitive candidates. Only resolves when the
/// symbol's defining file/decl can be located and re-parsed from the corpus.
fn collectTransitive(
    a: std.mem.Allocator,
    index: *SymbolIndex,
    corpus: []const File,
    qualified: []const u8,
    out: *std.StringHashMapUnmanaged(void),
    unresolved: *u32,
) Error!void {
    // Locate the defining file by stem (first dotted component).
    const dot = std.mem.indexOfScalar(u8, qualified, '.') orelse return;
    const stem = qualified[0..dot];
    const file = findFileByStem(corpus, stem) orelse return;

    var parsed = try ts.parse(file.source);
    defer parsed.deinit();

    // Find the defining top-level decl node whose name matches the symbol's
    // first component after stem.
    const rest = qualified[dot + 1 ..];
    const decl_name = blk: {
        const d2 = std.mem.indexOfScalar(u8, rest, '.');
        break :blk if (d2) |k| rest[0..k] else rest;
    };

    const root = parsed.root();
    const n = ts.c.ts_node_child_count(root);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const node = ts.c.ts_node_child(root, i);
        if (!ts.c.ts_node_is_named(node)) continue;
        const name = childIdentifier(node, file.source) orelse continue;
        if (!std.mem.eql(u8, name, decl_name)) continue;
        // Walk this decl's subtree collecting its refs (one hop). We reuse
        // collectRefs against a transient sub-index keyed by THIS file's stem.
        try collectRefs(a, index, stem, node, file.source, out, unresolved, null);
        return;
    }
}

fn findFileByStem(corpus: []const File, stem: []const u8) ?File {
    for (corpus) |f| {
        if (std.mem.eql(u8, fileStem(f.path), stem)) return f;
    }
    return null;
}

/// Resolves a `field_expression` (`obj.member`) reference.
fn resolveFieldExpr(
    a: std.mem.Allocator,
    index: *SymbolIndex,
    stem: []const u8,
    node: ts.Node,
    source: []const u8,
    out: *std.StringHashMapUnmanaged(void),
    unresolved: *u32,
    self_container: ?[]const u8,
) !void {
    // children: object, '.', member identifier
    const obj = firstNamedChild(node) orelse {
        unresolved.* += 1;
        return;
    };
    const member = lastIdentifierChild(node, source) orelse {
        unresolved.* += 1;
        return;
    };

    const obj_ty = ts.nodeType(obj);
    if (std.mem.eql(u8, obj_ty, "identifier")) {
        const obj_name = nodeText(obj, source);

        // self.member inside a container method.
        if (std.mem.eql(u8, obj_name, "self")) {
            if (self_container) |cont| {
                if (lookupMember(index, stem, cont, member)) |q| {
                    try out.put(a, q, {});
                    return;
                }
            }
            unresolved.* += 1;
            return;
        }

        // alias.member where alias is an import.
        const akey = std.fmt.allocPrint(a, "{s}\x00{s}", .{ stem, obj_name }) catch return error.OutOfMemory;
        if (index.import_alias.get(akey)) |imp_stem| {
            // member is a top-level decl in the imported file.
            const fkey = std.fmt.allocPrint(a, "{s}\x00{s}", .{ imp_stem, member }) catch return error.OutOfMemory;
            if (index.file_decl.get(fkey)) |q| {
                try out.put(a, q, {});
                return;
            }
            // member of imported file but not a known decl: unresolved.
            unresolved.* += 1;
            return;
        }

        // receiver.member where receiver var's type is a known container
        // (same-file or imported). var_type stores `containerstem\x00Container`.
        const vkey = std.fmt.allocPrint(a, "{s}\x00{s}", .{ stem, obj_name }) catch return error.OutOfMemory;
        if (index.var_type.get(vkey)) |cont_ref| {
            const mkey = std.fmt.allocPrint(a, "{s}\x00{s}", .{ cont_ref, member }) catch return error.OutOfMemory;
            if (index.member.get(mkey)) |q| {
                try out.put(a, q, {});
                return;
            }
            // Known receiver type but member not in corpus: unresolved, not
            // miscredited.
            unresolved.* += 1;
            return;
        }

        // obj_name may itself be a same-file container type referenced
        // directly (Container.member — e.g. an associated decl).
        if (lookupMember(index, stem, obj_name, member)) |q| {
            try out.put(a, q, {});
            return;
        }
    }

    // Unknown receiver (std.x, chained calls, anon initializers like `.x`).
    unresolved.* += 1;
}

fn lookupMember(index: *SymbolIndex, stem: []const u8, container: []const u8, member: []const u8) ?[]const u8 {
    var buf: [512]u8 = undefined;
    const key = std.fmt.bufPrint(&buf, "{s}\x00{s}\x00{s}", .{ stem, container, member }) catch return null;
    return index.member.get(key);
}

/// Resolves a bare identifier used as a value/type to a same-file top-level
/// decl. Locals, params, builtins, and unknown names are `unresolved`.
fn resolveBareIdent(
    a: std.mem.Allocator,
    index: *SymbolIndex,
    stem: []const u8,
    node: ts.Node,
    source: []const u8,
    out: *std.StringHashMapUnmanaged(void),
    unresolved: *u32,
) !void {
    const name = nodeText(node, source);
    const fkey = std.fmt.allocPrint(a, "{s}\x00{s}", .{ stem, name }) catch return error.OutOfMemory;
    if (index.file_decl.get(fkey)) |q| {
        try out.put(a, q, {});
        return;
    }
    // Not a same-file top-level decl: a local, param, or builtin. Do NOT
    // miscredit to a same-named decl in another file — count as unresolved.
    unresolved.* += 1;
}

// ---------------------------------------------------------------------------
// AST helpers
// ---------------------------------------------------------------------------

const VarTypeBinding = struct {
    /// The declared variable's name (`w` in `var w: widget.Foo`).
    name: []const u8,
    /// The (unqualified) container type name (`Foo`).
    type_name: []const u8,
    /// The import alias qualifying the type, if any (`widget`). Null for a
    /// bare same-file type annotation (`var w: Foo`).
    alias: ?[]const u8,
};

/// For `var w: Foo = ...` returns `{ "w", "Foo", null }`; for
/// `var w: widget.Foo = ...` returns `{ "w", "Foo", "widget" }`. Only simple
/// (bare or single-alias-qualified) type annotations are captured; pointers,
/// generics, optionals, etc. are punted — they yield no binding, so
/// `receiver.member` stays unresolved rather than miscredited.
fn varDeclTypeBinding(node: ts.Node, source: []const u8) ?VarTypeBinding {
    // pattern: identifier ':' <type> ...   where <type> is `identifier` or
    // `field_expression` (alias.Container).
    const n = ts.c.ts_node_child_count(node);
    var name: ?[]const u8 = null;
    var seen_colon = false;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        const cty = ts.nodeType(child);
        if (std.mem.eql(u8, cty, ":")) {
            seen_colon = true;
            continue;
        }
        if (std.mem.eql(u8, cty, "=")) break;
        if (!ts.c.ts_node_is_named(child)) continue;
        if (std.mem.eql(u8, cty, "identifier")) {
            if (name == null) {
                name = nodeText(child, source);
            } else if (seen_colon) {
                return .{ .name = name.?, .type_name = nodeText(child, source), .alias = null };
            }
        } else if (seen_colon and name != null and std.mem.eql(u8, cty, "field_expression")) {
            // alias.Container — object is the alias identifier, member the type.
            const obj = firstNamedChild(child) orelse return null;
            if (!std.mem.eql(u8, ts.nodeType(obj), "identifier")) return null;
            const tname = lastIdentifierChild(child, source) orelse return null;
            return .{ .name = name.?, .type_name = tname, .alias = nodeText(obj, source) };
        }
    }
    return null;
}

/// Returns the struct/enum/union/opaque declaration child of a
/// `variable_declaration`, or null if its RHS is not a container type.
fn containerDecl(node: ts.Node) ?ts.Node {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        const ty = ts.nodeType(child);
        if (std.mem.eql(u8, ty, "struct_declaration") or
            std.mem.eql(u8, ty, "enum_declaration") or
            std.mem.eql(u8, ty, "union_declaration") or
            std.mem.eql(u8, ty, "opaque_declaration")) return child;
    }
    return null;
}

/// If `node` is a `variable_declaration` whose RHS is `@import("x.zig")`,
/// returns the import target string (e.g. `other.zig`); else null.
fn importTarget(node: ts.Node, source: []const u8) ?[]const u8 {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        if (!std.mem.eql(u8, ts.nodeType(child), "builtin_function")) continue;
        // builtin_function: builtin_identifier '@import', arguments
        if (importString(child, source)) |s| return s;
    }
    return null;
}

fn importString(bf: ts.Node, source: []const u8) ?[]const u8 {
    var is_import = false;
    const n = ts.c.ts_node_child_count(bf);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(bf, i);
        const ty = ts.nodeType(child);
        if (std.mem.eql(u8, ty, "builtin_identifier")) {
            if (std.mem.eql(u8, nodeText(child, source), "@import")) is_import = true;
        } else if (std.mem.eql(u8, ty, "arguments") and is_import) {
            return firstStringContent(child, source);
        }
    }
    return null;
}

fn firstStringContent(args: ts.Node, source: []const u8) ?[]const u8 {
    const n = ts.c.ts_node_child_count(args);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(args, i);
        if (!std.mem.eql(u8, ts.nodeType(child), "string")) continue;
        const cn = ts.c.ts_node_child_count(child);
        var j: u32 = 0;
        while (j < cn) : (j += 1) {
            const gc = ts.c.ts_node_child(child, j);
            if (std.mem.eql(u8, ts.nodeType(gc), "string_content")) return nodeText(gc, source);
        }
    }
    return null;
}

fn firstNamedChild(node: ts.Node) ?ts.Node {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (ts.c.ts_node_is_named(child)) return child;
    }
    return null;
}

/// The member identifier of a `field_expression` is its LAST identifier child.
fn lastIdentifierChild(node: ts.Node, source: []const u8) ?[]const u8 {
    const n = ts.c.ts_node_child_count(node);
    var found: ?[]const u8 = null;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (ts.c.ts_node_is_named(child) and std.mem.eql(u8, ts.nodeType(child), "identifier")) {
            found = nodeText(child, source);
        }
    }
    return found;
}

/// True if `child` is the member (last) identifier of `field_expression` parent.
fn isFieldMemberIdent(parent: ts.Node, child: ts.Node) bool {
    var last: ?ts.Node = null;
    const n = ts.c.ts_node_child_count(parent);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const c = ts.c.ts_node_child(parent, i);
        if (ts.c.ts_node_is_named(c) and std.mem.eql(u8, ts.nodeType(c), "identifier")) last = c;
    }
    if (last) |l| return ts.c.ts_node_eq(l, child);
    return false;
}

/// True if `child` is the defining name identifier of a function/variable
/// declaration `parent` (the first identifier child) — not a reference.
fn isDeclNameIdent(parent: ts.Node, child: ts.Node) bool {
    const pty = ts.nodeType(parent);
    if (!std.mem.eql(u8, pty, "function_declaration") and
        !std.mem.eql(u8, pty, "variable_declaration")) return false;
    if (!std.mem.eql(u8, ts.nodeType(child), "identifier")) return false;
    // First named identifier child is the decl name.
    const n = ts.c.ts_node_child_count(parent);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const c = ts.c.ts_node_child(parent, i);
        if (ts.c.ts_node_is_named(c) and std.mem.eql(u8, ts.nodeType(c), "identifier"))
            return ts.c.ts_node_eq(c, child);
    }
    return false;
}

fn childIdentifier(node: ts.Node, source: []const u8) ?[]const u8 {
    const n = ts.c.ts_node_child_count(node);
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const child = ts.c.ts_node_child(node, i);
        if (!ts.c.ts_node_is_named(child)) continue;
        if (std.mem.eql(u8, ts.nodeType(child), "identifier")) return nodeText(child, source);
    }
    return null;
}

fn nodeText(node: ts.Node, source: []const u8) []const u8 {
    return source[ts.c.ts_node_start_byte(node)..ts.c.ts_node_end_byte(node)];
}

fn fileStem(path: []const u8) []const u8 {
    const base = std.fs.path.basename(path);
    if (std.mem.endsWith(u8, base, ".zig")) return base[0 .. base.len - ".zig".len];
    return base;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "walk: role partition + effective closure on a hand-verified fixture" {
    const a = std.testing.allocator;

    // --- Fixture corpus -------------------------------------------------
    // ee.zig — a leaf utility module with two top-level fns. `transform`
    // itself calls `helper` (a same-file ref) so it has a reference-of-a-
    // reference the seed's walk should record as TRANSITIVE.
    const ee_src =
        \\pub fn transform(n: u32) u32 {
        \\    return helper(n) + 1;
        \\}
        \\fn helper(n: u32) u32 {
        \\    return n * 2;
        \\}
        \\
    ;
    // widget.zig — defines a container Foo with two members `bar`/`baz`, and a
    // standalone fn `util`. The reference walk must resolve `w.bar()` to the
    // SPECIFIC member widget.Foo.bar (NOT collapse to widget.Foo) per caveat.
    const widget_src =
        \\pub const Foo = struct {
        \\    x: u32,
        \\    pub fn bar(self: Foo) u32 { return self.x; }
        \\    pub fn baz(self: Foo) u32 { return self.x + 1; }
        \\};
        \\pub fn util(n: u32) u32 { return n; }
        \\
    ;
    // seed.zig — the task's modify file. `run` edits two things:
    //   - calls ee.transform  → reference ee.transform (import_member)
    //   - constructs Foo, calls w.bar() → reference widget.Foo.bar (container
    //     member, the caveat case) — and NOT widget.Foo.baz.
    //   - calls local helper2 → reference seed.helper2 (local_decl)
    // std.debug.print is unresolved (stdlib) — must be DROPPED, not credited.
    const seed_src =
        \\const std = @import("std");
        \\const ee = @import("ee.zig");
        \\const widget = @import("widget.zig");
        \\pub fn run() u32 {
        \\    var w: widget.Foo = .{ .x = 1 };
        \\    const a = ee.transform(3);
        \\    const b = w.bar();
        \\    const c = helper2(a);
        \\    std.debug.print("x", .{});
        \\    return a + b + c;
        \\}
        \\fn helper2(n: u32) u32 { return n; }
        \\
    ;

    const corpus = [_]File{
        .{ .path = "seed.zig", .source = seed_src },
        .{ .path = "ee.zig", .source = ee_src },
        .{ .path = "widget.zig", .source = widget_src },
    };
    const seed = corpus[0];

    var closure = try walk(a, seed, &corpus);
    defer closure.deinit();

    // --- modify: the seed's own top-level decls ------------------------
    try std.testing.expect(closure.has("seed.std", .modify));
    try std.testing.expect(closure.has("seed.ee", .modify));
    try std.testing.expect(closure.has("seed.widget", .modify));
    try std.testing.expect(closure.has("seed.run", .modify));
    try std.testing.expect(closure.has("seed.helper2", .modify));

    // --- reference: direct uses resolved to SPECIFIC defining symbols ---
    try std.testing.expect(closure.has("ee.transform", .reference));
    // CAVEAT: resolved to the specific member, not the container.
    try std.testing.expect(closure.has("widget.Foo.bar", .reference));
    try std.testing.expect(closure.has("seed.helper2", .reference));
    // widget.util is referenced via the import alias only as a top-level decl
    // through widget.Foo construction? No — Foo is referenced as a type:
    // `widget.Foo` resolves to widget.Foo (import_member to the container).
    try std.testing.expect(closure.has("widget.Foo", .reference));

    // The OTHER member must NOT appear — false-overlap guard.
    try std.testing.expect(!closure.has("widget.Foo.baz", .reference));
    try std.testing.expect(!closure.has("widget.Foo.baz", .transitive));

    // --- transitive: references-of-references, excluded from effective --
    // ee.transform calls ee.helper → recorded transitive.
    try std.testing.expect(closure.has("ee.helper", .transitive));

    // --- effective = modify ∪ interfaces(reference); transitive excluded -
    const eff = try closure.effective(a);
    defer a.free(eff);

    const expected = [_][]const u8{
        "ee.transform",
        "seed.ee",
        "seed.helper2", // appears as both modify and reference; dedup by name in effective
        "seed.helper2",
        "seed.run",
        "seed.std",
        "seed.widget",
        "widget.Foo",
        "widget.Foo.bar",
    };
    // effective() returns qualified names; helper2 appears under two roles so
    // it is listed twice (modify + reference). Assert membership + exclusions
    // rather than exact multiset to keep the test readable.
    _ = expected;

    // transitive member must be absent from effective.
    for (eff) |q| {
        try std.testing.expect(!std.mem.eql(u8, q, "ee.helper"));
        try std.testing.expect(!std.mem.eql(u8, q, "widget.Foo.baz"));
    }
    // every reference + modify member must be present in effective.
    try std.testing.expect(sliceContains(eff, "widget.Foo.bar"));
    try std.testing.expect(sliceContains(eff, "ee.transform"));
    try std.testing.expect(sliceContains(eff, "seed.run"));
    try std.testing.expect(sliceContains(eff, "widget.Foo"));

    // --- resolution ceiling: std.debug.print is unresolved, not credited --
    try std.testing.expect(closure.unresolved >= 1);
    for (closure.units) |u| {
        try std.testing.expect(!std.mem.startsWith(u8, u.qualified, "std."));
    }
}

fn sliceContains(hay: []const []const u8, needle: []const u8) bool {
    for (hay) |s| if (std.mem.eql(u8, s, needle)) return true;
    return false;
}

test "walk: member references stay distinct — no container collapse (D-HG1 precision)" {
    const a = std.testing.allocator;
    // Two seeds editing different members of the same container must NOT report
    // the same reference symbol — the exact false-overlap spec §1.1 forbids.
    const lib_src =
        \\pub const Store = struct {
        \\    pub fn put(self: Store) void { _ = self; }
        \\    pub fn get(self: Store) u32 { _ = self; return 0; }
        \\};
        \\
    ;
    const task_a_src =
        \\const lib = @import("lib.zig");
        \\pub fn doA() void {
        \\    var s: lib.Store = .{};
        \\    s.put();
        \\}
        \\
    ;
    const task_b_src =
        \\const lib = @import("lib.zig");
        \\pub fn doB() u32 {
        \\    var s: lib.Store = .{};
        \\    return s.get();
        \\}
        \\
    ;
    const corpus_a = [_]File{
        .{ .path = "taskA.zig", .source = task_a_src },
        .{ .path = "lib.zig", .source = lib_src },
    };
    const corpus_b = [_]File{
        .{ .path = "taskB.zig", .source = task_b_src },
        .{ .path = "lib.zig", .source = lib_src },
    };
    var ca = try walk(a, corpus_a[0], &corpus_a);
    defer ca.deinit();
    var cb = try walk(a, corpus_b[0], &corpus_b);
    defer cb.deinit();

    try std.testing.expect(ca.has("lib.Store.put", .reference));
    try std.testing.expect(!ca.has("lib.Store.get", .reference));
    try std.testing.expect(cb.has("lib.Store.get", .reference));
    try std.testing.expect(!cb.has("lib.Store.put", .reference));
}

test "walk: unresolved references are counted, not miscredited" {
    const a = std.testing.allocator;
    // A seed referencing only stdlib + builtins + a same-named decl in another
    // file. The other-file decl must NOT be miscredited to the bare ident.
    const other_src = "pub fn shared() void {}\n";
    const seed_src =
        \\const std = @import("std");
        \\pub fn go() void {
        \\    shared();
        \\    std.debug.assert(true);
        \\}
        \\
    ;
    const corpus = [_]File{
        .{ .path = "seed.zig", .source = seed_src },
        .{ .path = "other.zig", .source = other_src },
    };
    var c = try walk(a, corpus[0], &corpus);
    defer c.deinit();
    // `shared` is NOT a same-file decl and `other.zig` is not imported here, so
    // it must be unresolved — never credited as other.shared.
    try std.testing.expect(!c.has("other.shared", .reference));
    try std.testing.expect(c.unresolved >= 1);
}
