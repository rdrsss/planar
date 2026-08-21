//! engine/templates/loader — disk + embedded template resolution.
//!
//! Mirrors Go `internal/templates/loader.go`. Resolution chain for `load`:
//!
//!   1. <root>/<set>/<system>/<kind>.json     (user-chosen set on disk)
//!   2. <root>/default/<system>/<kind>.json   (baseline set on disk)
//!   3. embedded defaults                     (always present in binary)
//!
//! The first readable, valid JSON file wins. JSON shape is parsed eagerly
//! so `render`/`validate` can walk the decoded tree without re-parsing.

const std = @import("std");
const embed = @import("templates_embed");

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

/// Template is a parsed template file. Owns its allocations through `deinitTemplate`.
pub const Template = struct {
    /// Template set this template was resolved from.
    set_name: []const u8,
    /// External system slug (e.g. "github-issues", "jira").
    system: []const u8,
    /// Kind within the set (e.g. "issue", "epic").
    kind: []const u8,
    /// "disk" or "embedded".
    source: []const u8,
    /// Filesystem path (disk) or "embedded:<path>" (embedded).
    path: []const u8,
    /// Verbatim JSON content.
    raw: []const u8,
    /// Decoded JSON tree. Lifetime tied to `parsed`; do not free directly.
    fields: std.json.Value,
    /// Backing parser for `fields`. Held so we can free it on deinit.
    parsed: *std.json.Parsed(std.json.Value),
};

/// deinitTemplate frees every allocation owned by `t`.
pub fn deinitTemplate(t: Template, allocator: std.mem.Allocator) void {
    allocator.free(t.set_name);
    allocator.free(t.system);
    allocator.free(t.kind);
    allocator.free(t.source);
    allocator.free(t.path);
    allocator.free(t.raw);
    t.parsed.deinit();
    allocator.destroy(t.parsed);
}

/// ListEntry describes one available template triple discovered on disk
/// or in the embedded defaults.
pub const ListEntry = struct {
    set_name: []const u8,
    system: []const u8,
    kind: []const u8,
    /// "disk" or "embedded".
    source: []const u8,
    /// Filesystem path (disk) or "embedded:<path>" (embedded).
    path: []const u8,
};

/// deinitListEntries frees every allocation owned by `entries`, then the
/// outer slice.
pub fn deinitListEntries(entries: []const ListEntry, allocator: std.mem.Allocator) void {
    for (entries) |e| {
        allocator.free(e.set_name);
        allocator.free(e.system);
        allocator.free(e.kind);
        allocator.free(e.source);
        allocator.free(e.path);
    }
    allocator.free(entries);
}

pub const LoadError = error{
    TemplateNotFound,
    InvalidJson,
    OutOfMemory,
    AccessDenied,
    SystemResources,
    Unexpected,
};

/// load resolves and parses a template using the three-level fallback chain.
/// `root` is the user's templates directory (may be empty). `set_name`,
/// `system`, and `kind` identify the template; `set_name == "default"` is
/// honoured (the user-set lookup is skipped to avoid checking the same path
/// twice).
pub fn load(
    allocator: std.mem.Allocator,
    set_name: []const u8,
    system: []const u8,
    kind: []const u8,
    root: []const u8,
) !Template {
    var buf: [std.fs.max_path_bytes]u8 = undefined;
    var fb: std.heap.FixedBufferAllocator = .init(&buf);
    const path_alloc = fb.allocator();

    // 1. <root>/<set>/<system>/<kind>.json — skip when set is "default".
    if (root.len > 0 and !std.mem.eql(u8, set_name, "default")) {
        const candidate = try std.fs.path.join(path_alloc, &.{ root, set_name, system, std.fmt.allocPrint(path_alloc, "{s}.json", .{kind}) catch unreachable });
        if (loadFromDisk(allocator, set_name, system, kind, candidate)) |t| {
            return t;
        } else |_| {}
        fb.reset();
    }

    // 2. <root>/default/<system>/<kind>.json — baseline on disk.
    if (root.len > 0) {
        const candidate = try std.fs.path.join(path_alloc, &.{ root, "default", system, std.fmt.allocPrint(path_alloc, "{s}.json", .{kind}) catch unreachable });
        if (loadFromDisk(allocator, "default", system, kind, candidate)) |t| {
            return t;
        } else |_| {}
        fb.reset();
    }

    // 3. Embedded defaults.
    if (loadFromEmbedded(allocator, set_name, system, kind)) |t| {
        return t;
    } else |_| {}

    return error.TemplateNotFound;
}

