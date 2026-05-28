//! handlers/synthesize.zig — `planar synthesize`
//! Synthesize fresh planning artifacts from a repo's docs + code + git history.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "synthesize",
    .desc = "Synthesize fresh planning artifacts from a repo's docs + code + git history.",
    .long_desc = "synthesize reads a repository's existing planning docs, source\n  code, and git history AS INPUT for an LLM synthesis pass. It\n  produces fresh product-spec / tech-spec / roadmap artifacts (NOT a\n  verbatim transcription) and proposes them via the same workbench\n  pipeline as the planner agent.",
    .flags = &.{
        .{ .long = "--apply", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--apply-removals", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--scope", .kind = .string },
        .{ .long = "--code-layout", .kind = .string },
        .{ .long = "--treat-as-greenfield", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--treat-as-nongreenfield", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--threshold", .kind = .string, .desc = "Similarity threshold (float as string for now)" },
        .{ .long = "--literal", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false } },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "repo-root", .kind = .string, .required = true },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"synthesize"}, args_ptr);
    const ctx = runtime.current();
    const d = if (args.apply) runtime.ensureDb() catch |e|
        exit.die(ctx, e, "opening database: {s}", .{@errorName(e)}) else null;

    const planar_home = engine.identity.workspace.planarHome(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving PLANAR_HOME: {s}", .{@errorName(e)});
    defer ctx.allocator.free(planar_home);

    if (args.literal) {
        try ctx.stderr.print("synthesize: --literal mode; delegating to import.\n", .{});
        const delegated = engine.import.run(d, ctx.allocator, ctx.environ, planar_home, .{
            .repo_root = args.repo_root,
            .apply = args.apply,
            .apply_removals = args.apply_removals,
            .scope = args.scope,
        }) catch |e| switch (e) {
            error.NotFound => exit.die(ctx, e, "repo-root not found or not a directory: {s}", .{args.repo_root}),
            error.InvalidInput => exit.die(ctx, e, "invalid synthesize --literal arguments", .{}),
            else => exit.die(ctx, e, "literal delegation failed: {s}", .{@errorName(e)}),
        };
        defer engine.import.deinitOutcome(delegated, ctx.allocator);
        if (args.json) {
            const OutJSON = struct {
                mode: []const u8,
                provider: []const u8,
                repo_slug: []const u8,
                fingerprint: []const u8,
                cache_path: ?[]const u8,
                pending_path: ?[]const u8,
                message: []const u8,
                applied: ?engine.import.ApplyReport = null,
            };
            const mode_text = switch (delegated.mode) {
                .skipped => "skipped",
                .pending => "pending",
                .cache_hit => "cache_hit",
            };
            const payload: OutJSON = .{
                .mode = mode_text,
                .provider = delegated.provider.text(),
                .repo_slug = delegated.repo_slug,
                .fingerprint = delegated.fingerprint,
                .cache_path = delegated.cache_path,
                .pending_path = delegated.pending_path,
                .message = delegated.message,
                .applied = delegated.applied,
            };
            try std.json.Stringify.value(payload, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
            return;
        }
        try ctx.stdout.print("{s}\n", .{delegated.message});
        return;
    }

    const out = engine.synthesize.run(d, ctx.allocator, ctx.environ, planar_home, .{
        .repo_root = args.repo_root,
        .apply = args.apply,
        .apply_removals = args.apply_removals,
        .code_layout = args.code_layout,
        .treat_as_greenfield = args.treat_as_greenfield,
        .treat_as_nongreenfield = args.treat_as_nongreenfield,
        .scope = args.scope,
    }) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "repo-root not found or not a directory: {s}", .{args.repo_root}),
        error.QueryFailed => exit.die(ctx, e, "database apply failed", .{}),
        error.InvalidInput => exit.die(ctx, e, "invalid synthesize arguments", .{}),
        else => exit.die(ctx, e, "synthesize failed: {s}", .{@errorName(e)}),
    };
    defer engine.synthesize.deinitOutcome(out, ctx.allocator);

    if (args.json) {
        const OutJSON = struct {
            mode: []const u8,
            provider: []const u8,
            repo_slug: []const u8,
            fingerprint: []const u8,
            cache_path: []const u8,
            pending_path: []const u8,
            docs_count: usize,
            guide_files_count: usize,
            tree_entry_count: usize,
            greenfield: bool,
            message: []const u8,
            applied: ?engine.synthesize.ApplyReport = null,
        };
        const payload: OutJSON = .{
            .mode = switch (out.mode) {
                .pending => "pending",
                .cache_hit => "cache_hit",
            },
            .provider = out.provider.text(),
            .repo_slug = out.repo_slug,
            .fingerprint = out.fingerprint,
            .cache_path = out.cache_path,
            .pending_path = out.pending_path,
            .docs_count = out.docs_count,
            .guide_files_count = out.guide_files_count,
            .tree_entry_count = out.tree_entry_count,
            .greenfield = out.greenfield,
            .message = out.message,
            .applied = out.applied,
        };
        try std.json.Stringify.value(payload, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }

    try ctx.stdout.print("{s}\n", .{out.message});
}
