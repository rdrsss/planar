//! Comptime tree validator.
//!
//! `validate(root)` runs at compile time and `@compileError`s on:
//!   - duplicate long-flag names within a command (including inherited)
//!   - duplicate short-flag chars within a command (including inherited)
//!   - duplicate sub-command names within a parent
//!   - Default tag mismatched against the flag's Kind
//!   - flag declared `required = true` with a `default` set (contradictory)
//!
//! Call once near the tree declaration:
//!
//!   const root = cli.Cmd{ ... };
//!   comptime cli.validate(root);
//!
//! Failures surface at the call site (not inside this file) thanks to
//! `@compileError`'s point-of-use semantics.

const std = @import("std");
const cmd_mod = @import("cmd.zig");
const flag = @import("flag.zig");

pub fn validate(comptime root: cmd_mod.Cmd) void {
    comptime {
        validateNode(root, &.{});
    }
}

fn validateNode(comptime node: cmd_mod.Cmd, comptime parent_flags: []const flag.Flag) void {
    // Duplicate sub-command names.
    for (node.cmds, 0..) |a, i| {
        for (node.cmds[i + 1 ..]) |b| {
            if (std.mem.eql(u8, a.name, b.name)) {
                @compileError("cli.validate: duplicate sub-command name '" ++ a.name ++ "' under '" ++ node.name ++ "'");
            }
        }
    }

    // Build the combined flag set this command sees (inherited + own).
    const combined = parent_flags ++ node.flags;

    // Duplicate long names.
    for (combined, 0..) |a, i| {
        for (combined[i + 1 ..]) |b| {
            if (std.mem.eql(u8, a.long, b.long)) {
                @compileError("cli.validate: duplicate flag long name '" ++ a.long ++ "' in '" ++ node.name ++ "' (or inherited)");
            }
        }
    }

    // Duplicate short chars.
    for (combined, 0..) |a, i| {
        if (a.short == null) continue;
        for (combined[i + 1 ..]) |b| {
            if (b.short == null) continue;
            if (a.short.? == b.short.?) {
                @compileError("cli.validate: duplicate flag short char '-" ++ &[_]u8{a.short.?} ++ "' in '" ++ node.name ++ "' (or inherited)");
            }
        }
    }

    // Per-flag invariants.
    for (node.flags) |f| {
        if (f.default) |d| {
            // Default tag must match Kind.
            const default_tag: flag.Kind = d;
            if (default_tag != f.kind) {
                @compileError("cli.validate: flag '" ++ f.long ++ "' has default of kind ." ++ @tagName(default_tag) ++ " but declared kind ." ++ @tagName(f.kind));
            }
            // Required + default is contradictory (default makes it not-required).
            if (f.required) {
                @compileError("cli.validate: flag '" ++ f.long ++ "' is required AND has a default; pick one");
            }
        }
    }

    // Recurse.
    for (node.cmds) |child| {
        validateNode(child, combined);
    }
}

// ---- tests ----

test "validate accepts a well-formed tree" {
    const root = cmd_mod.Cmd{
        .name = "tool",
        .flags = &.{
            .{ .long = "--verbose", .short = 'v', .kind = .bool, .default = .{ .bool = false } },
        },
        .cmds = &.{
            .{
                .name = "add",
                .flags = &.{
                    .{ .long = "--title", .kind = .string, .required = true },
                },
            },
        },
    };
    comptime validate(root);
}

// Negative tests are awkward to write because @compileError aborts the
// build; we rely on the validator firing at the call site for any tree
// that violates invariants. The acceptance test above is a smoke check
// that valid trees compile cleanly.
