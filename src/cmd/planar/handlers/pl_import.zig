//! handlers/pl_import.zig — `planar pl-import`
//! Import an existing repo's planning content into Planar.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("../runtime.zig");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "pl-import",
    .desc = "Import an existing repo's state into Planar.",
    .long_desc = "pl-import translates the planning artefacts of an existing\n  repository into Planar's data model. It discovers\n  tech specs, roadmap milestones, ADRs, and backlog files,\n  infers completion status from checkbox state and git history,\n  and produces an ImportPlan for review before committing.",
    .flags = &.{
        .{ .long = "--from-github", .kind = .bool, .default = .{ .bool = false }, .desc = "Pull source from GitHub issues" },
        .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--strict", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--threshold", .kind = .string, .desc = "Similarity threshold (float as string for now)" },
        .{ .long = "--roadmap", .kind = .string, .desc = "Path to a roadmap source" },
        .{ .long = "--apply", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--apply-removals", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--no-status-inference", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--interpret", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--no-interpret", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--scope", .kind = .string },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "repo-root", .kind = .string, .required = true },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"pl-import"}, args_ptr);
    const ctx = runtime.current();
    const d = if (args.apply) runtime.ensureDb() catch |e|
        exit.die(ctx, e, "opening database: {s}", .{@errorName(e)}) else null;

    const planar_home = engine.identity.workspace.planarHome(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving PLANAR_HOME: {s}", .{@errorName(e)});
    defer ctx.allocator.free(planar_home);

    const out = engine.import.run(d, ctx.allocator, ctx.environ, planar_home, .{
        .repo_root = args.repo_root,
        .interpret = args.interpret,
        .no_interpret = args.no_interpret,
        .apply = args.apply,
        .apply_removals = args.apply_removals,
        .scope = args.scope,
    }) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "repo-root not found or not a directory: {s}", .{args.repo_root}),
        error.QueryFailed => exit.die(ctx, e, "database apply failed", .{}),
        error.InvalidInput => exit.die(ctx, e, "invalid pl-import arguments", .{}),
        else => exit.die(ctx, e, "pl-import failed: {s}", .{@errorName(e)}),
    };
    defer engine.import.deinitOutcome(out, ctx.allocator);

    if (args.json) {
        const OutJSON = struct {
            mode: []const u8,
            provider: []const u8,
            repo_slug: []const u8,
            fingerprint: []const u8,
            cache_path: ?[]const u8,
            pending_path: ?[]const u8,
            docs_count: usize,
            guide_files_count: usize,
            tree_entry_count: usize,
            message: []const u8,
            applied: ?engine.import.ApplyReport = null,
        };
        const mode_text = switch (out.mode) {
            .skipped => "skipped",
            .pending => "pending",
            .cache_hit => "cache_hit",
        };
        const payload: OutJSON = .{
            .mode = mode_text,
            .provider = out.provider.text(),
            .repo_slug = out.repo_slug,
            .fingerprint = out.fingerprint,
            .cache_path = out.cache_path,
            .pending_path = out.pending_path,
            .docs_count = out.docs_count,
            .guide_files_count = out.guide_files_count,
            .tree_entry_count = out.tree_entry_count,
            .message = out.message,
            .applied = out.applied,
        };
        try std.json.Stringify.value(payload, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }

    try ctx.stdout.print("{s}\n", .{out.message});
}
