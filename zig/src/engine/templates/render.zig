//! engine/templates/render — text/template substitution against a Context.
//!
//! Mirrors Go `internal/templates/render.go`. The renderer walks the decoded
//! JSON tree of a Template; for every string value it executes the embedded
//! `{{...}}` directives against the supplied Context and replaces the value
//! with the rendered string. Non-string values pass through unchanged.
//!
//! Supported directives (the subset our embedded templates exercise):
//!   {{.Field}}              — field reference; nested via `.A.B.C`
//!   {{.}}                   — current iteration value (inside `range`)
//!   {{range .Field}}…{{end}} — iterate over a slice; body is re-rendered
//!                              per element with `{{.}}` bound to the
//!                              element
//!   {{if .Field}}…{{end}}    — emit body only when Field is truthy
//!                              (non-empty string / non-zero number /
//!                              non-empty slice)
//!
//! Anything outside this surface (else, with, range over map, pipelines,
//! custom funcs) is rejected with `error.UnsupportedDirective` so we fail
//! loudly rather than silently produce wrong output. The Go embedded
//! templates we ship today only use the supported surface.

const std = @import("std");
const context_mod = @import("context.zig");

pub const Context = context_mod.Context;

pub const RenderError = error{
    UnsupportedDirective,
    UnknownField,
    UnclosedDirective,
    UnexpectedEnd,
    OutOfMemory,
};

/// Rendered payload — the template's JSON shape with every string field
/// substituted. The outer container always matches the template's root
/// (object or array of objects); the caller owns the returned `std.json.Value`
/// via the parsed wrapper.
pub const Rendered = struct {
    /// Owning arena for every allocation in `value`. Free with `deinit`.
    arena: *std.heap.ArenaAllocator,
    value: std.json.Value,

    pub fn deinit(self: *Rendered) void {
        const child = self.arena.child_allocator;
        self.arena.deinit();
        child.destroy(self.arena);
    }

    /// Serialize the rendered value to compact JSON. Caller owns the bytes.
    pub fn toJson(self: *const Rendered, allocator: std.mem.Allocator) ![]u8 {
        var out: std.Io.Writer.Allocating = .init(allocator);
        defer out.deinit();
        try std.json.Stringify.value(self.value, .{ .whitespace = .indent_2 }, &out.writer);
        return try allocator.dupe(u8, out.written());
    }
};

/// renderTemplate executes every string field in `fields` against `ctx` and
/// returns the substituted JSON tree. Caller calls `deinit` on the result.
pub fn renderTemplate(
    allocator: std.mem.Allocator,
    fields: std.json.Value,
    ctx: Context,
) !Rendered {
    const arena = try allocator.create(std.heap.ArenaAllocator);
    arena.* = .init(allocator);
    errdefer {
        arena.deinit();
        allocator.destroy(arena);
    }

    const value = try renderValue(arena.allocator(), fields, ctx);
    return .{ .arena = arena, .value = value };
}

fn renderValue(
    allocator: std.mem.Allocator,
    v: std.json.Value,
    ctx: Context,
) RenderError!std.json.Value {
    switch (v) {
        .string => |s| {
            const rendered = try execString(allocator, s, ctx, null);
            return .{ .string = rendered };
        },
        .object => |obj| {
            var out: std.json.ObjectMap = .empty;
            var it = obj.iterator();
            while (it.next()) |entry| {
                const child = try renderValue(allocator, entry.value_ptr.*, ctx);
                const key_dup = try allocator.dupe(u8, entry.key_ptr.*);
                try out.put(allocator, key_dup, child);
            }
            return .{ .object = out };
        },
        .array => |arr| {
            var out: std.json.Array = .init(allocator);
            try out.ensureTotalCapacityPrecise(arr.items.len);
            for (arr.items) |item| {
                const child = try renderValue(allocator, item, ctx);
                out.appendAssumeCapacity(child);
            }
            return .{ .array = out };
        },
        else => return v,
    }
}

/// CurrentItem is the binding for `{{.}}` inside a `range` body. When nil,
/// `{{.}}` is an error (Go's text/template binds `.` to the top-level
/// context outside range, but our templates never use that form).
const CurrentItem = union(enum) {
    string: []const u8,
    child_ref: context_mod.ChildRef,
};

