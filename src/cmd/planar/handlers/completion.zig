//! handlers/completion.zig — `planar completion <shell>`
//!
//! Emit a shell-completion script for `bash`, `zsh`, or `fish`. The
//! script is built at comptime from the root command tree (see
//! `src/cli/completion.zig`), so generation is a pure write of a
//! `.rodata` string — no runtime tree traversal, no template engine.
//!
//! Shapes (intentional, v1):
//!   - bash:  function + `compgen`, scoped to the verb tree.
//!   - zsh:   compdef-style with `_describe` so descriptions show in
//!            completion menus.
//!   - fish:  one `complete` line per option gated on the current path.
//!
//! Limitations match the comptime module: flag *values* and positional
//! arguments are not completed. Sourcing the bash/zsh output or placing
//! the fish output into `~/.config/fish/completions/planar.fish` gives
//! working tab-completion for verbs and flag names.
//!
//! Mirrors Go's cobra-generated `planar completion <shell>`. Output is
//! NOT byte-identical to cobra; the locked decision is "working
//! completions for the verb tree, not byte-identical to Go" (cycle A).

const std = @import("std");
const cli = @import("cli");
const main = @import("../main.zig");
const runtime = @import("../runtime.zig");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "completion",
    .desc = "Generate the autocompletion script for the specified shell.",
    .positionals = &.{
        .{ .name = "shell", .kind = .string, .required = true, .desc = "Shell: bash, zsh, or fish" },
    },
    .run = cli.handler(handle),
};

// The comptime-generated scripts live in `.rodata`; the handler picks the
// one matching the operator's requested shell.
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