fn loadFromDisk(
    allocator: std.mem.Allocator,
    set_name: []const u8,
    system: []const u8,
    kind: []const u8,
    path: []const u8,
) !Template {
    const raw_owned = std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(4 * 1024 * 1024)) catch
        return error.TemplateNotFound;
    errdefer allocator.free(raw_owned);

    return try parseTemplate(allocator, set_name, system, kind, "disk", path, raw_owned);
}

fn loadFromEmbedded(
    allocator: std.mem.Allocator,
    set_name: []const u8,
    system: []const u8,
    kind: []const u8,
) !Template {
    for (embed.all) |entry| {
        if (!std.mem.eql(u8, entry.system, system)) continue;
        if (!std.mem.eql(u8, entry.kind, kind)) continue;
        const raw_owned = try allocator.dupe(u8, entry.body);
        errdefer allocator.free(raw_owned);
        const path = try std.fmt.allocPrint(allocator, "embedded:{s}/{s}.json", .{ system, kind });
        defer allocator.free(path);
        return try parseTemplate(allocator, set_name, system, kind, "embedded", path, raw_owned);
    }
    return error.TemplateNotFound;
}

/// parseTemplate decodes raw JSON bytes into a Template and takes ownership
/// of `raw_owned` on success. On parse failure, `raw_owned` is freed and an
/// error is returned.
fn parseTemplate(
    allocator: std.mem.Allocator,
    set_name: []const u8,
    system: []const u8,
    kind: []const u8,
    source: []const u8,
    path: []const u8,
    raw_owned: []u8,
) !Template {
    const parsed_ptr = try allocator.create(std.json.Parsed(std.json.Value));
    errdefer allocator.destroy(parsed_ptr);

    parsed_ptr.* = std.json.parseFromSlice(std.json.Value, allocator, raw_owned, .{}) catch {
        allocator.free(raw_owned);
        return error.InvalidJson;
    };
    errdefer parsed_ptr.deinit();

    return .{
        .set_name = try allocator.dupe(u8, set_name),
        .system = try allocator.dupe(u8, system),
        .kind = try allocator.dupe(u8, kind),
        .source = try allocator.dupe(u8, source),
        .path = try allocator.dupe(u8, path),
        .raw = raw_owned,
        .fields = parsed_ptr.value,
        .parsed = parsed_ptr,
    };
}