/// execString parses `src` and emits the substituted output as an owned
/// slice. The optional `item` is the current iteration binding for `{{.}}`.
pub fn execString(
    allocator: std.mem.Allocator,
    src: []const u8,
    ctx: Context,
    item: ?CurrentItem,
) RenderError![]const u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);

    var cursor: usize = 0;
    try execRange(allocator, src, &cursor, ctx, item, &out, null);
    return try out.toOwnedSlice(allocator);
}

/// execRange consumes `src[cursor..]` writing rendered output into `out`.
/// When `stop_keyword` is non-null, the loop returns as soon as it sees
/// `{{stop_keyword}}` (consuming the directive), letting the caller resume.
fn execRange(
    allocator: std.mem.Allocator,
    src: []const u8,
    cursor: *usize,
    ctx: Context,
    item: ?CurrentItem,
    out: *std.ArrayList(u8),
    stop_keyword: ?[]const u8,
) RenderError!void {
    while (cursor.* < src.len) {
        const open = std.mem.indexOfPos(u8, src, cursor.*, "{{") orelse {
            try out.appendSlice(allocator, src[cursor.*..]);
            cursor.* = src.len;
            if (stop_keyword != null) return error.UnexpectedEnd;
            return;
        };
        try out.appendSlice(allocator, src[cursor.*..open]);
        const close = std.mem.indexOfPos(u8, src, open + 2, "}}") orelse return error.UnclosedDirective;
        const directive = std.mem.trim(u8, src[open + 2 .. close], " \t");
        cursor.* = close + 2;

        if (stop_keyword) |kw| {
            if (std.mem.eql(u8, directive, kw)) return;
        }

        try execDirective(allocator, src, cursor, ctx, item, out, directive);
    }
    if (stop_keyword != null) return error.UnexpectedEnd;
}

fn execDirective(
    allocator: std.mem.Allocator,
    src: []const u8,
    cursor: *usize,
    ctx: Context,
    item: ?CurrentItem,
    out: *std.ArrayList(u8),
    directive: []const u8,
) RenderError!void {
    if (directive.len == 0) return error.UnsupportedDirective;

    // `end` only valid when stop_keyword bubbles up — bare `end` here is a
    // mismatched close.
    if (std.mem.eql(u8, directive, "end")) return error.UnsupportedDirective;
    if (std.mem.eql(u8, directive, "else")) return error.UnsupportedDirective;

    if (std.mem.startsWith(u8, directive, "range ")) {
        const path = std.mem.trim(u8, directive["range ".len..], " \t");
        return execLoop(allocator, src, cursor, ctx, out, path);
    }
    if (std.mem.startsWith(u8, directive, "if ")) {
        const path = std.mem.trim(u8, directive["if ".len..], " \t");
        return execIf(allocator, src, cursor, ctx, item, out, path);
    }

    // Reference directive: `{{.}}` or `{{.A.B}}`.
    if (directive[0] != '.') return error.UnsupportedDirective;
    const value = try resolveReference(allocator, directive, ctx, item);
    try out.appendSlice(allocator, value);
}

fn execLoop(
    allocator: std.mem.Allocator,
    src: []const u8,
    cursor: *usize,
    ctx: Context,
    out: *std.ArrayList(u8),
    path: []const u8,
) RenderError!void {
    // Capture the loop body source range.
    const body_start = cursor.*;
    var probe = cursor.*;
    var depth: usize = 1;
    while (true) {
        const open = std.mem.indexOfPos(u8, src, probe, "{{") orelse return error.UnclosedDirective;
        const close = std.mem.indexOfPos(u8, src, open + 2, "}}") orelse return error.UnclosedDirective;
        const inner = std.mem.trim(u8, src[open + 2 .. close], " \t");
        if (std.mem.startsWith(u8, inner, "range ") or std.mem.startsWith(u8, inner, "if ")) {
            depth += 1;
        } else if (std.mem.eql(u8, inner, "end")) {
            depth -= 1;
            if (depth == 0) {
                const body = src[body_start..open];
                cursor.* = close + 2;
                return iterateAndRender(allocator, body, ctx, out, path);
            }
        }
        probe = close + 2;
    }
}

