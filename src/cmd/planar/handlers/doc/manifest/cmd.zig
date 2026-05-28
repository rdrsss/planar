//! handlers/doc/manifest/cmd.zig — `planar doc manifest {verify, diff, update, info}`
//!
//! Four-verb surface over the `.manifest-docs` Merkle index:
//!   - `verify`  — O(1) root compare; the drift gate. Exits non-zero on drift.
//!   - `diff`    — per-path four-signal classifier; survey, exits 0.
//!   - `update`  — rebuild the manifest and write atomically.
//!   - `info`    — print one entry's hashes + sources.
//!
//! Per plan 85 t#2661 — clean rename of the prior build/check/validate
//! surface to the names docs/features/doc-system.md describes.

const cli = @import("cli");

const verify = @import("verify.zig");
const diff = @import("diff.zig");
const update = @import("update.zig");
const info = @import("info.zig");

pub const verb: cli.Cmd = .{
    .name = "manifest",
    .desc = "Manage the docs/ Merkle index (.manifest-docs).",
    .cmds = &.{
        .{
            .name = "verify",
            .desc = "O(1) root compare against the stored manifest; non-zero on drift.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(verify.handle),
        },
        .{
            .name = "diff",
            .desc = "Per-path four-signal drift survey (regenerate-candidate, hand-edit, new-authoring, deletion).",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(diff.handle),
        },
        .{
            .name = "update",
            .desc = "Rebuild the manifest from the current docs/ tree and write atomically.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(update.handle),
        },
        .{
            .name = "info",
            .desc = "Inspect one manifest entry's hashes and sources.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "entry-path", .kind = .string, .required = true },
            },
            .run = cli.handler(info.handle),
        },
    },
};
