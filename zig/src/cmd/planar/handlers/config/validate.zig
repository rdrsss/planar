//! handlers/config/validate.zig — `planar config validate [<path>]`
//!
//! Parse the config file (or <path> if supplied), check syntax, key schema,
//! and the sensitive-data invariant. Exit 0 on clean; non-zero on any issue.
//!
//! Mirrors Go's runConfigValidate in src/cmd/planar/internal/system/config.go
//! and the rule set in src/internal/config/validate.go.
//!
//! Error format: "<severity>: line <N>: <key>: <message>" or
//!               "<severity>: <key>: <message>" (no line when unavailable).
//!
//! D-validate-shape:
//!   - exit 0 if valid
//!   - exit non-zero with stderr error citing line+column for parse errors

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const config_path = @import("path.zig");
const engine = @import("engine");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = cli.castArgs(main.root, &.{ "config", "validate" }, args_ptr);
    const ctx = runtime.current();

    // Resolve the path to validate.
    const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    // Read the file.
    const content = readFile(ctx, path) orelse return;
    defer ctx.allocator.free(content);

    // Step 1: TOML parse.
    var pe: engine.config.parse.ParseError = undefined;
    var map = engine.config.parse.parse(ctx.allocator, content, &pe) catch |e| switch (e) {
        error.ParseFailed => {
            ctx.stderr.print("error: line {d}: col {d}: TOML parse error: {s}\n", .{
                pe.line, pe.column, pe.message,
            }) catch {};
            runtime.shutdown();
            std.process.exit(1);
        },
        error.UnsupportedFeature => {
            ctx.stderr.print("error: line {d}: col {d}: unsupported TOML feature: {s}\n", .{
                pe.line, pe.column, pe.message,
            }) catch {};
            runtime.shutdown();
            std.process.exit(1);
        },
        error.OutOfMemory => exit.die(ctx, error.OutOfMemory, "out of memory", .{}),
    };
    defer engine.config.parse.deinitMap(&map, ctx.allocator);

    // Step 2: sensitive-data check (mirrors Go's checkSensitiveValues).
    // Iterate the file line-by-line; for each `key = "value"` form, flag any
    // line whose key matches a sensitive-name rule and carries a non-empty
    // literal. Comments and blank lines are skipped; table-header lines have
    // no `=` and are skipped naturally.
    var has_errors = false;
    var line_iter = std.mem.splitScalar(u8, content, '\n');
    var line_idx: usize = 0;
    while (line_iter.next()) |line| : (line_idx += 1) {
        const trimmed = std.mem.trim(u8, line, " \t\r");
        if (trimmed.len == 0 or trimmed[0] == '#') continue;
        const eq_idx = std.mem.indexOfScalar(u8, trimmed, '=') orelse continue;
        const raw_key = std.mem.trim(u8, trimmed[0..eq_idx], " \t");
        const raw_val_with_comment = std.mem.trim(u8, trimmed[eq_idx + 1 ..], " \t");
        const raw_val = stripInlineComment(raw_val_with_comment);
        const val = std.mem.trim(u8, std.mem.trim(u8, raw_val, "\"'"), " \t");
        if (val.len == 0) continue;
        if (engine.config.sensitiveName(raw_key)) {
            ctx.stderr.print(
                "error: line {d}: {s}: sensitive key must not carry a literal value in the config file (use *_env convention instead)\n",
                .{ line_idx + 1, raw_key },
            ) catch {};
            has_errors = true;
        }
    }

    // Step 3: semantic checks (mirrors Go's Validate()).
    // external.github-issues.auth cross-reference.
    const gh_auth = strVal(map.get("external.github-issues.auth"));
    const gh_token_env = strVal(map.get("external.github-issues.token_env"));

    if (std.mem.eql(u8, gh_auth, "token-env") and gh_token_env.len == 0) {
        ctx.stderr.print(
            "error: external.github-issues.token_env: auth = \"token-env\" requires token_env to name an env var\n",
            .{},
        ) catch {};
        has_errors = true;
    }

    if (gh_auth.len > 0) {
        const valid_auth = [_][]const u8{ "gh-cli", "token-env", "oauth-stored" };
        var found = false;
        for (valid_auth) |va| {
            if (std.mem.eql(u8, gh_auth, va)) {
                found = true;
                break;
            }
        }
        if (!found) {
            ctx.stderr.print(
                "error: external.github-issues.auth: unrecognised auth value \"{s}\" (expected: gh-cli, token-env, oauth-stored)\n",
                .{gh_auth},
            ) catch {};
            has_errors = true;
        }
    }

    // Step 4: work-type routing validation (plan 899 D10, task
    // worktype-routing-map). A `[routing.<vendor>.<tier>]` entry must name a
    // candidate model id present in that tier's resolved candidate list —
    // file-over-default, same as every other model-tier key. Resolve the
    // effective config (defaults + this file, no env/assoc override needed
    // for these keys) to get the merged candidate lists + routing entries,
    // then cross-check every present routing key against its tier's list.
    {
        var eff_res = engine.config.effective.resolve(ctx.allocator, content, ctx.environ, null) catch |e| switch (e) {
            error.OutOfMemory => exit.die(ctx, error.OutOfMemory, "out of memory", .{}),
            // Step 1 already validated TOML syntax against the same content;
            // a second ParseFailed here would indicate a resolver bug, not a
            // user-facing config error.
            error.ParseFailed => unreachable,
        };
        defer eff_res.deinit(ctx.allocator);

        for (engine.config.effective.vendors) |v| {
            for (engine.config.effective.tiers) |t| {
                var mbuf: [160]u8 = undefined;
                const model_key = std.fmt.bufPrint(&mbuf, "models.{s}.{s}", .{ v, t }) catch continue;
                const model_entry = eff_res.effective.get(model_key) orelse continue;
                const candidates: []const []const u8 = if (model_entry.candidates.len > 0)
                    model_entry.candidates
                else
                    &[_][]const u8{model_entry.value};

                for (engine.config.effective.work_types) |wt| {
                    var rbuf: [220]u8 = undefined;
                    const routing_key = std.fmt.bufPrint(&rbuf, "routing.{s}.{s}.{s}", .{ v, t, wt }) catch continue;
                    const routing_entry = eff_res.effective.get(routing_key) orelse continue;
                    if (routing_entry.value.len == 0) continue;

                    var found = false;
                    for (candidates) |c| {
                        if (std.mem.eql(u8, c, routing_entry.value)) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        ctx.stderr.print(
                            "error: {s}: routed model id \"{s}\" is not in the {s} candidate list\n",
                            .{ routing_key, routing_entry.value, model_key },
                        ) catch {};
                        has_errors = true;
                    }
                }
            }
        }
    }

    if (has_errors) {
        runtime.shutdown();
        std.process.exit(1);
    }

    try ctx.stdout.print("config validate: ok\n", .{});
}

