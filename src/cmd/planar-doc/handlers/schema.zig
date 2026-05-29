//! handlers/schema.zig — `planar-doc schema`
//!
//! Emit the binary's full command tree as a deterministic flat JSON
//! catalog (every command + flags + aliases + positionals), built at
//! comptime via `cli.schema`. Consumed by the CLI-usage linter under
//! `scripts/` to validate authored agent/skill/doc surfaces never
//! reference a flag this binary does not expose. Output is always JSON.

const std = @import("std");
const cli = @import("cli");
const main = @import("../main.zig");
const runtime = @import("runtime");

pub const verb: cli.Cmd = .{
    .name = "schema",
    .desc = "Print the full command tree as a JSON catalog (flags, aliases, positionals).",
    .run = cli.handler(handle),
};

const catalog = cli.schema.json(main.root, .{});

fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = args_ptr;
    const ctx = runtime.current();
    try ctx.stdout.writeAll(catalog);
    try ctx.stdout.writeAll("\n");
}
