//! workflow/scan.zig — scan a directory for `*.lua` workflow files and
//! parse their `@meta` blocks.
//!
//! Callers pass a directory path and a `local` flag.  The scanner opens
//! the directory (no-ops silently when absent) and returns an
//! allocator-owned slice of `WorkflowEntry` values.

const std = @import("std");
const meta = @import("meta.zig");

/// A discovered workflow entry.
pub const WorkflowEntry = struct {
    /// Absolute path to the `.lua` source file (allocator-owned).
    path: []const u8,
    /// Base filename (e.g. `finalize_closeout.lua`), points into `path`.
    filename: []const u8,
    /// Parsed `@meta` block fields (allocator-owned strings).
    workflow_meta: meta.WorkflowMeta,
    /// true when a `--[[ @meta … --]]` block was found in the file.
    meta_found: bool,
    /// true when sourced from the sandbox (`~/.planar/local/workflows/`).
    is_local: bool,

    /// effectiveName returns the name to display and match on:
    ///   1. `workflow_meta.name` when the @meta block is present and has a name.
    ///   2. The filename stem (sans `.lua`) otherwise.
    pub fn effectiveName(self: WorkflowEntry) []const u8 {
        if (self.meta_found and self.workflow_meta.name.len > 0) {
            return self.workflow_meta.name;
        }
        if (std.mem.endsWith(u8, self.filename, ".lua")) {
            return self.filename[0 .. self.filename.len - 4];
        }
        return self.filename;
    }
};

/// scan reads every `*.lua` file in `dir_path`, parses its `@meta` block,
/// and returns an allocator-owned slice of `WorkflowEntry`.
///
/// If `dir_path` does not exist or cannot be opened, an empty slice is
/// returned (not an error) — an absent directory is treated as empty.
///
/// The returned entries are sorted by effective name for stable output.
pub fn scan(
    dir_path: []const u8,
    is_local: bool,
    allocator: std.mem.Allocator,
    io: std.Io,
) ![]WorkflowEntry {
    var dir = std.Io.Dir.cwd().openDir(io, dir_path, .{ .iterate = true }) catch {
        // Directory absent or inaccessible — return empty slice.
        return &.{};
    };
    defer dir.close(io);

    var entries: std.ArrayList(WorkflowEntry) = .empty;
    errdefer {
        for (entries.items) |e| freeEntry(e, allocator);
        entries.deinit(allocator);
    }

    var it = dir.iterate();
    while (try it.next(io)) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".lua")) continue;

        // Build the absolute path.
        const abs_path = try std.fs.path.join(allocator, &.{ dir_path, entry.name });
        errdefer allocator.free(abs_path);

        // The filename slice is a view into abs_path.
        const filename = std.fs.path.basename(abs_path);

        // Read the file content to extract @meta.
        const content = std.Io.Dir.cwd().readFileAlloc(
            io,
            abs_path,
            allocator,
            std.Io.Limit.limited(256 * 1024),
        ) catch {
            // Unreadable file: record an entry with no meta.
            try entries.append(allocator, .{
                .path = abs_path,
                .filename = filename,
                .workflow_meta = .{
                    .name = try allocator.dupe(u8, ""),
                    .description = try allocator.dupe(u8, ""),
                    .phases = try allocator.dupe(u8, ""),
                    .seam = try allocator.dupe(u8, ""),
                },
                .meta_found = false,
                .is_local = is_local,
            });
            continue;
        };
        defer allocator.free(content);

        const parsed = meta.parse(content);

        // Dupe meta strings onto allocator so they survive content free.
        const duped_name = try allocator.dupe(u8, parsed.meta.name);
        errdefer allocator.free(duped_name);
        const duped_desc = try allocator.dupe(u8, parsed.meta.description);
        errdefer allocator.free(duped_desc);
        const duped_phases = try allocator.dupe(u8, parsed.meta.phases);
        errdefer allocator.free(duped_phases);
        const duped_seam = try allocator.dupe(u8, parsed.meta.seam);
        errdefer allocator.free(duped_seam);

        try entries.append(allocator, .{
            .path = abs_path,
            .filename = filename,
            .workflow_meta = .{
                .name = duped_name,
                .description = duped_desc,
                .phases = duped_phases,
                .seam = duped_seam,
            },
            .meta_found = parsed.found,
            .is_local = is_local,
        });
    }

    const items = try entries.toOwnedSlice(allocator);
    std.mem.sort(WorkflowEntry, items, {}, struct {
        fn lessThan(_: void, a: WorkflowEntry, b: WorkflowEntry) bool {
            return std.mem.lessThan(u8, a.effectiveName(), b.effectiveName());
        }
    }.lessThan);
    return items;
}

/// freeEntry frees every allocator-owned string in a single WorkflowEntry.
/// Does NOT free the entry struct itself (caller-owned value type).
pub fn freeEntry(e: WorkflowEntry, allocator: std.mem.Allocator) void {
    allocator.free(e.path);
    allocator.free(e.workflow_meta.name);
    allocator.free(e.workflow_meta.description);
    allocator.free(e.workflow_meta.phases);
    allocator.free(e.workflow_meta.seam);
}

/// deinitEntries frees every entry's strings and then the slice itself.
/// Use when the caller owns the full slice returned by `scan`.
pub fn deinitEntries(entries: []WorkflowEntry, allocator: std.mem.Allocator) void {
    for (entries) |e| freeEntry(e, allocator);
    allocator.free(entries);
}
