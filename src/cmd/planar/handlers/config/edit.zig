//! handlers/config/edit.zig — `planar config edit`
//!
//! Open the config file in $EDITOR. If the file doesn't exist, write the
//! starter content first (like `config init`).
//!
//! Mirrors Go's runConfigEdit in src/cmd/planar/internal/system/config.go.
//! Uses editor.zig from M4 for $EDITOR invocation (do NOT re-implement).
//!
//! D-no-audit: config edit is filesystem-only, no DB writes.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const config_path = @import("path.zig");
const editor_mod = @import("../../editor.zig");

/// The starter config content written when the file is absent before editing.
/// Mirrors init.zig's starter_config exactly.
const starter_config =
    \\# ~/.planar/config.toml — Planar configuration
    \\#
    \\# This file was created by "planar config init" (or "planar init").
    \\# All keys are optional. Anything you don't set falls through to the
    \\# embedded defaults shipped in the binary.
    \\#
    \\# To see every available key with its current default value, run:
    \\#   planar config show --defaults
    \\#
    \\# To see the fully resolved configuration with provenance per key, run:
    \\#   planar config show --effective
    \\#
    \\# Environment variables override config values. They always take precedence.
    \\# See docs/architecture.md for the full configuration plane reference.
    \\#
    \\# To edit this file in your $EDITOR, run:
    \\#   planar config edit
    \\#
    \\# Example customizations (uncomment and fill in):
    \\#
    \\# [defaults]
    \\# vendor = "claude"
    \\#
    \\# [workbench]
    \\# root = "~/.planar/workbench"
    \\#
    \\# [external.jira]
    \\# base_url = "https://your-org.atlassian.net"
    \\# user_env  = "JIRA_USER"
    \\# token_env = "JIRA_TOKEN"
    \\#
    \\# [associations."org:acme"]
    \\# github_lead_repo = "acme/platform"
    \\#
    \\# [associations."org:acme".external.jira.status]
    \\# done = "Closed"
    \\
;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "config", "edit" }, args_ptr);
    const ctx = runtime.current();

    // Resolve config path.
    const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    // Ensure the file exists (create parent dirs + write starter if absent).
    // Mirrors Go's config.Init(path) call in runConfigEdit.
    ensureExists(ctx, path) catch |e|
        exit.die(ctx, e, "initializing config file: {s}", .{@errorName(e)});

    // Invoke the editor with the file.
    // editor.zig resolves: PLANAR_EDITOR → VISUAL → EDITOR → vi.
    // We invoke directly with the file path instead of through a temp file,
    // since this is editing the actual config file (not a workbench draft).
    // Per editor.zig's contract: pass the path as the editor argument.
    const editor_cmd = editor_mod.resolveEditor(ctx.allocator, null) catch |e|
        exit.die(ctx, e, "resolving editor: {s}", .{@errorName(e)});
    defer ctx.allocator.free(editor_cmd);

    // Spawn the editor directly on the config file (not a temp copy).
    var argv_list: std.ArrayListUnmanaged([]const u8) = .empty;
    defer argv_list.deinit(ctx.allocator);
    argv_list.append(ctx.allocator, editor_cmd) catch |e|
        exit.die(ctx, e, "OOM building editor argv: {s}", .{@errorName(e)});
    argv_list.append(ctx.allocator, path) catch |e|
        exit.die(ctx, e, "OOM building editor argv: {s}", .{@errorName(e)});

    var child = std.process.spawn(ctx.io, .{
        .argv = argv_list.items,
        .stdin = .inherit,
        .stdout = .inherit,
        .stderr = .inherit,
    }) catch |e| exit.die(ctx, e, "spawning editor: {s}", .{@errorName(e)});

    const term = child.wait(ctx.io) catch |e|
        exit.die(ctx, e, "waiting for editor: {s}", .{@errorName(e)});

    const ec: u8 = switch (term) {
        .exited => |code| code,
        else => 1,
    };
    if (ec != 0) {
        ctx.stderr.print("error: editor exited with code {d}\n", .{ec}) catch {};
        runtime.shutdown();
        std.process.exit(1);
    }
}

fn ensureExists(ctx: *const runtime.Ctx, path: []const u8) !void {
    // Check if file already exists.
    std.Io.Dir.cwd().access(ctx.io, path, .{}) catch |e| {
        if (e == error.FileNotFound) {
            // Ensure parent directory exists (recursive mkdir -p, relative or absolute).
            if (std.fs.path.dirname(path)) |parent| {
                std.Io.Dir.cwd().createDirPath(ctx.io, parent) catch |ce| switch (ce) {
                    error.PathAlreadyExists => {},
                    else => return ce,
                };
            }
            // Write starter content.
            std.Io.Dir.cwd().writeFile(ctx.io, .{
                .sub_path = path,
                .data = starter_config,
            }) catch return error.FileWriteFailed;
            return;
        }
        return e;
    };
    // File exists — nothing to do.
}