fn iterateAndRender(
    allocator: std.mem.Allocator,
    body: []const u8,
    ctx: Context,
    out: *std.ArrayList(u8),
    path: []const u8,
) RenderError!void {
    if (std.mem.eql(u8, path, ".Touches")) {
        for (ctx.touches) |s| {
            try renderBody(allocator, body, ctx, .{ .string = s }, out);
        }
        return;
    }
    if (std.mem.eql(u8, path, ".Children")) {
        for (ctx.children) |c| {
            try renderBody(allocator, body, ctx, .{ .child_ref = c }, out);
        }
        return;
    }
    return error.UnknownField;
}

fn renderBody(
    allocator: std.mem.Allocator,
    body: []const u8,
    ctx: Context,
    item: CurrentItem,
    out: *std.ArrayList(u8),
) RenderError!void {
    var cursor: usize = 0;
    try execRange(allocator, body, &cursor, ctx, item, out, null);
}

fn execIf(
    allocator: std.mem.Allocator,
    src: []const u8,
    cursor: *usize,
    ctx: Context,
    item: ?CurrentItem,
    out: *std.ArrayList(u8),
    path: []const u8,
) RenderError!void {
    // Find matching {{end}}.
    const body_start = cursor.*;
    var probe = cursor.*;
    var depth: usize = 1;
    while (true) {
        const open = std.mem.indexOfPos(u8, src, probe, "{{") orelse return error.UnclosedDirective;
        const close = std.mem.indexOfPos(u8, src, open + 2, "}}") orelse return error.UnclosedDirective;
        const inner = std.mem.trim(u8, src[open + 2 .. close], " \t");
        if (std.mem.startsWith(u8, inner, "range ") or std.mem.startsWith(u8, inner, "if ")) {
            depth += 1;
        } else if (std.mem.eql(u8, inner, "end")) {
            depth -= 1;
            if (depth == 0) {
                const body = src[body_start..open];
                cursor.* = close + 2;
                const truthy = try evalTruthy(ctx, item, path);
                if (truthy) {
                    var inner_cursor: usize = 0;
                    try execRange(allocator, body, &inner_cursor, ctx, item, out, null);
                }
                return;
            }
        }
        probe = close + 2;
    }
}

fn evalTruthy(ctx: Context, item: ?CurrentItem, path: []const u8) RenderError!bool {
    var tmp: [128]u8 = undefined;
    var fb: std.heap.FixedBufferAllocator = .init(&tmp);
    const fa = fb.allocator();
    // Reuse resolveReference: a value is truthy when its rendered string is non-empty.
    // (Strings, ints non-zero, and present slices all render to non-empty content.)
    const rendered = resolveReference(fa, path, ctx, item) catch |e| switch (e) {
        error.UnknownField => return false,
        else => return e,
    };
    return rendered.len > 0;
}

