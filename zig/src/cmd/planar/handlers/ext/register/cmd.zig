//! handlers/ext/register/cmd.zig — `planar ext register {jira, github}`

const cli = @import("cli");

const jira = @import("jira.zig");
const github = @import("github.zig");

pub const verb: cli.Cmd = .{
    .name = "register",
    .desc = "Register an external system.",
    .cmds = &.{
        .{
            .name = "jira",
            .desc = "Register a Jira instance as an external system.",
            .flags = &.{
                .{ .long = "--base-url", .kind = .string, .required = true },
                .{ .long = "--project", .kind = .string, .required = true },
                .{ .long = "--auth-env", .kind = .string, .required = true, .desc = "Env var name holding the API token" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "slug", .kind = .string, .required = true }},
            .run = cli.handler(jira.handle),
        },
        .{
            .name = "github",
            .desc = "Register a GitHub Issues repository as an external system.",
            .flags = &.{
                .{ .long = "--project", .kind = .string, .required = true, .desc = "GitHub repository owner/repo" },
                .{ .long = "--auth-env", .kind = .string, .desc = "Env var name holding the token (uses gh-cli if omitted)" },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "slug", .kind = .string, .required = true }},
            .run = cli.handler(github.handle),
        },
    },
};
