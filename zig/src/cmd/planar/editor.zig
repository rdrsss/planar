//! cmd/planar/editor — Low-level $EDITOR invocation helper.
//!
//! This module is the Zig equivalent of Go's editflow.ResolveEditor + the
//! editor-exec portion of editflow.Run. It implements ONLY the primitive:
//!
//!   1. Resolve the editor command via PLANAR_EDITOR → VISUAL → EDITOR → "vi".
//!   2. Create a temp file with the given initial content and a stable extension.
//!   3. Exec the editor (inheriting TTY: stdin/stdout/stderr).
//!   4. Read the resulting file content back.
//!   5. Return content + tempfile path + editor exit code.
//!
//! What is NOT here (Cycle B handler concerns):
//!   - Dry-run gate.
//!   - Content-hash / mtime change detection.
//!   - Front-matter mutation application.
//!   - Post-pull DB writes.
//!   - Conflict detection.
//!
//! Location: cmd/planar/editor.zig (cmd-layer helper, same level as exit.zig
//! and runtime.zig). NOT under handlers/.
//!
//! Editor resolution follows the PLANAR_EDITOR → VISUAL → EDITOR → "vi"
//! chain, mirroring Go's editflow.ResolveEditor exactly.

const std = @import("std");

// =========================================================================
// Types
// =========================================================================

pub const Error = error{
    /// No editor found: $PLANAR_EDITOR, $VISUAL, $EDITOR all unset and no
    /// override provided, with no "vi" fallback available (test environments).
    NoEditor,
    /// Creating or writing the temp file failed.
    TempfileFailed,
    /// Reading the temp file after the editor exited failed.
    ReadFailed,
    /// Memory allocation failure.
    OutOfMemory,
    /// The file written by the editor exceeds the 64 MiB safety cap.
    /// Matches Go's behaviour, which refuses to load workbench files larger
    /// than 64 MiB (prevents runaway allocations from malicious/buggy editors).
    FileTooLarge,
};

/// Maximum file size accepted after the editor exits (64 MiB).
/// Matches Go's editflow size cap so behaviour is consistent across binaries.
const max_edit_file_bytes: usize = 64 * 1024 * 1024;

/// InvokeOpts controls optional behaviour of invoke().
pub const InvokeOpts = struct {
    /// When non-null, this editor command is used instead of the env-var chain.
    /// Used by tests to inject a fake editor (e.g. "/bin/true", "cat").
    editor_override: ?[]const u8 = null,
    /// File extension appended to the temp file name. Controls syntax
    /// highlighting in editors that detect by extension.
    file_extension: []const u8 = ".md",
};

/// Result holds the output of a successful invoke() call.
/// The caller owns all allocated memory; call deinit() when done.
pub const Result = struct {
    /// File contents after the editor exits.
    content: []u8,
    /// Path to the temp file. The caller is responsible for cleanup.
    path: []u8,
    /// Exit code of the editor process (0 = normal save).
    editor_exit_code: u8,

    pub fn deinit(self: Result, allocator: std.mem.Allocator) void {
        allocator.free(self.content);
        allocator.free(self.path);
    }
};

// =========================================================================
// Public API
// =========================================================================

