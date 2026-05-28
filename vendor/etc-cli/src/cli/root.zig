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
//!       const argv = try cli.argv(allocator, init.minimal.args);
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
const meta_mod = @import("meta.zig");
const validate_mod = @import("validate.zig");
const help_mod = @import("help.zig");
const completion_mod = @import("completion.zig");
const man_mod = @import("man.zig");
const doc_mod = @import("doc.zig");
const schema_mod = @import("schema.zig");
const artifacts_mod = @import("artifacts.zig");
const app_mod = @import("app.zig");
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
pub const Deprecation = meta_mod.Deprecation;
pub const Completion = meta_mod.Completion;
pub const CompletionKind = meta_mod.CompletionKind;
pub const Doc = doc_mod.Doc;
pub const Example = doc_mod.Example;
pub const ExitCode = doc_mod.ExitCode;
pub const Detail = err_mod.Detail;
pub const StructuredError = err_mod.Structured;
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
pub const help = help_mod;
pub const helpText = help_mod.helpText;
pub const helpTextWithOptions = help_mod.helpTextWithOptions;

// Comptime shell-completion script generation.
pub const completion = completion_mod;
pub const Shell = completion_mod.Shell;

// Comptime man-page generation.
pub const man = man_mod;

// Comptime machine-readable command schema generation.
pub const schema = schema_mod;

// Pure packaging/install artifact helpers.
pub const artifacts = artifacts_mod;

// Runtime entry points.
pub const run = app_mod.run;
pub const RunOptions = app_mod.Options;
pub const ExitCodes = app_mod.ExitCodes;
pub const parse = parser.parse;
pub const dispatch = parser.dispatch;
pub const formatError = err_mod.format;
pub const structuredError = err_mod.structured;
pub const errorKindName = err_mod.kindName;

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
    _ = meta_mod;
    _ = validate_mod;
    _ = help_mod;
    _ = completion_mod;
    _ = man_mod;
    _ = doc_mod;
    _ = schema_mod;
    _ = artifacts_mod;
    _ = app_mod;
    _ = parser;
    _ = err_mod;
    _ = duration;
}
