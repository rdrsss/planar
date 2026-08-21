//! handlers/workflow/run.zig — `planar workflow run <name> --phase <phase> [--args <json>]
//!   [--worktree <dir>] [--sandbox-root <dir>] [--local]`
//!
//! Resolves a workflow by name (shipped → sandbox, or sandbox-only when
//! --local is set), then execs `planar-execute run <resolved-path>
//! --phase <phase> [--args <json>] [--worktree <dir>] [--sandbox-root <dir>]`
//! with stdout/stderr inherited so the workflow's flow.result JSON streams
//! directly to the caller's terminal.  The exit code of planar-execute is
//! propagated exactly — a flow.fail / non-zero workflow surfaces as non-zero
//! here.
//!
//! READ-ONLY w.r.t. the DB — resolution is filesystem only; SQLite is never
//! opened in this handler.
//!
//! planar-execute binary resolution order (first found wins):
//!   1. `$PLANAR_EXECUTE_BIN` (integration-harness / script override).
//!   2. Sibling of argv[0] (`<dir>/planar-execute`; the installed-case
//!      `~/.planar/bin/planar-execute`).
//!   3. `planar-execute` on `$PATH`.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const dirs = @import("dirs.zig");
const scan = @import("scan.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workflow", "run" }, args_ptr);
    const ctx = runtime.current();
    const allocator = ctx.allocator;

    const name = args.name;

    // --- Resolve workflow directories. ---
    const d = dirs.resolve(ctx) catch |e|
        exit.die(ctx, e, "resolving workflow dirs: {s}", .{@errorName(e)});
    defer d.deinit(allocator);

    // --- Search shipped (unless --local) then sandbox. ---
    // Mirrors show.zig: shipped wins on name collision.
    const resolved_path: []const u8 = blk: {
        if (!args.local) {
            const entries = scan.scan(d.shipped, false, allocator, ctx.io) catch |e|
                exit.die(ctx, e, "scanning shipped workflows: {s}", .{@errorName(e)});
            defer scan.deinitEntries(entries, allocator);

            for (entries) |e| {
                if (std.mem.eql(u8, e.effectiveName(), name)) {
                    break :blk allocator.dupe(u8, e.path) catch |e2|
                        exit.die(ctx, e2, "OOM duping workflow path: {s}", .{@errorName(e2)});
                }
            }
        }

        // Search sandbox.
        const sandbox_entries = scan.scan(d.sandbox, true, allocator, ctx.io) catch |e|
            exit.die(ctx, e, "scanning sandbox workflows: {s}", .{@errorName(e)});
        defer scan.deinitEntries(sandbox_entries, allocator);

        for (sandbox_entries) |e| {
            if (std.mem.eql(u8, e.effectiveName(), name)) {
                break :blk allocator.dupe(u8, e.path) catch |e2|
                    exit.die(ctx, e2, "OOM duping workflow path: {s}", .{@errorName(e2)});
            }
        }

        // Not found in either location.
        exit.die(ctx, error.NotFound, "workflow '{s}' not found", .{name});
    };
    defer allocator.free(resolved_path);

    // --- Locate the planar-execute binary. ---
    const execute_bin = resolveExecuteBin(ctx, allocator) catch |e|
        exit.die(ctx, e, "locating planar-execute: {s}", .{@errorName(e)});
    defer allocator.free(execute_bin);

    // --- Build the argv for planar-execute. ---
    // planar-execute run <path> --phase <phase> [--args <json>] [--worktree <dir>] [--sandbox-root <dir>]
    var argv: std.ArrayListUnmanaged([]const u8) = .empty;
    defer argv.deinit(allocator);

    try argv.append(allocator, execute_bin);
    try argv.append(allocator, "run");
    try argv.append(allocator, resolved_path);
    try argv.append(allocator, "--phase");
    try argv.append(allocator, args.phase);

    if (args.args.len > 0) {
        try argv.append(allocator, "--args");
        try argv.append(allocator, args.args);
    }
    if (args.worktree.len > 0) {
        try argv.append(allocator, "--worktree");
        try argv.append(allocator, args.worktree);
    }
    if (args.sandbox_root.len > 0) {
        try argv.append(allocator, "--sandbox-root");
        try argv.append(allocator, args.sandbox_root);
    }

    // --- Spawn planar-execute with inherited stdio (streams straight through). ---
    // Flush the runtime's buffered stdout/stderr before yielding control so
    // nothing is interleaved with the child's output.
    ctx.stdout.flush() catch {};
    ctx.stderr.flush() catch {};

    var child = std.process.spawn(ctx.io, .{
        .argv = argv.items,
        .stdin = .inherit,
        .stdout = .inherit,
        .stderr = .inherit,
    }) catch |e|
        exit.die(ctx, e, "spawning planar-execute: {s}", .{@errorName(e)});

    const term = child.wait(ctx.io) catch |e|
        exit.die(ctx, e, "waiting for planar-execute: {s}", .{@errorName(e)});

    // --- Propagate the exit code exactly. ---
    const code: u8 = switch (term) {
        .exited => |c| c,
        else => 1,
    };

    if (code != 0) {
        runtime.shutdown();
        std.process.exit(code);
    }
}

/// resolveExecuteBin returns the absolute path to the `planar-execute`
/// binary, probing in priority order:
///   1. `$PLANAR_EXECUTE_BIN` env var.
///   2. Sibling of `argv[0]` (`<dir>/planar-execute`).
///   3. `planar-execute` on `$PATH` (fallback; returned as a bare name for
///      the OS to resolve).
///
/// Returns an allocator-owned string.  Errors when none of the candidates
/// are found and accessible.
fn resolveExecuteBin(ctx: *const runtime.Ctx, allocator: std.mem.Allocator) ![]const u8 {
    const environ = ctx.environ;

    // 1. $PLANAR_EXECUTE_BIN override.
    if (environ.getPosix("PLANAR_EXECUTE_BIN")) |v| {
        if (v.len > 0) return allocator.dupe(u8, v);
    }

    // 2. Sibling of argv[0].
    if (ctx.argv.len > 0) {
        const argv0 = ctx.argv[0];
        if (std.fs.path.dirname(argv0)) |dir| {
            const candidate = try std.fs.path.join(allocator, &.{ dir, "planar-execute" });
            errdefer allocator.free(candidate);
            // Check if the file exists and is accessible.
            std.Io.Dir.cwd().access(ctx.io, candidate, .{}) catch {
                allocator.free(candidate);
                // Fall through to PATH probe.
                return allocator.dupe(u8, "planar-execute");
            };
            return candidate;
        }
    }

    // 3. Let the OS resolve via $PATH (bare name).
    return allocator.dupe(u8, "planar-execute");
}
