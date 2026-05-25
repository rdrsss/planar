const cli = @import("cli");
const std = @import("std");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");
const c = @cImport({
    @cInclude("dirent.h");
    @cInclude("errno.h");
    @cInclude("sys/stat.h");
});

const ExtractQuestionsResult = struct {
    artifact_id: i64,
    file: []const u8,
    questions: []Question,
};

const Question = struct {
    title: []const u8,
    body: []const u8,
    source_line: usize,
};

const Line = struct {
    text: []const u8,
    line_no: usize,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "extract-questions" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan = common.resolvePlanArg(d, ctx.allocator, args.plan) catch |e| switch (e) {
        error.InvalidInput => exit.die(ctx, e, "invalid plan '{s}'", .{args.plan}),
        error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{args.plan}),
        else => exit.die(ctx, e, "resolving plan '{s}' failed: {s}", .{ args.plan, @errorName(e) }),
    };
    defer plan.deinit(ctx.allocator);

    const root = common.resolveWorkbenchRoot(ctx.allocator) catch |e|
        exit.die(ctx, e, "resolving workbench root failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const feature_dir = common.resolveFeatureDirForPlan(d, ctx.allocator, root, plan.id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{args.plan}),
        else => exit.die(ctx, e, "resolving feature directory failed: {s}", .{@errorName(e)}),
    };
    defer ctx.allocator.free(feature_dir);

    if (!pathExists(feature_dir)) {
        try ctx.stdout.print("workbench tree not found for plan {d}; run 'workbench push {s}' first\n", .{ plan.id, args.plan });
        return;
    }

    const files = collectTopLevelMarkdownFiles(ctx.allocator, feature_dir) catch |e|
        exit.die(ctx, e, "reading feature directory failed: {s}", .{@errorName(e)});
    defer freeStrings(ctx.allocator, files);

    var results: std.ArrayList(ExtractQuestionsResult) = .empty;
    defer deinitResults(ctx.allocator, results.items);
    defer results.deinit(ctx.allocator);

    for (files) |file_name| {
        const abs_path = std.fs.path.join(ctx.allocator, &.{ feature_dir, file_name }) catch continue;
        defer ctx.allocator.free(abs_path);

        const raw = std.Io.Dir.cwd().readFileAlloc(ctx.io, abs_path, ctx.allocator, .unlimited) catch |e| {
            ctx.stderr.print("warning: reading {s} failed: {s}\n", .{ file_name, @errorName(e) }) catch {};
            continue;
        };
        defer ctx.allocator.free(raw);

        const parsed = engine.workbench.parse.parse(ctx.allocator, raw) catch continue;
        defer engine.workbench.parse.deinit(parsed, ctx.allocator);

        const questions = extractQuestions(ctx.allocator, parsed.body) catch |e|
            exit.die(ctx, e, "extracting questions from {s} failed: {s}", .{ file_name, @errorName(e) });

        try results.append(ctx.allocator, .{
            .artifact_id = parsed.frontmatter.entity_id,
            .file = try ctx.allocator.dupe(u8, file_name),
            .questions = questions,
        });
    }

    if (args.json) {
        try std.json.Stringify.value(results.items, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }

    for (results.items) |result| {
        try ctx.stdout.print("{s} (artifact {d}): {d} question(s)\n", .{
            result.file,
            result.artifact_id,
            result.questions.len,
        });

        for (result.questions) |q| {
            if (q.body.len == 0) {
                try ctx.stdout.print("  [line {d}] {s}\n", .{ q.source_line, q.title });
            } else if (q.body.len > 60) {
                try ctx.stdout.print("  [line {d}] {s} — {s}…\n", .{ q.source_line, q.title, q.body[0..60] });
            } else {
                try ctx.stdout.print("  [line {d}] {s} — {s}\n", .{ q.source_line, q.title, q.body });
            }
        }
    }
}

fn deinitResults(allocator: std.mem.Allocator, values: []const ExtractQuestionsResult) void {
    for (values) |value| {
        allocator.free(value.file);
        freeQuestions(allocator, value.questions);
    }
}

fn freeQuestions(allocator: std.mem.Allocator, questions: []const Question) void {
    for (questions) |q| {
        allocator.free(q.title);
        allocator.free(q.body);
    }
    allocator.free(questions);
}

