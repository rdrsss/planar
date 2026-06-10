//! handlers/config/init.zig — `planar config init`
//!
//! Write a starter config.toml if it does not already exist.
//! Mirrors Go's config.Init() in src/internal/config/init.go:
//!
//!   - If the file exists: print "config init: already exists <path>" and exit 0.
//!   - If absent: create parent dirs, write starter content, exit 0.
//!
//! D-init-idempotent: file exists → "already exists" message, exit 0.
//!   (Go returns (false, nil) — no error — when file exists; Zig mirrors that.)

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const config_path = @import("path.zig");
const engine = @import("engine");

/// The starter config content written by `config init`.
/// Mirrors Go's starterConfig const in src/internal/config/init.go.
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
    \\# # Per-vendor model tier maps + role→tier routing (plan 540). Override a
    \\# # tier to re-route every role at that tier; see `planar models`.
    \\# [models.codex]
    \\# medium = "gpt-5.4"
    \\#
    \\# [roles]
    \\# coder = "large"
    \\#
    \\# [associations."org:acme"]
    \\# github_lead_repo = "acme/platform"
    \\#
    \\# [associations."org:acme".external.jira.status]
    \\# done = "Closed"
    \\
;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "config", "init" }, args_ptr);
    const ctx = runtime.current();

    const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    const created = initConfigFile(ctx, path) catch |e|
        exit.die(ctx, e, "initializing config file: {s}", .{@errorName(e)});

    if (created) {
        try ctx.stdout.print("config init: created {s}\n", .{path});
    } else {
        try ctx.stdout.print("config init: already exists {s}\n", .{path});
    }
}

/// Write the starter config at `path` if it does not exist.
/// Returns true if created, false if already existed.
fn initConfigFile(ctx: *const runtime.Ctx, path: []const u8) !bool {
    // Check if file exists.
    std.Io.Dir.cwd().access(ctx.io, path, .{}) catch |e| {
        if (e == error.FileNotFound) {
            // File does not exist — create it.
            try createConfigFile(ctx, path);
            return true;
        }
        return e;
    };
    // File exists.
    return false;
}

fn createConfigFile(ctx: *const runtime.Ctx, path: []const u8) !void {
    // Ensure parent directory exists (recursive mkdir -p, works with relative
    // and absolute paths). createDirPath ignores PathAlreadyExists.
    if (std.fs.path.dirname(path)) |parent| {
        std.Io.Dir.cwd().createDirPath(ctx.io, parent) catch |e| switch (e) {
            error.PathAlreadyExists => {},
            else => return e,
        };
    }
    // Write the starter content.
    std.Io.Dir.cwd().writeFile(ctx.io, .{
        .sub_path = path,
        .data = starter_config,
    }) catch return error.FileWriteFailed;
}
