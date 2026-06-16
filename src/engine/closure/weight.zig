//! engine.closure.weight — M2.4 token weighting for closure units.
//!
//! Computes the token-count weight `w(u)` for a context unit and the total
//! `cost` of a closure's effective set.
//!
//! ## Tokenization method
//!
//! The corpus is Zig source code, so `std.zig.Tokenizer` is the natural and
//! precise tokenizer.  It is deterministic: the same source bytes always
//! produce the same token sequence regardless of run order or allocator state.
//! `w(u)` counts every token the Zig tokenizer emits for the unit's source
//! span — keywords, identifiers, operators, punctuation, literals, and doc
//! comments — stopping at the `eof` sentinel.  `invalid`-tagged tokens are
//! counted (they occupy a position in the context window) but not silently
//! promoted to meaningful tokens.
//!
//! ## Dedup rule
//!
//! `cost` sums over the **effective closure** (modify ∪ interfaces(reference);
//! transitive excluded) **deduped by qualified symbol name**.  A symbol that
//! appears under both `modify` and `reference` roles (e.g. `seed.helper2`)
//! contributes its weight exactly ONCE — its token span is a fixed property of
//! the source, not a property of the role.  This satisfies spec §1.2 (closure
//! is a SET union) and §2 (cost sums over distinct units).
//!
//! ## Container spans
//!
//! Container symbols (struct/enum/union/opaque) carry the whole container body
//! as their byte span (M2.2 fold).  Computing the real token count of that span
//! is correct: it reflects the context-window cost of holding the entire
//! container resident.  A future refinement (OQ-3) may introduce per-method
//! weighting at a different granularity; for MVP, full-span counting is right.
//!
//! ## Pure module
//!
//! No DB access, no subprocesses, no I/O.  Every function is allocator-
//! explicit and has no global mutable state.

const std = @import("std");
const walk = @import("walk.zig");

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

/// A mapping from qualified symbol name to source span text.  The caller
/// builds this from `symbols.Symbol` entries (using `start_byte`/`end_byte`
/// into the source buffer) and from any corpus-span lookups the reference walk
/// produces.  Slices are borrowed — the caller must keep both the keys and the
/// span buffers alive for the duration of any `cost` call that uses this map.
pub const SpanMap = std.StringHashMapUnmanaged([]const u8);

/// Counts the Zig tokens in `span`.
///
/// Uses `std.zig.Tokenizer` against a null-terminated copy of the span
/// (required by the tokenizer API).  The count includes every token except the
/// terminal `eof` marker.  `invalid`-tagged tokens are counted — they occupy
/// context-window space regardless of their parse validity.
///
/// `gpa` is used only for the null-terminated scratch buffer; it is freed
/// before the function returns (not arena-backed, always released).
///
/// Determinism guarantee: for any fixed `span`, this function returns the same
/// count on every call.  The Zig tokenizer is a pure byte-sequence scanner with
/// no external dependencies.
pub fn tokenCount(gpa: std.mem.Allocator, span: []const u8) !u32 {
    // The tokenizer requires a null-terminated sentinel slice.  We allocate a
    // scratch buffer, copy the span in, and append a sentinel null byte.  The
    // buffer is freed before we return.
    const buf: [:0]u8 = try gpa.allocSentinel(u8, span.len, 0);
    defer gpa.free(buf);
    @memcpy(buf[0..span.len], span);

    var tokenizer = std.zig.Tokenizer.init(buf);
    var count: u32 = 0;
    while (true) {
        const tok = tokenizer.next();
        if (tok.tag == .eof) break;
        count += 1;
    }
    return count;
}

