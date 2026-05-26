//! handlers/links/cmd.zig — `planar links {add, list, remove, trail, update}`
//!
//! Top-level entity_links management: add / list / remove / trail operate on
//! the entity_links table (internal cross-cutting relationships). The `update`
//! subverb is stubbed (deferred to M11 — it targets external_links, not
//! entity_links; see update.zig for the follow-up task slug).
//!
//! Note: `planar link` (singular, handlers/link.zig) is the DIFFERENT verb
//! that creates external_links rows (Jira / GitHub Issues bindings). These
//! two verbs are intentionally distinct.

const cli = @import("cli");

const add = @import("add.zig");
const list = @import("list.zig");
const remove = @import("remove.zig");
const trail = @import("trail.zig");
const update = @import("update.zig");

pub const verb: cli.Cmd = .{
    .name = "links",
    .desc = "List or remove internal entity_links relationships.",
    .long_desc = "Manage internal cross-cutting entity_links relationships.\n\n  Entity links record typed relationships between any two Planar\n  entities (e.g. a task cites an artifact, a plan blocks another\n  plan). This domain is distinct from the top-level link/unlink\n  commands, which operate on external-system ticket linkage.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Create an entity_links row between two entities.",
            .flags = &.{
                .{ .long = "--relationship", .kind = .string, .required = true },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "from-ref", .kind = .string, .required = true },
                .{ .name = "to-ref", .kind = .string, .required = true },
            },
            .run = cli.handler(add.handle),
        },
        .{
            .name = "list",
            .desc = "List entity_links where the given entity is source or target.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "ref", .kind = .string, .required = true },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "remove",
            .desc = "Delete an entity_links row by its id.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "link-id", .kind = .string, .required = true },
            },
            .run = cli.handler(remove.handle),
        },
        .{
            .name = "trail",
            .desc = "Show the audit trail for an entity_links row.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "link-id", .kind = .string, .required = true },
            },
            .run = cli.handler(trail.handle),
        },
        .{
            .name = "update",
            .desc = "Change sync_direction on an existing external_links row (deferred to M11).",
            .flags = &.{
                .{ .long = "--sync", .kind = .string, .required = true },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "link-id", .kind = .string, .required = true },
            },
            .run = cli.handler(update.handle),
        },
    },
};