/// resolveReference renders `{{.path}}` to a string. Allocations use
/// `allocator` (typically scoped to the surrounding execString call).
fn resolveReference(
    allocator: std.mem.Allocator,
    directive: []const u8,
    ctx: Context,
    item: ?CurrentItem,
) RenderError![]const u8 {
    if (std.mem.eql(u8, directive, ".")) {
        if (item) |it| return try renderCurrentItem(allocator, it);
        return error.UnknownField;
    }
    // Strip leading dot then split on '.'.
    if (directive.len < 2 or directive[0] != '.') return error.UnknownField;
    const path = directive[1..];

    var head: []const u8 = path;
    var rest: []const u8 = "";
    if (std.mem.indexOfScalar(u8, path, '.')) |idx| {
        head = path[0..idx];
        rest = path[idx + 1 ..];
    }

    // Top-level dispatch on the head field name.
    if (std.mem.eql(u8, head, "Feature")) return try renderPlanField(allocator, ctx.feature, rest);
    if (std.mem.eql(u8, head, "Plan")) return try renderPlanField(allocator, ctx.plan, rest);
    if (std.mem.eql(u8, head, "Task")) return try renderTaskField(allocator, ctx.task, rest);
    if (std.mem.eql(u8, head, "Scenario")) return try renderScenarioField(allocator, ctx.scenario, rest);
    if (std.mem.eql(u8, head, "Assoc")) return try renderAssocField(allocator, ctx.assoc, rest);
    if (std.mem.eql(u8, head, "ExternalKey")) {
        if (rest.len > 0) return error.UnknownField;
        if (item) |it| switch (it) {
            .child_ref => |c| return try allocator.dupe(u8, c.external_key),
            else => {},
        };
        return try allocator.dupe(u8, ctx.external_key);
    }
    if (std.mem.eql(u8, head, "Touches")) {
        if (rest.len > 0) return error.UnknownField;
        // text/template prints a slice as `[a b c]`; we only render whole
        // Touches inside a `range`. Bare `{{.Touches}}` is an unsupported
        // shape in our templates.
        return error.UnsupportedDirective;
    }
    if (std.mem.eql(u8, head, "Title")) {
        // Used inside `{{range .Children}}{{.Title}}{{end}}`.
        if (item) |it| switch (it) {
            .child_ref => |c| {
                if (rest.len > 0) return error.UnknownField;
                return try allocator.dupe(u8, c.title);
            },
            else => return error.UnknownField,
        };
        return error.UnknownField;
    }
    return error.UnknownField;
}

fn renderCurrentItem(allocator: std.mem.Allocator, item: CurrentItem) RenderError![]const u8 {
    return switch (item) {
        .string => |s| try allocator.dupe(u8, s),
        .child_ref => |c| try allocator.dupe(u8, c.title),
    };
}

fn renderPlanField(allocator: std.mem.Allocator, p: context_mod.PlanInfo, rest: []const u8) RenderError![]const u8 {
    if (rest.len == 0) return error.UnsupportedDirective;
    if (std.mem.eql(u8, rest, "ID")) return try std.fmt.allocPrint(allocator, "{d}", .{p.id});
    if (std.mem.eql(u8, rest, "Slug")) return try allocator.dupe(u8, p.slug);
    if (std.mem.eql(u8, rest, "Title")) return try allocator.dupe(u8, p.title);
    if (std.mem.eql(u8, rest, "Body")) return try allocator.dupe(u8, p.body);
    if (std.mem.eql(u8, rest, "Status")) return try allocator.dupe(u8, p.status);
    if (std.mem.eql(u8, rest, "ScopeKind")) return try allocator.dupe(u8, p.scope_kind);
    if (std.mem.eql(u8, rest, "ScopeID")) return try std.fmt.allocPrint(allocator, "{d}", .{p.scope_id});
    return error.UnknownField;
}

fn renderTaskField(allocator: std.mem.Allocator, t: context_mod.TaskInfo, rest: []const u8) RenderError![]const u8 {
    if (rest.len == 0) return error.UnsupportedDirective;
    if (std.mem.eql(u8, rest, "ID")) return try std.fmt.allocPrint(allocator, "{d}", .{t.id});
    if (std.mem.eql(u8, rest, "Title")) return try allocator.dupe(u8, t.title);
    if (std.mem.eql(u8, rest, "Body")) return try allocator.dupe(u8, t.body);
    if (std.mem.eql(u8, rest, "Status")) return try allocator.dupe(u8, t.status);
    if (std.mem.eql(u8, rest, "Priority")) return try std.fmt.allocPrint(allocator, "{d}", .{t.priority});
    if (std.mem.eql(u8, rest, "ScopeKind")) return try allocator.dupe(u8, t.scope_kind);
    if (std.mem.eql(u8, rest, "ScopeID")) return try std.fmt.allocPrint(allocator, "{d}", .{t.scope_id});
    return error.UnknownField;
}

