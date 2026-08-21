//! engine/templates/validate — smoke-render every string field of a template
//! against a stub Context to surface obvious authoring mistakes.
//!
//! Mirrors Go `internal/templates/validate.go`. JSON syntax is guaranteed by
//! the loader (load fails fast on invalid JSON); validation focuses on
//! template-directive correctness.

const std = @import("std");
const loader = @import("loader.zig");
const render = @import("render.zig");
const context = @import("context.zig");

/// ValidationIssue describes a single problem found during template validation.
pub const ValidationIssue = struct {
    /// Dotted path to the offending field within the template JSON.
    json_path: []const u8,
    /// Human-readable description of the parse/render failure.
    message: []const u8,
};

/// deinitIssues frees every allocation held by `issues`, then the slice.
pub fn deinitIssues(issues: []const ValidationIssue, allocator: std.mem.Allocator) void {
    for (issues) |iss| {
        allocator.free(iss.json_path);
        allocator.free(iss.message);
    }
    allocator.free(issues);
}

/// validate walks every string field in `t.fields`, attempts a stub render,
/// and reports any failures. Returns an empty slice on a clean template.
pub fn validate(allocator: std.mem.Allocator, t: loader.Template) ![]ValidationIssue {
    var issues: std.ArrayList(ValidationIssue) = .empty;
    errdefer {
        for (issues.items) |iss| {
            allocator.free(iss.json_path);
            allocator.free(iss.message);
        }
        issues.deinit(allocator);
    }

    const ctx = context.stubContext();
    try collectIssues(allocator, t.fields, "", &issues, ctx);

    // Whole-template smoke render as a secondary check (catches inter-field
    // failures the per-string walk might miss — currently the same set of
    // errors but keeps parity with Go's behaviour).
    var rendered = render.renderTemplate(allocator, t.fields, ctx) catch |e| {
        try issues.append(allocator, .{
            .json_path = try allocator.dupe(u8, "(smoke-render)"),
            .message = try std.fmt.allocPrint(allocator, "smoke render failed: {s}", .{@errorName(e)}),
        });
        return try issues.toOwnedSlice(allocator);
    };
    rendered.deinit();

    return try issues.toOwnedSlice(allocator);
}

fn collectIssues(
    allocator: std.mem.Allocator,
    v: std.json.Value,
    path: []const u8,
    issues: *std.ArrayList(ValidationIssue),
    ctx: context.Context,
) !void {
    switch (v) {
        .string => |s| {
            // Attempt to render the string against the stub. Errors here are
            // recorded as per-field issues so the operator gets a precise
            // pointer back to the offending JSON path.
            var arena: std.heap.ArenaAllocator = .init(allocator);
            defer arena.deinit();
            const rendered = render.execString(arena.allocator(), s, ctx, null) catch |e| {
                try issues.append(allocator, .{
                    .json_path = try allocator.dupe(u8, path),
                    .message = try std.fmt.allocPrint(allocator, "{s}", .{@errorName(e)}),
                });
                return;
            };
            _ = rendered;
        },
        .object => |obj| {
            var it = obj.iterator();
            while (it.next()) |entry| {
                var child_path: []u8 = undefined;
                if (path.len == 0) {
                    child_path = try allocator.dupe(u8, entry.key_ptr.*);
                } else {
                    child_path = try std.fmt.allocPrint(allocator, "{s}.{s}", .{ path, entry.key_ptr.* });
                }
                defer allocator.free(child_path);
                try collectIssues(allocator, entry.value_ptr.*, child_path, issues, ctx);
            }
        },
        .array => |arr| {
            for (arr.items, 0..) |item, i| {
                const child_path = try std.fmt.allocPrint(allocator, "{s}[{d}]", .{ path, i });
                defer allocator.free(child_path);
                try collectIssues(allocator, item, child_path, issues, ctx);
            }
        },
        else => {},
    }
}

test "validate: clean template yields no issues" {
    const a = std.testing.allocator;
    const t = try loader.load(a, "default", "github-issues", "issue", "");
    defer loader.deinitTemplate(t, a);
    const issues = try validate(a, t);
    defer deinitIssues(issues, a);
    try std.testing.expectEqual(@as(usize, 0), issues.len);
}