/// Computes the total token cost of `closure`'s effective set.
///
/// The effective closure is `modify ∪ interfaces(reference)` (transitive
/// excluded).  Each qualified symbol name is counted at most ONCE regardless
/// of how many roles it appears under.  The weight of each unit is the token
/// count of its source span, looked up in `spans`.
///
/// Units with qualified names absent from `spans` are silently skipped (weight
/// 0 contribution).  This accommodates reference symbols whose corpus file was
/// not scanned for spans (stdlib imports, builtins, etc.) — their effective
/// contribution to the context cost is unknown and not fabricated.
///
/// Returns the total token count across all distinct effective-closure units.
pub fn cost(
    gpa: std.mem.Allocator,
    closure: *const walk.Closure,
    spans: SpanMap,
) !u32 {
    // Collect the set of distinct qualified names in the effective closure.
    // We use a temporary hash set; the arena is local to this call.
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const aa = arena.allocator();

    var seen: std.StringHashMapUnmanaged(void) = .empty;
    var total: u32 = 0;

    for (closure.units) |unit| {
        // Transitive units are excluded from the effective closure.
        if (unit.role == .transitive) continue;

        // Dedup by qualified name across roles.
        if (seen.contains(unit.qualified)) continue;
        try seen.put(aa, unit.qualified, {});

        // Look up the span for this unit.
        const span = spans.get(unit.qualified) orelse continue;
        const w = try tokenCount(gpa, span);
        total += w;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "tokenCount: hand-counted trivial span" {
    // Source: `fn add(a: u32, b: u32) u32 { return a + b; }`
    // Tokens (19):
    //   fn add ( a : u32 , b : u32 ) u32 { return a + b ; }
    //   1  2   3 4 5  6  7 8 9 10  11 12 13  14   15 16 17 18 19
    const span = "fn add(a: u32, b: u32) u32 { return a + b; }";
    const count = try tokenCount(std.testing.allocator, span);
    try std.testing.expectEqual(@as(u32, 19), count);
}

test "tokenCount: deterministic — same span yields same count twice" {
    const span = "pub const Foo = struct { x: u32, pub fn bar(self: Foo) u32 { return self.x; } };";
    const a = try tokenCount(std.testing.allocator, span);
    const b = try tokenCount(std.testing.allocator, span);
    try std.testing.expectEqual(a, b);
}

test "tokenCount: empty span yields zero" {
    const count = try tokenCount(std.testing.allocator, "");
    try std.testing.expectEqual(@as(u32, 0), count);
}

test "cost: sums weights over effective set only (transitive excluded)" {
    // Build a minimal fake Closure manually.  The Closure's arena backs the
    // unit list; we use a real ArenaAllocator so the deinit path works.
    const a = std.testing.allocator;

    var cl_arena = std.heap.ArenaAllocator.init(a);
    const aa = cl_arena.allocator();

    const units = try aa.dupe(walk.Unit, &[_]walk.Unit{
        .{ .qualified = "seed.run", .role = .modify },
        .{ .qualified = "ee.transform", .role = .reference },
        .{ .qualified = "ee.helper", .role = .transitive }, // excluded
    });
    var cl = walk.Closure{
        .arena = cl_arena,
        .units = units,
        .unresolved = 0,
    };
    defer cl.deinit();

    // Spans: trivial sources so token counts are hand-verifiable.
    // "fn run() void {}" → fn run ( ) void { } → 7 tokens
    // "fn transform(n: u32) u32 { return n; }" → fn transform ( n : u32 ) u32 { return n ; } → 13 tokens
    // ee.helper is transitive and must NOT contribute to cost.
    const run_src = "fn run() void {}";
    const transform_src = "fn transform(n: u32) u32 { return n; }";
    const helper_src = "fn helper(n: u32) u32 { return n; }"; // span present but should be skipped

    var spans: SpanMap = .empty;
    defer spans.deinit(a);
    try spans.put(a, "seed.run", run_src);
    try spans.put(a, "ee.transform", transform_src);
    try spans.put(a, "ee.helper", helper_src);

    const total = try cost(a, &cl, spans);

    // seed.run: fn run ( ) void { } = 7 tokens
    const run_tokens = try tokenCount(a, run_src);
    try std.testing.expectEqual(@as(u32, 7), run_tokens);
    // ee.transform: fn transform ( n : u32 ) u32 { return n ; } = 13 tokens
    const transform_tokens = try tokenCount(a, transform_src);
    try std.testing.expectEqual(@as(u32, 13), transform_tokens);

    // Total = 7 + 13 = 20 (ee.helper skipped — transitive)
    try std.testing.expectEqual(@as(u32, 20), total);
}

test "cost: dedup — symbol under both modify+reference counts once" {
    // seed.helper2 is both modify and reference.  Its weight must count ONCE.
    const a = std.testing.allocator;

    var cl_arena = std.heap.ArenaAllocator.init(a);
    const aa = cl_arena.allocator();

    const units = try aa.dupe(walk.Unit, &[_]walk.Unit{
        .{ .qualified = "seed.helper2", .role = .modify },
        .{ .qualified = "seed.helper2", .role = .reference },
        .{ .qualified = "seed.run", .role = .modify },
    });
    var cl = walk.Closure{
        .arena = cl_arena,
        .units = units,
        .unresolved = 0,
    };
    defer cl.deinit();

    const helper2_src = "fn helper2(n: u32) u32 { return n; }";
    const run_src = "fn run() void {}";

    var spans: SpanMap = .empty;
    defer spans.deinit(a);
    try spans.put(a, "seed.helper2", helper2_src);
    try spans.put(a, "seed.run", run_src);

    const total = try cost(a, &cl, spans);

    // helper2 tokens (counted once): fn helper2 ( n : u32 ) u32 { return n ; } = 13
    const h2 = try tokenCount(a, helper2_src);
    try std.testing.expectEqual(@as(u32, 13), h2);
    // run tokens: fn run ( ) void { } = 7
    const r = try tokenCount(a, run_src);
    try std.testing.expectEqual(@as(u32, 7), r);

    // Total: 13 + 7 = 20 (helper2 NOT double-counted)
    try std.testing.expectEqual(@as(u32, 20), total);

    // Explicit anti-double-count assertion: if helper2 counted twice the
    // total would be 13 + 13 + 7 = 33.  Assert we are strictly less.
    try std.testing.expect(total < 33);
}

test "cost: unit absent from SpanMap contributes zero" {
    // Unresolved reference symbols may lack spans; they must not crash.
    const a = std.testing.allocator;

    var cl_arena = std.heap.ArenaAllocator.init(a);
    const aa = cl_arena.allocator();

    const units = try aa.dupe(walk.Unit, &[_]walk.Unit{
        .{ .qualified = "seed.run", .role = .modify },
        .{ .qualified = "std.debug.print", .role = .reference }, // no span
    });
    var cl = walk.Closure{
        .arena = cl_arena,
        .units = units,
        .unresolved = 1,
    };
    defer cl.deinit();

    var spans: SpanMap = .empty;
    defer spans.deinit(a);
    try spans.put(a, "seed.run", "fn run() void {}");

    const total = try cost(a, &cl, spans);
    // Only seed.run contributes: 7 tokens.
    try std.testing.expectEqual(@as(u32, 7), total);
}
