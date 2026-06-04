//! cli_usage_lint — validate authored CLI invocations against the live
//! command schema.
//!
//! Every Planar binary exposes `<bin> schema`, a deterministic flat JSON
//! catalog of its command tree (commands, subcommands, and per-command
//! flags with inherited flags already merged in). This tool dumps that
//! schema for each binary, then scans the authored workflow surfaces
//! (agents/, skills/src/, docs/) for command invocations inside code
//! spans and fenced blocks, and reports any `--flag` referenced on a
//! command that the binary does not actually expose.
//!
//! This catches the drift class where a skill, agent doc, or reference
//! doc tells an operator (or an LLM) to run `planar workbench list
//! --plan 467` when `workbench list` has no `--plan` flag.
//!
//! Usage:
//!   cli_usage_lint <repo-root> <bin-path> [<bin-path> ...]
//!
//! Each <bin-path> is an executable whose basename is the binary name as
//! it appears in prose (planar, planar-agent, planar-watch, planar-doc).
//! The tool scans <repo-root>/{agents, skills/src, docs} for *.md files.
//!
//! A line containing the marker `cli-lint-ignore` is skipped entirely —
//! the escape hatch for intentional references to flags that do not exist
//! (documenting future or removed flags).
//!
//! Exit codes: 0 = clean, 1 = violations found, 2 = usage / internal error.

const std = @import("std");
const Io = std.Io;

/// Flags the parser auto-provides on every command but does not list in
/// the schema. Referencing these is always valid.
const global_ok_flags = [_][]const u8{ "--help", "-h" };

/// Directories under the repo root that hold authored CLI prose.
const scan_dirs = [_][]const u8{ "agents", "skills/src", "docs" };

// ---------------------------------------------------------------------------
// Schema model (subset of `<bin> schema` JSON we consume).
// ---------------------------------------------------------------------------

const FlagJson = struct {
    long: []const u8,
    aliases: [][]const u8 = &.{},
    /// Optional single-char short flag as a string, e.g. `"h"` for `-h`. Null when absent.
    short: ?[]const u8 = null,
};

const CommandJson = struct {
    command: []const u8,
    subcommands: [][]const u8 = &.{},
    flags: []FlagJson = &.{},
};

const SchemaJson = struct {
    commands: []CommandJson,
};

/// One resolved command: its allowed flag tokens and whether it is a leaf.
const Command = struct {
    flags: std.StringHashMapUnmanaged(void) = .{},
    is_leaf: bool = true,
};

const Catalog = struct {
    /// "planar workbench list" -> Command.
    commands: std.StringHashMapUnmanaged(Command) = .{},

    fn contains(self: *const Catalog, key: []const u8) bool {
        return self.commands.contains(key);
    }
    fn get(self: *const Catalog, key: []const u8) ?*Command {
        return self.commands.getPtr(key);
    }
};

const Violation = struct {
    file: []const u8,
    line: usize,
    command: []const u8,
    flag: []const u8,
    suggestion: ?[]const u8,
};

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const io = init.io;

    const args = try init.minimal.args.toSlice(arena);
    if (args.len < 3) {
        std.debug.print("usage: {s} <repo-root> <bin-path> [<bin-path> ...]\n", .{args[0]});
        std.process.exit(2);
    }
    const repo_root = args[1];
    const bin_paths = args[2..];

    // Build one merged catalog keyed by full command string. The leading
    // token of every key is the binary name, so the four trees never
    // collide.
    var catalog = Catalog{};
    for (bin_paths) |bin_path| {
        loadSchema(arena, io, &catalog, bin_path) catch |e| {
            std.debug.print("error: failed to load schema from {s}: {s}\n", .{ bin_path, @errorName(e) });
            std.process.exit(2);
        };
    }

    const bin_names = try arena.alloc([]const u8, bin_paths.len);
    for (bin_paths, 0..) |p, i| bin_names[i] = std.fs.path.basename(p);

    var violations: std.ArrayList(Violation) = .empty;
    var files_scanned: usize = 0;

    for (scan_dirs) |rel| {
        const dir_path = try std.fs.path.join(arena, &.{ repo_root, rel });
        scanDir(arena, io, &catalog, bin_names, dir_path, &violations, &files_scanned) catch |e| switch (e) {
            error.FileNotFound => {}, // a missing scan dir is not fatal
            else => {
                std.debug.print("error: scanning {s}: {s}\n", .{ dir_path, @errorName(e) });
                std.process.exit(2);
            },
        };
    }

    if (violations.items.len == 0) {
        std.debug.print("cli-usage-lint: clean ({d} files, {d} commands)\n", .{ files_scanned, catalog.commands.count() });
        return;
    }

    std.mem.sort(Violation, violations.items, {}, lessThanViolation);
    for (violations.items) |v| {
        if (v.suggestion) |s| {
            std.debug.print("{s}:{d}: `{s}` has no flag `{s}` (did you mean `{s}`?)\n", .{ v.file, v.line, v.command, v.flag, s });
        } else {
            std.debug.print("{s}:{d}: `{s}` has no flag `{s}`\n", .{ v.file, v.line, v.command, v.flag });
        }
    }
    std.debug.print("cli-usage-lint: {d} violation(s) across {d} files\n", .{ violations.items.len, files_scanned });
    std.process.exit(1);
}