fn renderScenarioField(allocator: std.mem.Allocator, s: context_mod.ScenarioInfo, rest: []const u8) RenderError![]const u8 {
    if (rest.len == 0) return error.UnsupportedDirective;
    if (std.mem.eql(u8, rest, "ID")) return try std.fmt.allocPrint(allocator, "{d}", .{s.id});
    if (std.mem.eql(u8, rest, "Title")) return try allocator.dupe(u8, s.title);
    if (std.mem.eql(u8, rest, "Body")) return try allocator.dupe(u8, s.body);
    return error.UnknownField;
}

fn renderAssocField(allocator: std.mem.Allocator, a: context_mod.AssocInfo, rest: []const u8) RenderError![]const u8 {
    if (rest.len == 0) return error.UnsupportedDirective;
    if (std.mem.eql(u8, rest, "Slug")) return try allocator.dupe(u8, a.slug);
    if (std.mem.eql(u8, rest, "Name")) return try allocator.dupe(u8, a.name);
    return error.UnknownField;
}

test "renderTemplate: simple substitution" {
    const a = std.testing.allocator;
    var parsed = try std.json.parseFromSlice(std.json.Value, a, "{\"title\":\"{{.Task.Title}}\"}", .{});
    defer parsed.deinit();
    var rendered = try renderTemplate(a, parsed.value, .{ .task = .{ .id = 1, .title = "do the thing", .body = "", .status = "todo" } });
    defer rendered.deinit();
    const obj = rendered.value.object;
    try std.testing.expectEqualStrings("do the thing", obj.get("title").?.string);
}

test "renderTemplate: range over touches" {
    const a = std.testing.allocator;
    var parsed = try std.json.parseFromSlice(std.json.Value, a, "{\"body\":\"{{range .Touches}}- {{.}}\\n{{end}}\"}", .{});
    defer parsed.deinit();
    var rendered = try renderTemplate(a, parsed.value, .{ .touches = &.{ "a/b", "c/d" } });
    defer rendered.deinit();
    try std.testing.expectEqualStrings("- a/b\n- c/d\n", rendered.value.object.get("body").?.string);
}

test "renderTemplate: if branch suppressed when truthy is false" {
    const a = std.testing.allocator;
    var parsed = try std.json.parseFromSlice(std.json.Value, a, "{\"x\":\"{{if .ExternalKey}}{{.ExternalKey}}{{end}}\"}", .{});
    defer parsed.deinit();
    var rendered_empty = try renderTemplate(a, parsed.value, .{ .external_key = "" });
    defer rendered_empty.deinit();
    try std.testing.expectEqualStrings("", rendered_empty.value.object.get("x").?.string);
    var rendered_present = try renderTemplate(a, parsed.value, .{ .external_key = "JIRA-1" });
    defer rendered_present.deinit();
    try std.testing.expectEqualStrings("JIRA-1", rendered_present.value.object.get("x").?.string);
}

test "renderTemplate: child ExternalKey inside range binds iteration item" {
    const a = std.testing.allocator;
    var parsed = try std.json.parseFromSlice(
        std.json.Value,
        a,
        "{\"x\":\"{{range .Children}}- {{.Title}}{{if .ExternalKey}} (#{{.ExternalKey}}){{end}}\\n{{end}}\"}",
        .{},
    );
    defer parsed.deinit();
    const children = [_]context_mod.ChildRef{
        .{ .title = "child-one", .external_key = "GH-11" },
        .{ .title = "child-two", .external_key = "" },
    };
    var rendered = try renderTemplate(a, parsed.value, .{
        .children = children[0..],
        .external_key = "PARENT-999",
    });
    defer rendered.deinit();
    try std.testing.expectEqualStrings(
        "- child-one (#GH-11)\n- child-two\n",
        rendered.value.object.get("x").?.string,
    );
}

test "renderTemplate: pass-through for non-string values" {
    const a = std.testing.allocator;
    var parsed = try std.json.parseFromSlice(std.json.Value, a, "{\"labels\":[\"a\",\"b\"],\"n\":42}", .{});
    defer parsed.deinit();
    var rendered = try renderTemplate(a, parsed.value, .{});
    defer rendered.deinit();
    try std.testing.expectEqual(@as(usize, 2), rendered.value.object.get("labels").?.array.items.len);
    try std.testing.expectEqual(@as(i64, 42), rendered.value.object.get("n").?.integer);
}
