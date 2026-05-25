//! handlers/config/show.zig — `planar config show`
//!
//! Print the resolved configuration.
//!
//! Flags (from cmd.zig scaffolding):
//!   --effective   Show each key with its provenance.
//!   --raw         Print the user's file verbatim (empty if absent).
//!   --defaults    Print the embedded defaults.toml verbatim.
//!   --scope       Resolve as if the named association slug were active.
//!   --json        JSON output (one object per line in effective mode, else one object).
//!
//! D-sensitive-masking: without --raw, tokens/passwords/secrets are masked as ***.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const config_path = @import("path.zig");
const engine = @import("engine");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "config", "show" }, args_ptr);
    const ctx = runtime.current();

    // --defaults: print embedded defaults.toml verbatim.
    if (args.defaults) {
        try ctx.stdout.print("{s}", .{engine.config.defaults_toml});
        return;
    }

    const scope_opt: ?[]const u8 = if (args.scope) |s| (if (s.len > 0) s else null) else null;
    const json_mode = args.json;

    // --raw: print user's config file verbatim.
    if (args.raw) {
        const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
            exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
        defer ctx.allocator.free(path);

        const content = std.Io.Dir.cwd().readFileAlloc(
            ctx.io,
            path,
            ctx.allocator,
            .unlimited,
        ) catch |e| switch (e) {
            error.FileNotFound => {
                // Empty output when file is absent.
                return;
            },
            else => exit.die(ctx, e, "reading config file: {s}", .{@errorName(e)}),
        };
        defer ctx.allocator.free(content);
        try ctx.stdout.print("{s}", .{content});
        return;
    }

    // Load user config file (if present).
    const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    const file_content: ?[]u8 = std.Io.Dir.cwd().readFileAlloc(
        ctx.io,
        path,
        ctx.allocator,
        .unlimited,
    ) catch |e| switch (e) {
        error.FileNotFound => null,
        else => exit.die(ctx, e, "reading config file: {s}", .{@errorName(e)}),
    };
    defer if (file_content) |fc| ctx.allocator.free(fc);

    // Resolve effective configuration.
    var resolved = engine.config.resolve(
        ctx.allocator,
        file_content,
        ctx.environ,
        scope_opt,
    ) catch |e| exit.die(ctx, e, "resolving configuration: {s}", .{@errorName(e)});
    defer resolved.deinit(ctx.allocator);

    if (args.effective or json_mode) {
        try showEffective(ctx, &resolved.effective, json_mode);
        return;
    }

    // Default: human-readable key=value.
    try showHuman(ctx, &resolved.effective);
}

/// Print each key with provenance. Mirrors Go's showEffective.
fn showEffective(
    ctx: *const runtime.Ctx,
    eff: *const engine.config.EffectiveMap,
    json_mode: bool,
) !void {
    const keys = engine.config.sortedKeys(eff, ctx.allocator) catch |e|
        exit.die(ctx, e, "sorting keys: {s}", .{@errorName(e)});
    defer ctx.allocator.free(keys);

    var prov_buf: [256]u8 = undefined;

    for (keys) |k| {
        const entry = eff.get(k) orelse continue;
        const display_val = maskedValue(k, entry);

        // Build provenance label — for env we prepend "env: <VAR_NAME>".
        const prov_str: []const u8 = switch (entry.source) {
            .env => std.fmt.bufPrint(&prov_buf, "env: {s}", .{entry.env_var_name}) catch entry.env_var_name,
            .assoc_override => "per-association override",
            .config_file => "config file",
            .embedded_default => "embedded default",
        };

        if (json_mode) {
            // One JSON object per line: {"key":"...","value":"...","provenance":"..."}
            try ctx.stdout.print(
                "{{\"key\":\"{s}\",\"value\":\"{s}\",\"provenance\":\"{s}\"}}\n",
                .{ k, display_val, prov_str },
            );
        } else {
            // Human: "key = value  [provenance]"
            try ctx.stdout.print("{s} = {s}  [{s}]\n", .{ k, display_val, prov_str });
        }
    }
}

/// Print compact key=value without provenance. Mirrors Go's showHuman.
fn showHuman(ctx: *const runtime.Ctx, eff: *const engine.config.EffectiveMap) !void {
    const keys = engine.config.sortedKeys(eff, ctx.allocator) catch |e|
        exit.die(ctx, e, "sorting keys: {s}", .{@errorName(e)});
    defer ctx.allocator.free(keys);

    for (keys) |k| {
        const entry = eff.get(k) orelse continue;
        const display_val = maskedValue(k, entry);
        try ctx.stdout.print("{s} = {s}\n", .{ k, display_val });
    }
}

/// Return the display value, masking sensitive fields with ***.
fn maskedValue(key: []const u8, entry: engine.config.ValueWithSource) []const u8 {
    if (engine.config.sensitiveName(key)) return "***";
    if (entry.env_var_name.len > 0 and engine.config.sensitiveName(entry.env_var_name)) return "***";
    return entry.value;
}

