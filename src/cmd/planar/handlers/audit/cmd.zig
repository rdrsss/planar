//! handlers/audit/cmd.zig — `planar audit {trail, commits, session, publish-decision, handoff-readiness}`
//!
//! M7 introduced `audit trail <entity-id> [--kind] [--grep]` reading from
//! `audit_log` + entity_links (slug `engine-audit-read`). M8 adds the
//! sibling form `audit trail --link <id>` (task 2297) that walks
//! external_links + sync_events for a specific external link, matching
//! the Go binary's `audit trail <link-id>` shape. The entity-id form
//! remains the default; supplying `--link` switches to the link-scoped
//! reader. The positional is therefore optional; omitting both surfaces
//! a usage error.

const cli = @import("cli");

const trail = @import("trail.zig");
const commits = @import("commits.zig");
const session = @import("session.zig");
const publish_decision = @import("publish_decision.zig");
const handoff_readiness = @import("handoff_readiness.zig");

pub const verb: cli.Cmd = .{
    .name = "audit",
    .desc = "Cross-plane audit trail commands.",
    .long_desc = "Cross-plane audit trail commands.\n\n  Subcommands inspect external-link history, query attributed session\n  commits, recompute decision publication targets, render session\n  timelines, and walk the full audit trail for any external link.",
    .cmds = &.{
        .{
            .name = "trail",
            .desc = "Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events).",
            .flags = &.{
                .{ .long = "--kind", .kind = .string },
                .{ .long = "--grep", .kind = .string },
                .{ .long = "--link", .kind = .string, .desc = "External link id; switches to link-scoped (external_links + sync_events) form" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "entity-id", .kind = .string, .required = false }},
            .run = cli.handler(trail.handle),
        },
        .{
            .name = "commits",
            .desc = "List commits attributed to sessions and claims.",
            .flags = &.{
                .{ .long = "--session", .kind = .int },
                .{ .long = "--task", .kind = .int },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--shas", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(commits.handle),
        },
        .{
            .name = "session",
            .desc = "Show the timeline for a session.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "session-id", .kind = .string, .required = true }},
            .run = cli.handler(session.handle),
        },
        .{
            .name = "publish-decision",
            .desc = "Post the decision body to linked operational-plane targets.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "decision-id", .kind = .string, .required = true }},
            .run = cli.handler(publish_decision.handle),
        },
        .{
            .name = "handoff-readiness",
            .desc = "Check resume-readiness for all in-flight tasks.",
            .flags = &.{
                .{ .long = "--threshold", .kind = .int, .default = .{ .int = 90 } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handoff_readiness.handle),
        },
    },
};
