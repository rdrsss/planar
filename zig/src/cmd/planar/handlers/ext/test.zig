//! handlers/ext/test — `planar ext test <slug> [--json]`
//!
//! Test connectivity + auth against the named external system. We do not
//! probe the remote here — Go's `ForSystem` is followed by `adapter.Search`,
//! but Search has no Zig equivalent yet (M9-or-later). Building the adapter
//! exercises auth-env lookup + http.Client construction, which is the
//! load-bearing check at this stage of the port.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const adapter_factory = @import("adapter_factory.zig");

const TestJSON = struct {
    slug: []const u8,
    ok: bool,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "ext", "test" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const sys = engine.external.system.showBySlug(d, ctx.allocator, args.slug) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "external system '{s}' not found", .{args.slug}),
        else => exit.die(ctx, e, "ext test: {s}", .{@errorName(e)}),
    };
    defer engine.external.system.deinit(sys, ctx.allocator);

    var h = adapter_factory.build(ctx.allocator, ctx.io, &ctx.environ, sys) catch |e| switch (e) {
        error.TokenEnvVarMissing => exit.die(ctx, error.InvalidInput, "token env var '{s}' is not set", .{sys.auth_ref}),
        error.UnsupportedAuthMethod => exit.die(ctx, error.InvalidInput, "oauth-stored auth not yet supported (matches Go: deferred)", .{}),
        error.GhCliNotFound => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh` binary not on PATH", .{}),
        error.GhCliFailed => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh auth token` returned non-zero (run `gh auth login`)", .{}),
        error.GhCliEmptyToken => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh auth token` returned empty output", .{}),
        error.UnsupportedSystemKind => exit.die(ctx, error.InvalidInput, "system kind '{s}' is not supported in the zig port", .{sys.kind.toText()}),
        else => exit.die(ctx, e, "ext test: building adapter: {s}", .{@errorName(e)}),
    };
    defer {
        h.deinit();
        ctx.allocator.destroy(h);
    }

    const ok = probeOk(h);
    if (args.json) {
        const out = TestJSON{ .slug = args.slug, .ok = ok };
        try std.json.Stringify.value(out, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else if (ok) {
        try ctx.stdout.print("{s}: ok  (adapter wired)\n", .{args.slug});
    } else {
        try ctx.stdout.print("{s}: FAIL  (adapter not configured)\n", .{args.slug});
    }
}

fn probeOk(h: *adapter_factory.Handle) bool {
    return switch (h.kind) {
        .jira => h.jira_adapter != null,
        .github => h.github_adapter != null,
    };
}
