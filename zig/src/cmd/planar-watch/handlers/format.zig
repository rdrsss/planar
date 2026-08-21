//! handlers/format — shared text-rendering helpers for planar-watch
//! column output.
//!
//! Extracted from ps.zig (M4, task 3065) so that tree.zig can reuse
//! the same formatters without an import cycle. Functions here are
//! pure (no DB access); callers receive heap-allocated strings they
//! must free.

const std = @import("std");
const engine = @import("engine");
const runtime = @import("runtime");

const follow = @import("follow.zig");
const agentactivity = engine.runtime.agentactivity;

/// Render the activity summary for text output.
/// Returns a heap-allocated string that must be freed by the caller.
/// Format: `"<summary>"` (quoted). Truncation: > 80 bytes → truncated
/// at a safe byte boundary with U+2026 (…, 3 UTF-8 bytes) as the
/// trailing marker such that the total rendered length including quotes
/// is ≤ 82 bytes (80 content bytes + 2 quote bytes).
/// Empty quotes (`""`) when the summary is null or empty.
pub fn renderActivitySummary(allocator: std.mem.Allocator, summary: ?[]const u8) ![]u8 {
    const s = summary orelse "";
    if (s.len == 0) return allocator.dupe(u8, "\"\"");

    const limit = 80;
    if (s.len <= limit) {
        var buf = try allocator.alloc(u8, s.len + 2);
        buf[0] = '"';
        @memcpy(buf[1 .. 1 + s.len], s);
        buf[1 + s.len] = '"';
        return buf;
    }

    const ellipsis = "\xE2\x80\xA6"; // U+2026, 3 bytes
    const content_max = limit - ellipsis.len; // 77
    var cut = content_max;
    while (cut > 0 and (s[cut] & 0xC0) == 0x80) cut -= 1;

    const out_len = 1 + cut + ellipsis.len + 1;
    var buf = try allocator.alloc(u8, out_len);
    buf[0] = '"';
    @memcpy(buf[1 .. 1 + cut], s[0..cut]);
    @memcpy(buf[1 + cut .. 1 + cut + ellipsis.len], ellipsis);
    buf[out_len - 1] = '"';
    return buf;
}

/// Render the worktree column for text output.
/// Returns a heap-allocated string that must be freed by the caller.
/// When `worktree_path` is null → `""`.
/// When the full path is ≤ 40 chars → the basename only.
/// When the full path is > 40 chars → `…<basename>`.
pub fn renderWorktreeColumn(allocator: std.mem.Allocator, worktree_path: ?[]const u8) ![]u8 {
    const path = worktree_path orelse return allocator.dupe(u8, "\"\"");
    if (path.len == 0) return allocator.dupe(u8, "\"\"");

    const basename: []const u8 = if (std.mem.lastIndexOfScalar(u8, path, '/')) |idx|
        path[idx + 1 ..]
    else
        path;

    if (path.len <= 40) {
        return std.fmt.allocPrint(allocator, "{s}", .{basename});
    }
    return std.fmt.allocPrint(allocator, "\xE2\x80\xA6{s}", .{basename});
}

/// Render the relative-heartbeat column for text output.
/// Returns a heap-allocated string that must be freed by the caller.
/// Format strips the " ago" suffix from relativeTime output since the
/// column key `last_hb:` already provides context.
/// Returns `""` when `last_heartbeat_at` is empty.
pub fn renderRelativeHeartbeat(allocator: std.mem.Allocator, last_heartbeat_at: []const u8) ![]u8 {
    if (last_heartbeat_at.len == 0) return allocator.dupe(u8, "");

    const ctx = runtime.current();
    const ts = std.Io.Clock.now(.real, ctx.io);
    const now_ms = ts.toMilliseconds();

    const rel = follow.relativeTime(allocator, now_ms, last_heartbeat_at) catch {
        return allocator.dupe(u8, "");
    };
    defer allocator.free(rel);

    const suffix = " ago";
    if (std.mem.endsWith(u8, rel, suffix)) {
        return allocator.dupe(u8, rel[0 .. rel.len - suffix.len]);
    }
    return allocator.dupe(u8, rel);
}