fn freeStrings(allocator: std.mem.Allocator, values: []const []const u8) void {
    for (values) |v| allocator.free(v);
    allocator.free(values);
}

fn collectTopLevelMarkdownFiles(allocator: std.mem.Allocator, feature_dir: []const u8) ![]const []const u8 {
    const dir_z = try allocator.dupeZ(u8, feature_dir);
    defer allocator.free(dir_z);

    const dp = c.opendir(dir_z.ptr) orelse return error.OpenFailed;
    defer _ = c.closedir(dp);

    var out: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit(allocator);
    }

    while (c.readdir(dp)) |ent| {
        const name = std.mem.span(@as([*:0]const u8, @ptrCast(&ent.*.d_name)));
        if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) continue;
        if (std.mem.eql(u8, name, "README.md")) continue;
        if (!std.mem.endsWith(u8, name, ".md")) continue;

        const abs_path = try std.fs.path.join(allocator, &.{ feature_dir, name });
        defer allocator.free(abs_path);

        if (!pathIsRegularFile(abs_path)) continue;
        try out.append(allocator, try allocator.dupe(u8, name));
    }

    insertionSortStrings(out.items);
    return try out.toOwnedSlice(allocator);
}

fn insertionSortStrings(values: [][]const u8) void {
    if (values.len < 2) return;
    var i: usize = 1;
    while (i < values.len) : (i += 1) {
        const key = values[i];
        var j = i;
        while (j > 0 and std.mem.order(u8, key, values[j - 1]) == .lt) : (j -= 1) {
            values[j] = values[j - 1];
        }
        values[j] = key;
    }
}

fn pathExists(path: []const u8) bool {
    return statMode(path) != null;
}

fn pathIsRegularFile(path: []const u8) bool {
    const mode = statMode(path) orelse return false;
    return (mode & c.S_IFMT) == c.S_IFREG;
}

fn statMode(path: []const u8) ?c.mode_t {
    const path_z = std.heap.page_allocator.dupeZ(u8, path) catch return null;
    defer std.heap.page_allocator.free(path_z);
    var st: c.struct_stat = undefined;
    if (c.stat(path_z.ptr, &st) != 0) return null;
    return st.st_mode;
}

fn extractQuestions(allocator: std.mem.Allocator, body: []const u8) ![]Question {
    const lines = try splitLinesWithNumbers(allocator, body);
    defer allocator.free(lines);

    var section_start: ?usize = null;
    for (lines, 0..) |line, idx| {
        if (isOpenQuestionsH2(line.text)) {
            section_start = idx;
            break;
        }
    }
    if (section_start == null) return allocator.alloc(Question, 0);

    const start = section_start.? + 1;
    var end = lines.len;
    var i = start;
    while (i < lines.len) : (i += 1) {
        if (isH1orH2(lines[i].text)) {
            end = i;
            break;
        }
    }
    const section = lines[start..end];

    var has_h3 = false;
    for (section) |line| {
        if (isH3(line.text)) {
            has_h3 = true;
            break;
        }
    }

    if (has_h3) return extractH3Questions(allocator, section);
    return extractBulletQuestions(allocator, section);
}

fn splitLinesWithNumbers(allocator: std.mem.Allocator, body: []const u8) ![]Line {
    var out: std.ArrayList(Line) = .empty;
    errdefer out.deinit(allocator);

    var line_no: usize = 1;
    var start: usize = 0;
    var i: usize = 0;
    while (i < body.len) : (i += 1) {
        if (body[i] == '\n') {
            try out.append(allocator, .{
                .text = body[start..i],
                .line_no = line_no,
            });
            line_no += 1;
            start = i + 1;
        }
    }
    try out.append(allocator, .{
        .text = body[start..],
        .line_no = line_no,
    });

    return try out.toOwnedSlice(allocator);
}

fn isOpenQuestionsH2(line: []const u8) bool {
    if (!std.mem.startsWith(u8, line, "## ")) return false;
    const text = std.mem.trim(u8, line[3..], " \t\r");
    return std.ascii.eqlIgnoreCase(text, "open questions");
}

