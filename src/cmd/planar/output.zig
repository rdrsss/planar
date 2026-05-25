//! cmd/planar/output — uniform text + JSON emission for handler results.
//!
//! Handlers call `emit(ctx, mod, value, .{ .json = args.json })` and the
//! dispatcher picks the wire:
//!
//!   json = false → calls `mod.renderText(value, ctx.stdout)`
//!                  (each engine module declares its own text renderer).
//!   json = true  → calls `std.json.Stringify.value(value, …, ctx.stdout)`
//!                  using the struct's field order as the wire format.
//!
//! The `json` flag stays out of runtime.Ctx because it's per-handler
//! state — each verb declares its own `--json` and passes the parsed
//! value in explicitly. No implicit globals; the call site reads.
//!
//! Engine modules opt into text rendering by declaring
//! `pub fn renderText(value, writer) !void`. A `@compileError` here
//! points at the missing function if a handler tries to emit a type
//! that hasn't been taught how to render itself.

const std = @import("std");
const runtime = @import("runtime.zig");

pub const Options = struct {
    json: bool = false,
};

/// Emit `value` to `ctx.stdout` in the format the operator asked for.
/// `mod` is the engine module whose `renderText` formats `value` —
/// typically `engine.<domain>.<entity>` (e.g. `engine.health`).
pub fn emit(
    ctx: *const runtime.Ctx,
    comptime mod: type,
    value: anytype,
    opts: Options,
) !void {
    if (!@hasDecl(mod, "renderText")) {
        @compileError(
            "output.emit: module `" ++ @typeName(mod) ++ "` is missing `pub fn renderText`. " ++
                "Each engine entity must declare its own text renderer; the JSON path is " ++
                "automatic via std.json.Stringify.value.",
        );
    }
    if (opts.json) {
        try std.json.Stringify.value(value, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try mod.renderText(value, ctx.stdout);
    }
}

/// Write `s` to `writer` as a JSON-encoded string literal — surrounding
/// quotes plus RFC 8259 escaping of `"`, `\`, and control characters.
/// UTF-8 multibyte sequences pass through unchanged (matches Go's
/// `encoding/json` default).
///
/// Use this whenever a handler is hand-rolling a JSON object via
/// `writer.print("{{\"k\":{s}", .{...})` and `s` is an interpolated
/// string whose contents are not statically known to be JSON-safe.
/// The mirror situation in Go is `json.Marshal(s)` on a string field;
/// here we delegate to `std.json.Stringify.encodeJsonString`, which
/// implements the same escaping rules.
pub fn writeJsonString(writer: *std.Io.Writer, s: []const u8) !void {
    try std.json.Stringify.encodeJsonString(s, .{}, writer);
}

/// Emit a slice of values. List handlers (`<entity> list`) use this
/// instead of `emit` because the rendering differs from single-value
/// output (one-line table vs key/value detail). The engine module
/// declares `renderListText(values, writer)` alongside `renderText`.
/// JSON path uses `std.json.Stringify.value` on the slice directly —
/// it serializes as a JSON array of objects.
pub fn emitList(
    ctx: *const runtime.Ctx,
    comptime mod: type,
    values: anytype,
    opts: Options,
) !void {
    if (!@hasDecl(mod, "renderListText")) {
        @compileError(
            "output.emitList: module `" ++ @typeName(mod) ++ "` is missing `pub fn renderListText`. " ++
                "Each engine entity that has a `list` verb must declare a list renderer.",
        );
    }
    if (opts.json) {
        try std.json.Stringify.value(values, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try mod.renderListText(values, ctx.stdout);
    }
}

test "writeJsonString: bare ASCII" {
    var buf: [64]u8 = undefined;
    var w: std.Io.Writer = .fixed(&buf);
    try writeJsonString(&w, "hello");
    try std.testing.expectEqualStrings("\"hello\"", w.buffered());
}

test "writeJsonString: embedded double quote escapes" {
    var buf: [64]u8 = undefined;
    var w: std.Io.Writer = .fixed(&buf);
    try writeJsonString(&w, "say \"hi\"");
    try std.testing.expectEqualStrings("\"say \\\"hi\\\"\"", w.buffered());
}

test "writeJsonString: embedded backslash escapes" {
    var buf: [64]u8 = undefined;
    var w: std.Io.Writer = .fixed(&buf);
    try writeJsonString(&w, "C:\\path");
    try std.testing.expectEqualStrings("\"C:\\\\path\"", w.buffered());
}

test "writeJsonString: embedded control char (newline) escapes" {
    var buf: [64]u8 = undefined;
    var w: std.Io.Writer = .fixed(&buf);
    try writeJsonString(&w, "line1\nline2");
    try std.testing.expectEqualStrings("\"line1\\nline2\"", w.buffered());
}

test "writeJsonString: non-ASCII UTF-8 passes through unchanged" {
    var buf: [64]u8 = undefined;
    var w: std.Io.Writer = .fixed(&buf);
    try writeJsonString(&w, "café");
    // Bytes for "café" → 'c','a','f',0xC3,0xA9. The default Options does
    // not set escape_unicode, so the bytes pass through verbatim — matches
    // Go's encoding/json behavior.
    try std.testing.expectEqualStrings("\"café\"", w.buffered());
}

test "writeJsonString: output is parseable JSON for adversarial input" {
    var buf: [256]u8 = undefined;
    var w: std.Io.Writer = .fixed(&buf);
    try writeJsonString(&w, "\"\\\n\t\r\x00mixed");

    // Round-trip: parse and check the value equals the original.
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const parsed = try std.json.parseFromSliceLeaky(
        []const u8,
        arena.allocator(),
        w.buffered(),
        .{},
    );
    try std.testing.expectEqualStrings("\"\\\n\t\r\x00mixed", parsed);
}