fn lessThanViolation(_: void, a: Violation, b: Violation) bool {
    const f = std.mem.order(u8, a.file, b.file);
    if (f != .eq) return f == .lt;
    return a.line < b.line;
}

// ---------------------------------------------------------------------------
// Schema loading.
// ---------------------------------------------------------------------------

fn loadSchema(arena: std.mem.Allocator, io: Io, catalog: *Catalog, bin_path: []const u8) !void {
    const res = try std.process.run(arena, io, .{
        .argv = &.{ bin_path, "schema" },
    });
    if (res.term != .exited or res.term.exited != 0) return error.SchemaCommandFailed;

    const parsed = try std.json.parseFromSlice(SchemaJson, arena, res.stdout, .{
        .ignore_unknown_fields = true,
        .allocate = .alloc_always,
    });
    const schema = parsed.value;

    for (schema.commands) |c| {
        var cmd = Command{ .is_leaf = c.subcommands.len == 0 };
        for (c.flags) |f| {
            try cmd.flags.put(arena, f.long, {});
            for (f.aliases) |a| try cmd.flags.put(arena, a, {});
            if (f.short) |s| {
                const short = try std.fmt.allocPrint(arena, "-{s}", .{s});
                try cmd.flags.put(arena, short, {});
            }
        }
        try catalog.commands.put(arena, try arena.dupe(u8, c.command), cmd);
    }
}

// ---------------------------------------------------------------------------
// Directory walk.
// ---------------------------------------------------------------------------

fn scanDir(
    arena: std.mem.Allocator,
    io: Io,
    catalog: *const Catalog,
    bin_names: []const []const u8,
    dir_path: []const u8,
    violations: *std.ArrayList(Violation),
    files_scanned: *usize,
) !void {
    var dir = try Io.Dir.cwd().openDir(io, dir_path, .{ .iterate = true });
    defer dir.close(io);

    var it = dir.iterate();
    while (try it.next(io)) |entry| {
        const child = try std.fs.path.join(arena, &.{ dir_path, entry.name });
        switch (entry.kind) {
            .directory => try scanDir(arena, io, catalog, bin_names, child, violations, files_scanned),
            .file => {
                if (!std.mem.endsWith(u8, entry.name, ".md")) continue;
                files_scanned.* += 1;
                const content = try Io.Dir.cwd().readFileAlloc(io, child, arena, .unlimited);
                try scanFile(arena, catalog, bin_names, child, content, violations);
            },
            else => {},
        }
    }
}

// ---------------------------------------------------------------------------
// Per-file scan.
// ---------------------------------------------------------------------------

fn scanFile(
    arena: std.mem.Allocator,
    catalog: *const Catalog,
    bin_names: []const []const u8,
    file: []const u8,
    content: []const u8,
    violations: *std.ArrayList(Violation),
) !void {
    var in_fence = false;
    var line_no: usize = 0;
    var lines = std.mem.splitScalar(u8, content, '\n');
    while (lines.next()) |line| {
        line_no += 1;
        const trimmed = std.mem.trimStart(u8, line, " \t");
        if (std.mem.startsWith(u8, trimmed, "```") or std.mem.startsWith(u8, trimmed, "~~~")) {
            in_fence = !in_fence;
            continue;
        }
        // Escape hatch for intentional references to flags that do not
        // exist (e.g. documenting future or removed flags). A line
        // containing `cli-lint-ignore` is skipped entirely.
        if (std.mem.indexOf(u8, line, "cli-lint-ignore") != null) continue;

        // Collect the code text of this line: the whole line when inside a
        // fence, otherwise only the inline `code` spans.
        if (in_fence) {
            try scanCodeSegment(arena, catalog, bin_names, file, line_no, line, violations);
        } else {
            try scanInlineSpans(arena, catalog, bin_names, file, line_no, line, violations);
        }
    }
}

