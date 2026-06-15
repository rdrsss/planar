//! handlers/ext/cmd.zig — `planar ext {register {jira, github}, list, test, create, propagate, propagate-one}`

const cli = @import("cli");

const register = @import("register/cmd.zig");
const list = @import("list.zig");
const test_h = @import("test.zig");
const create = @import("create.zig");
const propagate = @import("propagate.zig");
const propagate_one = @import("propagate_one.zig");

pub const verb: cli.Cmd = .{
    .name = "ext",
    .desc = "Manage external operational-plane systems (Jira, GitHub Issues, etc.).",
    .long_desc = "Register and interact with external systems on the operational plane.\n\n  Sub-commands: register, list, test, create, propagate.\n  Currently supported systems: Jira, GitHub Issues, GitHub Projects.",
    .cmds = &.{
        register.verb,
        .{
            .name = "list",
            .desc = "List registered external systems.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(list.handle),
        },
        .{
            .name = "test",
            .desc = "Test connection to an external system.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "slug", .kind = .string, .required = true }},
            .run = cli.handler(test_h.handle),
        },
        .{
            .name = "create",
            .desc = "Create an external counterpart for a local entity.",
            .flags = &.{
                .{ .long = "--from", .kind = .string, .required = true, .desc = "Source local entity ref (kind:id)" },
                .{ .long = "--type", .kind = .string, .desc = "External issue type, e.g. Epic, Story" },
                .{ .long = "--role", .kind = .string, .desc = "Link role (default: mirror)" },
                .{ .long = "--sync", .kind = .string, .desc = "Sync direction (default: two-way)" },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "system-slug", .kind = .string, .required = true }},
            .run = cli.handler(create.handle),
        },
        .{
            .name = "propagate-one",
            .desc = "Render + POST + record one entity counterpart in one transaction, idempotent skip on existing link.",
            .long_desc = "Render one entity's template, POST the counterpart to the external system, and\n" ++
                "  record the external_links row in one transaction. Idempotent: if a mirror link\n" ++
                "  already exists for this entity+system pair, the call is a no-op and returns\n" ++
                "  op=skipped.\n\n" ++
                "  --from <kind:id>  Source local entity ref (plan:N or task:N)\n" ++
                "  --strategy        Override GitHub strategy: parent-issue, projects-v2, tracking-issue",
            .flags = &.{
                .{ .long = "--from", .kind = .string, .required = true, .desc = "Source local entity ref (kind:id, e.g. plan:42 or task:7)" },
                .{ .long = "--strategy", .kind = .string, .desc = "Override GitHub strategy: parent-issue, projects-v2, tracking-issue" },
                .{ .long = "--sync", .kind = .string, .desc = "Sync direction for created link: read-only, write-back, two-way" },
                .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false }, .desc = "Preview without contacting the remote system" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "system", .kind = .string, .required = true }},
            .run = cli.handler(propagate_one.handle),
        },
        .{
            .name = "propagate",
            .desc = "Propagate a feature (plan + descendants) to an external system.",
            .flags = &.{
                .{ .long = "--system", .kind = .string, .desc = "External system slug (defaults to first registered system)" },
                .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false }, .desc = "Preview creation plan without contacting the remote system" },
                .{ .long = "--restrategize", .kind = .bool, .default = .{ .bool = false }, .desc = "Force fresh strategy detection (M10 engine path)" },
                .{ .long = "--yes", .kind = .bool, .default = .{ .bool = false }, .desc = "Auto-confirm prompts (M10 engine path)" },
                .{ .long = "--verify-counterparts", .kind = .bool, .default = .{ .bool = false }, .desc = "Probe remote counterparts (M10 engine path)" },
                .{ .long = "--unlink", .kind = .bool, .default = .{ .bool = false }, .desc = "Remove links for missing counterparts (requires --verify-counterparts)" },
                .{ .long = "--recreate", .kind = .bool, .default = .{ .bool = false }, .desc = "Remove and recreate missing counterparts (requires --verify-counterparts)" },
                .{ .long = "--github-strategy", .kind = .string, .desc = "Override GitHub strategy: parent-issue, projects-v2, tracking-issue" },
                .{ .long = "--sync", .kind = .string, .desc = "Sync direction for created links: read-only, write-back, two-way" },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(propagate.handle),
        },
    },
};