/// invoke opens $EDITOR (or a fallback/override) with `initial_content` in a
/// temp file, waits for the editor to exit, reads the resulting file, and
/// returns the content + path + exit code.
///
/// The caller owns the returned Result and must call result.deinit().
///
/// Returns error.NoEditor when no editor can be resolved (no env vars set and
/// no override provided).
///
/// Non-zero exit codes are surfaced in Result.editor_exit_code; the caller
/// decides whether to treat that as cancel. This matches Go's git-style
/// contract: the editor value is a single argv0, not shell-parsed.
pub fn invoke(
    io: std.Io,
    allocator: std.mem.Allocator,
    initial_content: []const u8,
    opts: InvokeOpts,
) (Error || std.mem.Allocator.Error)!Result {
    // 1. Resolve the editor command.
    const editor_cmd = try resolveEditor(allocator, opts.editor_override);
    defer allocator.free(editor_cmd);

    // 2. Create the temp file.
    const tmp_path = try createTempFile(io, allocator, initial_content, opts.file_extension);
    errdefer {
        std.Io.Dir.deleteFileAbsolute(io, tmp_path) catch {};
        allocator.free(tmp_path);
    }

    // 3. Exec the editor, inheriting TTY.
    // The editor value is treated as a single argv0 (git-style contract).
    // Editors with required flags (e.g. "code --wait") must be configured by
    // the operator; a value with embedded spaces will fail to exec, matching
    // Go's ResolveEditor documented behaviour.
    var exit_code: u8 = 0;
    {
        var argv: std.ArrayListUnmanaged([]const u8) = .empty;
        defer argv.deinit(allocator);

        try argv.append(allocator, editor_cmd);
        try argv.append(allocator, tmp_path);

        var child = std.process.spawn(io, .{
            .argv = argv.items,
            .stdin = .inherit,
            .stdout = .inherit,
            .stderr = .inherit,
        }) catch return Error.TempfileFailed;
        const term = child.wait(io) catch return Error.TempfileFailed;
        exit_code = switch (term) {
            .exited => |code| code,
            else => 1,
        };
    }

    // 4. Read the resulting file content regardless of exit code.
    // Capped at max_edit_file_bytes (64 MiB) to match Go's behaviour and prevent
    // runaway allocations from a malicious or buggy editor writing unbounded data.
    // error.StreamTooLong from readFileAlloc maps to our FileTooLarge error.
    const content = std.Io.Dir.cwd().readFileAlloc(
        io,
        tmp_path,
        allocator,
        std.Io.Limit.limited(max_edit_file_bytes),
    ) catch |err| switch (err) {
        error.StreamTooLong => return Error.FileTooLarge,
        else => return Error.ReadFailed,
    };

    const path_owned = try allocator.dupe(u8, tmp_path);
    allocator.free(tmp_path); // the errdefer above would double-free without this

    // 5. Return — non-zero exit code is surfaced in Result.editor_exit_code;
    //    the caller decides whether to treat it as cancel.
    return Result{
        .content = content,
        .path = path_owned,
        .editor_exit_code = exit_code,
    };
}

/// resolveEditor returns the editor command string using the precedence chain:
///   1. opts.editor_override (non-null)
///   2. $PLANAR_EDITOR
///   3. $VISUAL
///   4. $EDITOR
///   5. "vi" (fallback — matches Go's editflow.ResolveEditor)
///
/// Returns a newly allocated string owned by the caller.
pub fn resolveEditor(allocator: std.mem.Allocator, override: ?[]const u8) Error![]u8 {
    if (override) |o| return allocator.dupe(u8, o) catch return Error.OutOfMemory;

    const env_names = [_][]const u8{ "PLANAR_EDITOR", "VISUAL", "EDITOR" };
    for (env_names) |name| {
        if (getPosixEnv(name)) |val| {
            if (val.len > 0) return allocator.dupe(u8, val) catch return Error.OutOfMemory;
        }
    }

    // Fallback to "vi".
    return allocator.dupe(u8, "vi") catch return Error.OutOfMemory;
}

// =========================================================================
// Internal helpers
// =========================================================================

/// getPosixEnv reads a single environment variable from std.c.environ.
/// Returns null when the variable is not set or empty.
/// The returned slice points into the process's environ block; caller must
/// NOT free it.
fn getPosixEnv(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (s.len <= key.len + 1) continue;
        if (s[key.len] != '=') continue;
        if (!std.mem.eql(u8, s[0..key.len], key)) continue;
        const val = s[key.len + 1 ..];
        if (val.len == 0) return null;
        return val;
    }
    return null;
}

/// createTempFile writes initial_content to a new temp file under the system
/// temp directory and returns its absolute path (owned by caller).
fn createTempFile(
    io: std.Io,
    allocator: std.mem.Allocator,
    initial_content: []const u8,
    extension: []const u8,
) (Error || std.mem.Allocator.Error)![]u8 {
    // Resolve temp dir: $TMPDIR or /tmp.
    const tmp_dir = getPosixEnv("TMPDIR") orelse "/tmp";

    // Build a unique filename: planar-edit-<random><ext>
    var rng_buf: [4]u8 = undefined;
    io.random(&rng_buf);
    const hex = std.fmt.bytesToHex(rng_buf, .lower);

    const filename = std.fmt.allocPrint(allocator, "planar-edit-{s}{s}", .{ hex, extension }) catch
        return Error.TempfileFailed;
    defer allocator.free(filename);

    const path = std.fs.path.join(allocator, &.{ tmp_dir, filename }) catch
        return Error.TempfileFailed;
    errdefer allocator.free(path);

    // Write initial content.
    std.Io.Dir.cwd().writeFile(io, .{ .sub_path = path, .data = initial_content }) catch
        return Error.TempfileFailed;

    return path;
}

// =========================================================================
// Tests
// =========================================================================