/// Extract `...` inline code spans from a prose line and scan each.
fn scanInlineSpans(
    arena: std.mem.Allocator,
    catalog: *const Catalog,
    bin_names: []const []const u8,
    file: []const u8,
    line_no: usize,
    line: []const u8,
    violations: *std.ArrayList(Violation),
) !void {
    var i: usize = 0;
    while (i < line.len) {
        if (line[i] != '`') {
            i += 1;
            continue;
        }
        const start = i + 1;
        var j = start;
        while (j < line.len and line[j] != '`') j += 1;
        if (j >= line.len) break; // unterminated span
        try scanCodeSegment(arena, catalog, bin_names, file, line_no, line[start..j], violations);
        i = j + 1;
    }
}

/// A code segment may contain several shell commands joined by pipes /
/// separators. Split on those, then look for binary invocations.
fn scanCodeSegment(
    arena: std.mem.Allocator,
    catalog: *const Catalog,
    bin_names: []const []const u8,
    file: []const u8,
    line_no: usize,
    segment: []const u8,
    violations: *std.ArrayList(Violation),
) !void {
    // Split on shell separators only. We deliberately do NOT split on
    // `<`/`>`: those almost always delimit `<placeholder>` metavars
    // mid-command in this prose, and splitting on them would shred the
    // invocation (dropping the flags after the first placeholder). Real
    // stdin/stdout redirections are rare here and harmless — the post-
    // redirection fragment carries no binary token and is skipped.
    var parts = std.mem.tokenizeAny(u8, segment, "|;&()`");
    while (parts.next()) |part| {
        try scanCommand(arena, catalog, bin_names, file, line_no, part, violations);
    }
}

/// Scan a single shell-command-ish run of text for a binary invocation.
fn scanCommand(
    arena: std.mem.Allocator,
    catalog: *const Catalog,
    bin_names: []const []const u8,
    file: []const u8,
    line_no: usize,
    text: []const u8,
    violations: *std.ArrayList(Violation),
) !void {
    var toks: std.ArrayList([]const u8) = .empty;
    var it = std.mem.tokenizeAny(u8, text, " \t");
    while (it.next()) |t| try toks.append(arena, cleanToken(t));

    // Find the binary token.
    var bi: ?usize = null;
    var bin_name: []const u8 = "";
    for (toks.items, 0..) |t, idx| {
        for (bin_names) |name| {
            if (std.mem.eql(u8, t, name)) {
                bi = idx;
                bin_name = name;
                break;
            }
        }
        if (bi != null) break;
    }
    const bin_idx = bi orelse return;
    const rest = toks.items[bin_idx + 1 ..];

    // Resolve the deepest known command path.
    var resolved = bin_name;
    var k: usize = 0;
    var stop_reason: enum { end, flag, placeholder, word } = .end;
    while (k < rest.len) : (k += 1) {
        const t = rest[k];
        if (isFlag(t)) {
            stop_reason = .flag;
            break;
        }
        if (isPlaceholder(t)) {
            stop_reason = .placeholder;
            break;
        }
        const candidate = try std.fmt.allocPrint(arena, "{s} {s}", .{ resolved, t });
        if (catalog.contains(candidate)) {
            resolved = candidate;
        } else {
            stop_reason = .word;
            break;
        }
    }

    const cmd = catalog.get(resolved) orelse return;

    // Ambiguity guard: if we stopped at a placeholder/value while the
    // resolved command still has subcommands, we cannot know the real
    // leaf, so its flag set is unknown. Skip to avoid false positives.
    if (!cmd.is_leaf and (stop_reason == .placeholder or stop_reason == .word)) return;

    // Validate every flag token in the remainder against the resolved set.
    for (rest) |t| {
        if (!isFlag(t)) continue;
        const name = flagName(t);
        if (name.len == 0) continue; // bare "--" passthrough or "-"
        if (isPlaceholderFlag(name)) continue;
        if (isGlobalOk(name)) continue;
        if (cmd.flags.contains(name)) continue;
        try violations.append(arena, .{
            .file = file,
            .line = line_no,
            .command = try arena.dupe(u8, resolved),
            .flag = try arena.dupe(u8, name),
            .suggestion = suggest(arena, cmd, name),
        });
    }
}

// ---------------------------------------------------------------------------
// Token classification helpers.
// ---------------------------------------------------------------------------

