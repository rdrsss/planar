//! handlers/completion.zig — `planar-watch completion <shell>`
//!
//! Mirrors the operator binary's completion verb. The script is built
//! at comptime from this binary's root command tree, so we get
//! per-binary completions without sharing state between the three
//! command surfaces.

const std = @import("std");
const cli = @import("cli");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "completion",
    .desc = "Generate the autocompletion script for the specified shell.",
    .positionals = &.{
        .{ .name = "shell", .kind = .string, .required = true, .desc = "Shell: bash, zsh, or fish" },
    },
    .run = cli.handler(handle),
};

const bash_script = cli.completion.script(main.root, .bash);
const zsh_script = cli.completion.script(main.root, .zsh);
const fish_script = cli.completion.script(main.root, .fish);

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"completion"}, args_ptr);
    const ctx = runtime.current();

    if (std.mem.eql(u8, args.shell, "bash")) {
        try ctx.stdout.writeAll(bash_script);
    } else if (std.mem.eql(u8, args.shell, "zsh")) {
        try ctx.stdout.writeAll(zsh_script);
    } else if (std.mem.eql(u8, args.shell, "fish")) {
        try ctx.stdout.writeAll(fish_script);
    } else {
        exit.die(
            ctx,
            error.InvalidInput,
            "unsupported shell '{s}'; supported: bash, zsh, fish",
            .{args.shell},
        );
    }
}
