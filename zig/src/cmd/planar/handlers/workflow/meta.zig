//! workflow/meta.zig — parse `@meta` frontmatter from Lua workflow files.
//!
//! The `@meta` block is an in-file Lua long-string comment at the very top
//! of a workflow file.  Syntax:
//!
//!   --[[ @meta
//!   name: finalize-closeout
//!   description: Deterministic closeout gate.
//!   phases: closeout
//!   seam: planar run start/event/finish, planar plan closeout
//!   --]]
//!
//! The block MUST start on line 1 with `--[[ @meta` (no leading whitespace
//! before the `--[[`).  Key/value pairs are `key: value` (YAML-scalar
//! subset; no nesting, no arrays).  The block is closed by a line whose
//! only non-whitespace content is `--]]`.  Lines outside the block are
//! ignored.  An absent block yields an all-empty WorkflowMeta.
//!
//! Only `name`, `description`, `phases`, and `seam` are extracted;
//! unknown keys are silently skipped.

const std = @import("std");

/// Parsed `@meta` block from a workflow file.
pub const WorkflowMeta = struct {
    /// `name:` field (empty when absent).
    name: []const u8 = "",
    /// `description:` field (empty when absent).
    description: []const u8 = "",
    /// `phases:` field — comma-separated phase list (empty when absent).
    phases: []const u8 = "",
    /// `seam:` field — invocation seam summary (empty when absent).
    seam: []const u8 = "",
};

/// ParseResult pairs the parsed meta with whether a `@meta` block was
/// found at all.  A missing block is not an error — legacy or sandbox
/// workflows may omit it.
pub const ParseResult = struct {
    meta: WorkflowMeta,
    /// true when a `--[[ @meta … --]]` block was found.
    found: bool,
};

/// parse reads `content` (a Lua source file as a slice) and returns the
/// first `@meta` block, or `found=false` when none is present.
///
/// All returned string slices point into `content` (zero-copy) — the
/// caller must keep `content` alive for as long as the result is used,
/// or dupe the slices onto their own allocator.
pub fn parse(content: []const u8) ParseResult {
    var lines = std.mem.tokenizeScalar(u8, content, '\n');

    // --- Locate the opening line: `--[[ @meta` (must be on line 1). ---
    const first_raw = lines.next() orelse return .{ .meta = .{}, .found = false };
    const first = std.mem.trim(u8, first_raw, " \t\r");
    // Accept `--[[ @meta` exactly (case-sensitive).
    if (!std.mem.eql(u8, first, "--[[ @meta")) {
        return .{ .meta = .{}, .found = false };
    }

    var meta: WorkflowMeta = .{};

    // --- Parse key: value pairs until `--]]`. ---
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");

        // Closing marker.
        if (std.mem.eql(u8, line, "--]]")) break;

        // Skip blank lines inside the block.
        if (line.len == 0) continue;

        // Split on the first `:`.
        const colon = std.mem.indexOfScalar(u8, line, ':') orelse continue;
        const key = std.mem.trim(u8, line[0..colon], " \t");
        const value = std.mem.trim(u8, line[colon + 1 ..], " \t");

        if (std.mem.eql(u8, key, "name")) {
            meta.name = value;
        } else if (std.mem.eql(u8, key, "description")) {
            meta.description = value;
        } else if (std.mem.eql(u8, key, "phases")) {
            meta.phases = value;
        } else if (std.mem.eql(u8, key, "seam")) {
            meta.seam = value;
        }
        // Unknown keys: silently skip.
    }

    return .{ .meta = meta, .found = true };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "parse: full @meta block" {
    const src =
        \\--[[ @meta
        \\name: finalize-closeout
        \\description: Deterministic closeout gate.
        \\phases: closeout
        \\seam: planar run start/event/finish, planar plan closeout
        \\--]]
        \\
        \\function closeout() end
    ;
    const r = parse(src);
    try std.testing.expect(r.found);
    try std.testing.expectEqualStrings("finalize-closeout", r.meta.name);
    try std.testing.expectEqualStrings("Deterministic closeout gate.", r.meta.description);
    try std.testing.expectEqualStrings("closeout", r.meta.phases);
    try std.testing.expectEqualStrings("planar run start/event/finish, planar plan closeout", r.meta.seam);
}

test "parse: absent @meta block" {
    const src =
        \\-- plain comment, no @meta
        \\function run() end
    ;
    const r = parse(src);
    try std.testing.expect(!r.found);
    try std.testing.expectEqualStrings("", r.meta.name);
}

test "parse: partial @meta block (only name and description)" {
    const src =
        \\--[[ @meta
        \\name: status
        \\description: Read-composition status.
        \\--]]
        \\function status() end
    ;
    const r = parse(src);
    try std.testing.expect(r.found);
    try std.testing.expectEqualStrings("status", r.meta.name);
    try std.testing.expectEqualStrings("Read-composition status.", r.meta.description);
    try std.testing.expectEqualStrings("", r.meta.phases);
    try std.testing.expectEqualStrings("", r.meta.seam);
}

test "parse: @meta block with unknown keys (skipped)" {
    const src =
        \\--[[ @meta
        \\name: example
        \\unknown_key: ignored value
        \\description: A test workflow.
        \\--]]
    ;
    const r = parse(src);
    try std.testing.expect(r.found);
    try std.testing.expectEqualStrings("example", r.meta.name);
    try std.testing.expectEqualStrings("A test workflow.", r.meta.description);
}

test "parse: empty content" {
    const r = parse("");
    try std.testing.expect(!r.found);
}