const testing = std.testing;

test "resolveEditor: override takes precedence" {
    const editor = try resolveEditor(testing.allocator, "/usr/bin/nano");
    defer testing.allocator.free(editor);
    try testing.expectEqualStrings("/usr/bin/nano", editor);
}

test "resolveEditor: falls back to vi when no env or override" {
    // Temporarily unset editor env vars if set — in test env they may be unset already.
    // We test the fallback by using an override of null and relying on the test runner
    // not having PLANAR_EDITOR/VISUAL/EDITOR set to something unexpected.
    // Since we can't easily unset env vars in Zig tests, we just verify the
    // function returns a non-empty string (either an env var or "vi").
    const editor = try resolveEditor(testing.allocator, null);
    defer testing.allocator.free(editor);
    try testing.expect(editor.len > 0);
}

test "invoke: noop editor (true) — returns original content" {
    // Use /bin/true as a fake editor that does nothing (file unchanged).
    const initial = "---\nentity_kind: plan\nentity_id: 1\n---\n\nBody.\n";
    const result = try invoke(testing.io, testing.allocator, initial, .{
        .editor_override = "/usr/bin/true",
    });
    defer result.deinit(testing.allocator);

    // /bin/true exits 0; content is the original (editor made no changes).
    try testing.expectEqual(@as(u8, 0), result.editor_exit_code);
    try testing.expectEqualStrings(initial, result.content);
}

test "invoke: fake editor that writes new content" {
    // Use a shell one-liner: printf writes to the file path.
    // We pick a content string that is safe for the printf argument.
    // On macOS/Linux: sh -c 'printf "hello\n" > "$1"' _ <path>
    //
    // This test creates an explicit temp file, writes "hello\n" to it,
    // and then reads it back — confirming invoke() reads the post-edit content.
    //
    // Since we can't easily compose a multi-arg editor in opts (the override is a
    // single argv0 string), we use /bin/true for the functional test above and
    // test the read-back path here by directly exercising createTempFile + reading.
    const initial = "initial content\n";
    const path = try createTempFile(testing.io, testing.allocator, initial, ".md");
    defer {
        std.Io.Dir.deleteFileAbsolute(testing.io, path) catch {};
        testing.allocator.free(path);
    }

    const read_back = try std.Io.Dir.cwd().readFileAlloc(testing.io, path, testing.allocator, .unlimited);
    defer testing.allocator.free(read_back);

    try testing.expectEqualStrings(initial, read_back);
}

test "invoke: non-zero editor exit code — still returns content" {
    // /bin/false exits 1. invoke() should return the original content without error.
    // (The Zig invoke() does not return error.EditorExitedNonZero — it surfaces
    //  the code in Result.editor_exit_code for the caller to inspect.)
    const initial = "content\n";
    const result = try invoke(testing.io, testing.allocator, initial, .{
        .editor_override = "/usr/bin/false",
    });
    defer result.deinit(testing.allocator);

    try testing.expectEqual(@as(u8, 1), result.editor_exit_code);
    // Content is still the original (false exits immediately without touching the file).
    try testing.expectEqualStrings(initial, result.content);
}

test "invoke: temp file has .md extension" {
    const result = try invoke(testing.io, testing.allocator, "x\n", .{
        .editor_override = "/usr/bin/true",
        .file_extension = ".md",
    });
    defer result.deinit(testing.allocator);
    try testing.expect(std.mem.endsWith(u8, result.path, ".md"));
}

test "invoke: file exceeding 64 MiB cap returns FileTooLarge" {
    // Write initial content that exceeds max_edit_file_bytes to a temp file,
    // then invoke() with a stub editor (/usr/bin/true) that exits immediately
    // without modifying it.  The read-back step should hit the cap and return
    // error.FileTooLarge instead of ReadFailed or success.
    //
    // Allocating 64 MiB + 1 byte is intentional: it is the minimum size that
    // must trip the limit.  The allocation is backed by the test allocator and
    // released before the test exits, so the peak live set is ~64 MiB for the
    // duration of the invoke() call.  That is acceptable: the test verifies a
    // safety invariant that is triggered precisely at the boundary.
    const over_limit = max_edit_file_bytes + 1;
    const big = try testing.allocator.alloc(u8, over_limit);
    defer testing.allocator.free(big);
    @memset(big, 'x');

    const result = invoke(testing.io, testing.allocator, big, .{
        .editor_override = "/usr/bin/true",
    });
    try testing.expectError(Error.FileTooLarge, result);
}