/// Strip surrounding prose punctuation/quotes that can cling to a token.
fn cleanToken(t: []const u8) []const u8 {
    var s = t;
    while (s.len > 0 and isTrailingJunk(s[s.len - 1])) s = s[0 .. s.len - 1];
    while (s.len > 0 and isLeadingJunk(s[0])) s = s[1..];
    return s;
}

fn isTrailingJunk(c: u8) bool {
    return switch (c) {
        '.', ',', ';', ':', ')', ']', '}', '"', '\'', '`' => true,
        else => false,
    };
}

fn isLeadingJunk(c: u8) bool {
    return switch (c) {
        '(', '[', '{', '"', '\'', '`', '$' => true,
        else => false,
    };
}

fn isFlag(t: []const u8) bool {
    return t.len >= 2 and t[0] == '-';
}

/// The flag name with any `=value` suffix removed.
fn flagName(t: []const u8) []const u8 {
    if (std.mem.indexOfScalar(u8, t, '=')) |eq| return t[0..eq];
    return t;
}

/// Placeholder/value token where a subcommand could go: `<id>`, `...`,
/// `{x}`, `NNN`, `$VAR`, all-caps METAVAR, or a number.
fn isPlaceholder(t: []const u8) bool {
    if (t.len == 0) return true;
    if (t[0] == '<' or t[0] == '{' or t[0] == '$') return true;
    if (std.mem.eql(u8, t, "...")) return true;
    if (std.mem.eql(u8, t, "[flags]") or std.mem.eql(u8, t, "[options]")) return true;
    // All-uppercase metavar (FILE, PLAN_ID) or all-digit id.
    var all_upper = true;
    var all_digit = true;
    for (t) |c| {
        if (!(std.ascii.isUpper(c) or c == '_')) all_upper = false;
        if (!std.ascii.isDigit(c)) all_digit = false;
    }
    return all_upper or all_digit;
}

/// A flag token that is itself a placeholder, not a literal flag —
/// e.g. `--flag`, `--<opt>`, `--foo|--bar`, anything with brackets.
fn isPlaceholderFlag(name: []const u8) bool {
    for (name) |c| {
        switch (c) {
            '<', '>', '[', ']', '{', '}', '|' => return true,
            else => {},
        }
    }
    return false;
}

fn isGlobalOk(name: []const u8) bool {
    for (global_ok_flags) |g| {
        if (std.mem.eql(u8, name, g)) return true;
    }
    return false;
}

/// Closest allowed flag by Levenshtein distance, when within threshold.
fn suggest(arena: std.mem.Allocator, cmd: *const Command, name: []const u8) ?[]const u8 {
    var best: ?[]const u8 = null;
    var best_d: usize = std.math.maxInt(usize);
    var it = cmd.flags.keyIterator();
    while (it.next()) |k| {
        const d = levenshtein(arena, name, k.*) catch continue;
        if (d < best_d) {
            best_d = d;
            best = k.*;
        }
    }
    // Only suggest when the names are genuinely close.
    if (best) |b| {
        if (best_d <= 3 and best_d < name.len) return b;
    }
    return null;
}

