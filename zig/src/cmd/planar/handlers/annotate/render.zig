//! handlers/annotate/render — shared JSON / text emission helpers
//! for the annotate verb family.
//!
//! The engine module ships renderText / renderListText. JSON is built
//! here because the Annotation shape carries nested anchor fields and
//! a tag slice that don't slot neatly into output.emit's tagged-union
//! emitter.

const std = @import("std");
const engine = @import("engine");
const runtime = @import("runtime");

pub fn emitOne(ctx: *const runtime.Ctx, ann: engine.planning.annotation.Annotation, json: bool) !void {
    if (json) {
        try writeAnnotationJSON(ctx.stdout, ann);
        try ctx.stdout.print("\n", .{});
    } else {
        try engine.planning.annotation.renderText(ann, ctx.stdout);
    }
}

pub fn emitList(ctx: *const runtime.Ctx, items: []const engine.planning.annotation.Annotation, json: bool) !void {
    if (json) {
        try ctx.stdout.print("[", .{});
        for (items, 0..) |a, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try writeAnnotationJSON(ctx.stdout, a);
        }
        try ctx.stdout.print("]\n", .{});
    } else {
        try engine.planning.annotation.renderListText(items, ctx.stdout);
    }
}

fn writeAnnotationJSON(w: *std.Io.Writer, a: engine.planning.annotation.Annotation) !void {
    try w.print("{{", .{});

    try w.print("\"id\":{d},", .{a.id});
    try w.print("\"scope_kind\":\"{s}\",", .{@tagName(a.scope_kind)});
    if (a.scope_id) |sid| try w.print("\"scope_id\":{d},", .{sid}) else try w.print("\"scope_id\":null,", .{});

    try w.print("\"anchor\":{{\"path\":", .{});
    try std.json.Stringify.encodeJsonString(a.anchor.path, .{}, w);
    if (a.anchor.line_start) |n| try w.print(",\"line_start\":{d}", .{n}) else try w.print(",\"line_start\":null", .{});
    if (a.anchor.line_end) |n| try w.print(",\"line_end\":{d}", .{n}) else try w.print(",\"line_end\":null", .{});
    try w.print(",\"commit_sha\":", .{});
    try std.json.Stringify.encodeJsonString(a.anchor.commit_sha, .{}, w);
    try w.print(",\"text_hash\":", .{});
    try std.json.Stringify.encodeJsonString(a.anchor.text_hash, .{}, w);
    try w.print(",\"text\":", .{});
    try std.json.Stringify.encodeJsonString(a.anchor.text, .{}, w);
    try w.print("}},", .{});

    if (a.title) |s| {
        try w.print("\"title\":", .{});
        try std.json.Stringify.encodeJsonString(s, .{}, w);
        try w.print(",", .{});
    } else {
        try w.print("\"title\":null,", .{});
    }
    if (a.slug) |s| {
        try w.print("\"slug\":", .{});
        try std.json.Stringify.encodeJsonString(s, .{}, w);
        try w.print(",", .{});
    } else {
        try w.print("\"slug\":null,", .{});
    }

    try w.print("\"body\":", .{});
    try std.json.Stringify.encodeJsonString(a.body, .{}, w);
    try w.print(",\"status\":\"{s}\",", .{@tagName(a.status)});
    try w.print("\"vendor\":", .{});
    try std.json.Stringify.encodeJsonString(a.vendor, .{}, w);
    try w.print(",", .{});

    if (a.plan_id) |n| try w.print("\"plan_id\":{d},", .{n}) else try w.print("\"plan_id\":null,", .{});
    if (a.task_id) |n| try w.print("\"task_id\":{d},", .{n}) else try w.print("\"task_id\":null,", .{});

    try w.print("\"tags\":[", .{});
    for (a.tags, 0..) |t, i| {
        if (i > 0) try w.print(",", .{});
        try std.json.Stringify.encodeJsonString(t, .{}, w);
    }
    try w.print("],", .{});

    try w.print("\"created_at\":", .{});
    try std.json.Stringify.encodeJsonString(a.created_at, .{}, w);
    try w.print(",\"updated_at\":", .{});
    try std.json.Stringify.encodeJsonString(a.updated_at, .{}, w);
    try w.print("}}", .{});
}