fn isH1orH2(line: []const u8) bool {
    return std.mem.startsWith(u8, line, "# ") or
        std.mem.startsWith(u8, line, "## ") or
        std.mem.eql(u8, line, "#") or
        std.mem.eql(u8, line, "##");
}

fn isH3(line: []const u8) bool {
    return std.mem.startsWith(u8, line, "### ");
}

fn isBullet(line: []const u8) bool {
    const trimmed = trimLeftWhitespace(line);
    return std.mem.startsWith(u8, trimmed, "- ") or std.mem.startsWith(u8, trimmed, "* ");
}

fn extractH3Questions(allocator: std.mem.Allocator, section: []const Line) ![]Question {
    var out: std.ArrayList(Question) = .empty;
    errdefer {
        for (out.items) |q| {
            allocator.free(q.title);
            allocator.free(q.body);
        }
        out.deinit(allocator);
    }

    var current_title: ?[]const u8 = null;
    var current_line: usize = 0;
    var body_start: usize = 0;

    for (section, 0..) |line, idx| {
        if (isH3(line.text)) {
            if (current_title) |title| {
                try out.append(allocator, .{
                    .title = try allocator.dupe(u8, title),
                    .body = try joinBodyLines(allocator, section[body_start..idx]),
                    .source_line = current_line,
                });
            }
            current_title = std.mem.trim(u8, line.text[4..], " \t\r");
            current_line = line.line_no;
            body_start = idx + 1;
        }
    }

    if (current_title) |title| {
        try out.append(allocator, .{
            .title = try allocator.dupe(u8, title),
            .body = try joinBodyLines(allocator, section[body_start..]),
            .source_line = current_line,
        });
    }

    return try out.toOwnedSlice(allocator);
}

fn extractBulletQuestions(allocator: std.mem.Allocator, section: []const Line) ![]Question {
    var out: std.ArrayList(Question) = .empty;
    errdefer {
        for (out.items) |q| {
            allocator.free(q.title);
            allocator.free(q.body);
        }
        out.deinit(allocator);
    }

    for (section) |line| {
        if (!isBullet(line.text)) continue;
        const trimmed = trimLeftWhitespace(line.text);
        const raw = if (std.mem.startsWith(u8, trimmed, "- "))
            std.mem.trim(u8, trimmed[2..], " \t\r")
        else
            std.mem.trim(u8, trimmed[2..], " \t\r");
        const title, const body = splitFirstSentence(raw);
        try out.append(allocator, .{
            .title = try allocator.dupe(u8, title),
            .body = try allocator.dupe(u8, body),
            .source_line = line.line_no,
        });
    }

    return try out.toOwnedSlice(allocator);
}

fn splitFirstSentence(text: []const u8) struct { []const u8, []const u8 } {
    const terms = [_][]const u8{ ". ", "? ", "! " };
    var earliest: ?usize = null;

    for (terms) |term| {
        if (std.mem.indexOf(u8, text, term)) |idx| {
            const punct = idx + 1;
            if (earliest == null or punct < earliest.?) earliest = punct;
        }
    }

    if (earliest == null) {
        return .{ std.mem.trim(u8, text, " \t\r"), "" };
    }

    const title = std.mem.trim(u8, text[0..earliest.?], " \t\r");
    const body = if (earliest.? + 1 <= text.len)
        std.mem.trim(u8, text[earliest.? + 1 ..], " \t\r")
    else
        "";
    return .{ title, body };
}

fn joinBodyLines(allocator: std.mem.Allocator, lines: []const Line) ![]const u8 {
    var start: usize = 0;
    while (start < lines.len and std.mem.trim(u8, lines[start].text, " \t\r").len == 0) : (start += 1) {}

    var end = lines.len;
    while (end > start and std.mem.trim(u8, lines[end - 1].text, " \t\r").len == 0) : (end -= 1) {}

    if (start >= end) return allocator.dupe(u8, "");

    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);

    var i = start;
    while (i < end) : (i += 1) {
        try out.appendSlice(allocator, lines[i].text);
        if (i + 1 < end) try out.append(allocator, '\n');
    }

    return try out.toOwnedSlice(allocator);
}

fn trimLeftWhitespace(s: []const u8) []const u8 {
    var idx: usize = 0;
    while (idx < s.len and std.ascii.isWhitespace(s[idx])) : (idx += 1) {}
    return s[idx..];
}
