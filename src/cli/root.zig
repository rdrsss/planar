//! cli — comptime-driven command-tree argument parser.
//!
//! Public API surface. See per-file doc comments for design notes.
//!
//!   const root = cli.Cmd{
//!       .name = "tool",
//!       .flags = &.{ ... },
//!       .cmds = &.{
//!           .{
//!               .name = "do",
//!               .flags = &.{ .{ .long = "--it", .kind = .string, .required = true } },
//!               .run = cli.handler(handleDoIt),
//!           },
//!       },
//!   };
//!
//!   comptime cli.validate(root);
//!
//!   pub fn main(init: std.process.Init) !void {
//!       const allocator = init.arena.allocator();
//!       const argv = try cli.argv(allocator);
//!       defer cli.freeArgv(allocator, argv);
//!
//!       // Callback mode: dispatch invokes the matched leaf's handler.
//!       var buf: [4096]u8 = undefined;
//!       var fw: std.Io.File.Writer = .init(.stdout(), init.io, &buf);
//!       try cli.dispatch(root, argv, &fw.interface);
//!       try fw.interface.flush();
//!
//!       // OR — Result mode: parse + manually switch on the union.
//!       // var detail: cli.Detail = undefined;
//!       // const result = try cli.parse(root, argv, &detail);
//!       // switch (result) { .match => |u| switch (u) { ... }, .help => |p| ... }
//!   }
//!
//!   fn handleDoIt(args: cli.ArgsType(root, &.{"do"})) !void {
//!       std.debug.print("doing it with: {s}\n", .{args.it});
//!   }

const flag = @import("flag.zig");
const cmd = @import("cmd.zig");
const validate_mod = @import("validate.zig");
const help_mod = @import("help.zig");
const completion_mod = @import("completion.zig");
const parser = @import("parser.zig");
const err_mod = @import("error.zig");
const platform = @import("platform/root.zig");

/// Duration-string parser shared across CLI flags that accept human
/// durations (`--ttl`, `--stale-after`, `--interval`). See
/// `src/cli/duration.zig` for accepted formats.
pub const duration = @import("duration.zig");

// Types.
pub const Cmd = cmd.Cmd;
pub const Flag = flag.Flag;
pub const Positional = flag.Positional;
pub const Kind = flag.Kind;
pub const Default = flag.Default;
pub const Detail = err_mod.Detail;
pub const Parse = err_mod.Parse;

// Comptime helpers (tree introspection + typed args).
pub const ArgsType = cmd.ArgsType;
pub const ResultUnion = parser.ResultUnion;
pub const Result = parser.Result;
pub const findCmd = cmd.findCmd;
pub const allLeaves = cmd.allLeaves;
pub const collectInheritedFlags = cmd.collectInheritedFlags;

// Handler wrapping (type-erases a typed function into Cmd.run).
pub const handler = cmd.handler;
pub const HandlerFn = cmd.HandlerFn;
pub const castArgs = cmd.castArgs;

// Comptime validation (call once near the tree decl with `comptime cli.validate(root)`).
pub const validate = validate_mod.validate;

// Comptime help-text generation.
pub const helpText = help_mod.helpText;

// Comptime shell-completion script generation.
pub const completion = completion_mod;
pub const Shell = completion_mod.Shell;

// Runtime entry points.
pub const parse = parser.parse;
pub const dispatch = parser.dispatch;
pub const formatError = err_mod.format;

// Platform (OS-isolated argv acquisition).
pub const argv = platform.argv;
pub const freeArgv = platform.freeArgv;

// Re-export the platform namespace itself for callers who want to reach
// past argv/freeArgv into future OS helpers.
pub const Platform = platform;

// Pull every submodule into the test build. `zig build test` only walks
// tests reachable from the compiled root unit, so without these refs the
// sub-file `test` blocks are dropped on the floor.
test {
    _ = flag;
    _ = cmd;
    _ = validate_mod;
    _ = help_mod;
    _ = completion_mod;
    _ = parser;
    _ = err_mod;
    _ = duration;
}