/// listEntries enumerates available (set, system, kind) triples under
/// `root` by walking the disk hierarchy. Embedded defaults are not
/// included — call `listEmbeddedEntries` separately.
pub fn listEntries(allocator: std.mem.Allocator, root: []const u8) ![]ListEntry {
    if (root.len == 0) return try allocator.alloc(ListEntry, 0);

    var entries: std.ArrayList(ListEntry) = .empty;
    errdefer {
        for (entries.items) |e| {
            allocator.free(e.set_name);
            allocator.free(e.system);
            allocator.free(e.kind);
            allocator.free(e.source);
            allocator.free(e.path);
        }
        entries.deinit(allocator);
    }

    const io = fsIo();
    var root_dir = std.Io.Dir.cwd().openDir(io, root, .{ .iterate = true }) catch |e| switch (e) {
        error.FileNotFound, error.NotDir => return try entries.toOwnedSlice(allocator),
        else => return e,
    };
    defer root_dir.close(io);

    var sets_it = root_dir.iterate();
    while (try sets_it.next(io)) |set_entry| {
        if (set_entry.kind != .directory) continue;
        const set_name_owned = try allocator.dupe(u8, set_entry.name);
        defer allocator.free(set_name_owned);
        var set_dir = root_dir.openDir(io, set_name_owned, .{ .iterate = true }) catch continue;
        defer set_dir.close(io);

        var sys_it = set_dir.iterate();
        while (try sys_it.next(io)) |sys_entry| {
            if (sys_entry.kind != .directory) continue;
            const sys_name_owned = try allocator.dupe(u8, sys_entry.name);
            defer allocator.free(sys_name_owned);
            var sys_dir = set_dir.openDir(io, sys_name_owned, .{ .iterate = true }) catch continue;
            defer sys_dir.close(io);

            var file_it = sys_dir.iterate();
            while (try file_it.next(io)) |file_entry| {
                if (file_entry.kind != .file) continue;
                if (!std.mem.endsWith(u8, file_entry.name, ".json")) continue;
                const kind_name = file_entry.name[0 .. file_entry.name.len - ".json".len];
                const full_path = try std.fs.path.join(allocator, &.{ root, set_name_owned, sys_name_owned, file_entry.name });
                try entries.append(allocator, .{
                    .set_name = try allocator.dupe(u8, set_name_owned),
                    .system = try allocator.dupe(u8, sys_name_owned),
                    .kind = try allocator.dupe(u8, kind_name),
                    .source = try allocator.dupe(u8, "disk"),
                    .path = full_path,
                });
            }
        }
    }

    return try entries.toOwnedSlice(allocator);
}

/// listEmbeddedEntries returns one ListEntry per embedded template.
pub fn listEmbeddedEntries(allocator: std.mem.Allocator) ![]ListEntry {
    var entries: std.ArrayList(ListEntry) = .empty;
    errdefer {
        for (entries.items) |e| {
            allocator.free(e.set_name);
            allocator.free(e.system);
            allocator.free(e.kind);
            allocator.free(e.source);
            allocator.free(e.path);
        }
        entries.deinit(allocator);
    }

    for (embed.all) |t| {
        const path = try std.fmt.allocPrint(allocator, "embedded:{s}/{s}.json", .{ t.system, t.kind });
        try entries.append(allocator, .{
            .set_name = try allocator.dupe(u8, "default"),
            .system = try allocator.dupe(u8, t.system),
            .kind = try allocator.dupe(u8, t.kind),
            .source = try allocator.dupe(u8, "embedded"),
            .path = path,
        });
    }

    return try entries.toOwnedSlice(allocator);
}

test "load returns embedded template when no root is given" {
    const a = std.testing.allocator;
    const t = try load(a, "default", "github-issues", "issue", "");
    defer deinitTemplate(t, a);
    try std.testing.expectEqualStrings("github-issues", t.system);
    try std.testing.expectEqualStrings("issue", t.kind);
    try std.testing.expectEqualStrings("embedded", t.source);
    try std.testing.expect(t.raw.len > 0);
    try std.testing.expect(t.fields == .object);
}

test "load returns error.TemplateNotFound for unknown kind" {
    const a = std.testing.allocator;
    const err = load(a, "default", "github-issues", "no-such-kind", "");
    try std.testing.expectError(error.TemplateNotFound, err);
}

test "listEmbeddedEntries returns every embedded triple" {
    const a = std.testing.allocator;
    const entries = try listEmbeddedEntries(a);
    defer deinitListEntries(entries, a);
    try std.testing.expect(entries.len >= 3);
    for (entries) |e| {
        try std.testing.expectEqualStrings("default", e.set_name);
        try std.testing.expectEqualStrings("embedded", e.source);
    }
}

test "listEntries on nonexistent root returns empty slice" {
    const a = std.testing.allocator;
    const entries = try listEntries(a, "/nonexistent/templates/root");
    defer deinitListEntries(entries, a);
    try std.testing.expectEqual(@as(usize, 0), entries.len);
}