fn levenshtein(arena: std.mem.Allocator, a: []const u8, b: []const u8) !usize {
    const row = try arena.alloc(usize, b.len + 1);
    defer arena.free(row);
    for (0..b.len + 1) |j| row[j] = j;
    for (a, 0..) |ca, i| {
        var prev = row[0];
        row[0] = i + 1;
        for (b, 0..) |cb, j| {
            const tmp = row[j + 1];
            const cost: usize = if (ca == cb) 0 else 1;
            row[j + 1] = @min(@min(row[j] + 1, row[j + 1] + 1), prev + cost);
            prev = tmp;
        }
    }
    return row[b.len];
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

const testing = std.testing;

fn testCatalog(arena: std.mem.Allocator) !Catalog {
    var c = Catalog{};
    // planar workbench list: only --json (leaf)
    {
        var cmd = Command{ .is_leaf = true };
        try cmd.flags.put(arena, "--json", {});
        try c.commands.put(arena, "planar workbench list", cmd);
    }
    // planar task done: --scope, --json (leaf)
    {
        var cmd = Command{ .is_leaf = true };
        try cmd.flags.put(arena, "--scope", {});
        try cmd.flags.put(arena, "--json", {});
        try c.commands.put(arena, "planar task done", cmd);
    }
    // parent nodes (non-leaf, no flags)
    {
        try c.commands.put(arena, "planar", .{ .is_leaf = false });
        try c.commands.put(arena, "planar workbench", .{ .is_leaf = false });
        try c.commands.put(arena, "planar task", .{ .is_leaf = false });
    }
    return c;
}

test "flags valid on a leaf command pass" {
    var arena_i = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_i.deinit();
    const arena = arena_i.allocator();
    var c = try testCatalog(arena);
    var v: std.ArrayList(Violation) = .empty;
    try scanCommand(arena, &c, &.{"planar"}, "f.md", 1, "planar workbench list --json", &v);
    try testing.expectEqual(@as(usize, 0), v.items.len);
}

test "unknown flag on a leaf command is flagged" {
    var arena_i = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_i.deinit();
    const arena = arena_i.allocator();
    var c = try testCatalog(arena);
    var v: std.ArrayList(Violation) = .empty;
    try scanCommand(arena, &c, &.{"planar"}, "f.md", 7, "planar workbench list --plan 467", &v);
    try testing.expectEqual(@as(usize, 1), v.items.len);
    try testing.expectEqualStrings("planar workbench list", v.items[0].command);
    try testing.expectEqualStrings("--plan", v.items[0].flag);
}

test "placeholder under a non-leaf parent is skipped (no false positive)" {
    var arena_i = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_i.deinit();
    const arena = arena_i.allocator();
    var c = try testCatalog(arena);
    var v: std.ArrayList(Violation) = .empty;
    // `planar task <verb> --json`: resolves to non-leaf "planar task",
    // stops at placeholder → ambiguous → skipped.
    try scanCommand(arena, &c, &.{"planar"}, "f.md", 1, "planar task <verb> --json", &v);
    try testing.expectEqual(@as(usize, 0), v.items.len);
}

test "value before flags on a leaf still validates" {
    var arena_i = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_i.deinit();
    const arena = arena_i.allocator();
    var c = try testCatalog(arena);
    var v: std.ArrayList(Violation) = .empty;
    try scanCommand(arena, &c, &.{"planar"}, "f.md", 1, "planar task done 42 --bogus", &v);
    try testing.expectEqual(@as(usize, 1), v.items.len);
    try testing.expectEqualStrings("--bogus", v.items[0].flag);
}

test "--help and = suffix and placeholder flags are tolerated" {
    var arena_i = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_i.deinit();
    const arena = arena_i.allocator();
    var c = try testCatalog(arena);
    var v: std.ArrayList(Violation) = .empty;
    try scanCommand(arena, &c, &.{"planar"}, "f.md", 1, "planar task done --help --scope=foo --<opt>", &v);
    try testing.expectEqual(@as(usize, 0), v.items.len);
}

test "cleanToken strips clinging prose punctuation" {
    try testing.expectEqualStrings("planar", cleanToken("(planar"));
    try testing.expectEqualStrings("--json", cleanToken("--json)."));
    try testing.expectEqualStrings("list", cleanToken("`list`"));
}

test "inline span extraction finds invocations in prose" {
    var arena_i = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_i.deinit();
    const arena = arena_i.allocator();
    var c = try testCatalog(arena);
    var v: std.ArrayList(Violation) = .empty;
    const line = "Run `planar workbench list --plan 5` to see them.";
    try scanInlineSpans(arena, &c, &.{"planar"}, "f.md", 3, line, &v);
    try testing.expectEqual(@as(usize, 1), v.items.len);
    try testing.expectEqualStrings("--plan", v.items[0].flag);
}

test "FlagJson: short field parses as string — emitter shape match (task 3235)" {
    // The etc-cli emitter serializes `short` as a JSON string: "short":"v", NOT
    // a JSON number. Confirms ?[]const u8 parses the emitter output correctly
    // (the prior ?u8 declaration would have produced ParseFailed on any binary
    // that defines a short flag).
    var arena_i = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_i.deinit();
    const arena = arena_i.allocator();

    const fixture =
        \\{"commands":[
        \\  {"command":"planar verbose","subcommands":[],
        \\   "flags":[
        \\     {"long":"--verbose","aliases":[],"short":"v"}
        \\   ]}
        \\]}
    ;
    const parsed = try std.json.parseFromSlice(SchemaJson, arena, fixture, .{
        .ignore_unknown_fields = true,
        .allocate = .alloc_always,
    });
    try testing.expectEqual(@as(usize, 1), parsed.value.commands.len);
    const flag = parsed.value.commands[0].flags[0];
    try testing.expectEqualStrings("--verbose", flag.long);
    // short must be the string "v", not null or a numeric byte.
    const s = flag.short orelse return error.TestUnexpectedNull;
    try testing.expectEqualStrings("v", s);
}