fn readFile(ctx: *const runtime.Ctx, path: []const u8) ?[]u8 {
    const content = std.Io.Dir.cwd().readFileAlloc(
        ctx.io,
        path,
        ctx.allocator,
        .unlimited,
    ) catch |e| switch (e) {
        error.FileNotFound => {
            ctx.stderr.print("error: config file not found: {s}\n", .{path}) catch {};
            runtime.shutdown();
            std.process.exit(1);
        },
        else => exit.die(ctx, e, "reading config file: {s}", .{@errorName(e)}),
    };
    return content;
}

fn strVal(v: ?engine.config.parse.Value) []const u8 {
    const val = v orelse return "";
    return switch (val) {
        .string => |s| s,
        else => "",
    };
}

// Inline split helper used in validate — iterate lines without allocation.
fn stripInlineComment(s: []const u8) []const u8 {
    var in_quote = false;
    var quote_char: u8 = 0;
    var i: usize = 0;
    while (i < s.len) : (i += 1) {
        const c = s[i];
        if (in_quote) {
            if (c == quote_char) in_quote = false;
            continue;
        }
        if (c == '"' or c == '\'') {
            in_quote = true;
            quote_char = c;
            continue;
        }
        if (c == '#') return std.mem.trim(u8, s[0..i], " \t");
    }
    return s;
}
