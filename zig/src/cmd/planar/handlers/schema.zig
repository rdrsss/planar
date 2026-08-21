//! handlers/schema.zig — `planar schema`
//!
//! Emit a deterministic flat JSON catalog of the entire command tree —
//! every command, its flags (own + inherited), aliases, and positionals.
//! Built at comptime from the root command tree via `cli.schema`, so the
//! handler is a pure write of a `.rodata` string: no runtime traversal.
//!
//! The catalog is intended for structured consumers — LLM tool routers,
//! editor integrations, and the CLI-usage linter under `scripts/` that
//! validates authored agent/skill/doc surfaces never reference a flag the
//! binary does not expose. Output is always JSON.

const std = @import("std");
const cli = @import("cli");
const main = @import("../main.zig");
const runtime = @import("runtime");

pub const verb: cli.Cmd = .{
    .name = "schema",
    .desc = "Print the full command tree as a JSON catalog (flags, aliases, positionals).",
    .run = cli.handler(handle),
};

// The flat catalog lives in `.rodata`; the handler is a pure write.
const catalog = cli.schema.json(main.root, .{});

fn handle(args_ptr: *const anyopaque) anyerror!void {
    _ = args_ptr;
    const ctx = runtime.current();
    try ctx.stdout.writeAll(catalog);
    try ctx.stdout.writeAll("\n");
}
