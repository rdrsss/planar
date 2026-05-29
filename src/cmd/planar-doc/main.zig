//! planar-doc — repo-state documentation manifest tool (plan 423).
//!
//! Fourth binary in the Planar family. Unlike the other three binaries,
//! `planar-doc` does NOT open SQLite — its job is to walk the working
//! tree, hash files, and read/write a single repo-tracked
//! `.planar-manifest` file at the repo root. No DB handle, no
//! planning-entity touch.

const std = @import("std");

const cli = @import("cli");
const runtime = @import("runtime");

const cmd_tree = @import("handlers/cmd.zig");
const exit = @import("exit.zig");

/// Root command tree for `planar-doc`.
pub const root: cli.Cmd = .{
    .name = "planar-doc",
    .desc = "Repo-state documentation manifest tool.",
    .long_desc = "planar-doc maintains a repo-tracked manifest (.planar-manifest) that\n" ++
        "  links published docs to source-area xxh64 hashes. The binary never\n" ++
        "  opens SQLite — it walks the working tree, computes hashes, and\n" ++
        "  reads/writes one file at the repo root.",
    .cmds = cmd_tree.verbs,
};

comptime {
    @setEvalBranchQuota(50_000);
    cli.validate(root);
}

var stdout_buffer: [4096]u8 = undefined;
var stderr_buffer: [1024]u8 = undefined;

pub fn main(init: std.process.Init) !void {
    const arena: std.mem.Allocator = init.arena.allocator();
    const raw_args = try init.minimal.args.toSlice(arena);

    const db_path = try runtime.resolveDbPath(arena, init.minimal.environ);
    runtime.init(arena, init.io, &stdout_buffer, &stderr_buffer, db_path, init.minimal.environ, raw_args);
    defer runtime.shutdown();

    cli.dispatch(root, raw_args, runtime.current().stdout) catch |e| switch (e) {
        cli.Parse.UnknownFlag,
        cli.Parse.MissingValue,
        cli.Parse.InvalidValue,
        cli.Parse.MissingRequired,
        cli.Parse.MissingRequiredPositional,
        cli.Parse.TooManyPositionals,
        cli.Parse.UnknownSubcommand,
        cli.Parse.UnexpectedArgument,
        cli.Parse.DuplicateFlag,
        => exit.die(runtime.current(), e, "{s}", .{@errorName(e)}),
        error.NotImplemented => exit.die(runtime.current(), e, "not implemented yet", .{}),
        else => return e,
    };

    try runtime.flush();
}
