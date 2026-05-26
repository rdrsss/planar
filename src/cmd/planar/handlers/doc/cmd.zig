//! handlers/doc/cmd.zig — `planar doc {lint, manifest {build, check, validate},
//!   promote, regenerate, backlinks, orphans, coverage}`

const cli = @import("cli");

const lint = @import("lint.zig");
const manifest = @import("manifest/cmd.zig");
const promote = @import("promote.zig");
const regenerate = @import("regenerate.zig");
const backlinks = @import("backlinks.zig");
const orphans = @import("orphans.zig");
const coverage = @import("coverage.zig");

pub const verb: cli.Cmd = .{
    .name = "doc",
    .desc = "Outward-facing documentation: lint, manifest, drift detection.",
    .long_desc = "Outward-facing documentation tooling.\n\n  The lint subcommand validates citations and reference declarations\n  in markdown docs. The manifest subcommands track per-doc content +\n  source hashes in .manifest-docs so drift between published docs and\n  the underlying source artifacts is detectable.",
    .cmds = &.{
        .{
            .name = "lint",
            .desc = "Lint citations and references across docs.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
                .{ .long = "--no-refs", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--refs-only", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(lint.handle),
        },
        manifest.verb,
        .{
            .name = "promote",
            .desc = "Promote internal Planar entities into published docs.",
            .flags = &.{
                .{ .long = "--kind", .kind = .string, .required = true },
                .{ .long = "--source", .kind = .string, .required = true },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--out", .kind = .string },
                .{ .long = "--body-file", .kind = .string },
                .{ .long = "--title", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(promote.handle),
        },
        .{
            .name = "regenerate",
            .desc = "Re-synthesise existing docs from their current sources.",
            .flags = &.{
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--path", .kind = .string },
                .{ .long = "--all", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--merge", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--body-file", .kind = .string },
                .{ .long = "--out", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(regenerate.handle),
        },
        .{
            .name = "backlinks",
            .desc = "Report which docs link to a given entity.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "ref", .kind = .string, .required = true }},
            .run = cli.handler(backlinks.handle),
        },
        .{
            .name = "orphans",
            .desc = "Find docs not referenced by any entity.",
            .flags = &.{
                .{ .long = "--kind", .kind = .string, .default = .{ .string = "research" } },
                .{ .long = "--since", .kind = .int, .default = .{ .int = 90 } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(orphans.handle),
        },
        .{
            .name = "coverage",
            .desc = "Report documentation coverage metrics.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(coverage.handle),
        },
    },
};
