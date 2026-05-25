//! handlers/templates/cmd.zig — `planar templates {list, show, render, validate, init, path}`

const cli = @import("cli");

const list = @import("list.zig");
const show = @import("show.zig");
const render = @import("render.zig");
const validate = @import("validate.zig");
const init = @import("init.zig");
const path = @import("path.zig");

pub const verb: cli.Cmd = .{
    .name = "templates",
    .desc = "Inspect, validate, and render Planar JSON templates.",
    .cmds = &.{
        .{
            .name = "list",
            .desc = "List available templates.",
            .flags = &.{
                .{ .long = "--system", .kind = .string },
                .{ .long = "--set", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "show",
            .desc = "Show a template's raw JSON.",
            .positionals = &.{
                .{ .name = "set", .kind = .string, .required = true },
                .{ .name = "system", .kind = .string, .required = true },
                .{ .name = "kind", .kind = .string, .required = true },
            },
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "render",
            .desc = "Render a template against a database entity.",
            .positionals = &.{
                .{ .name = "set", .kind = .string, .required = true },
                .{ .name = "system", .kind = .string, .required = true },
                .{ .name = "kind", .kind = .string, .required = true },
                .{ .name = "entity-ref", .kind = .string, .required = true },
            },
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .run = cli.handler(render.handle),
        },
        .{
            .name = "validate",
            .desc = "Validate template syntax.",
            .positionals = &.{
                .{ .name = "set", .kind = .string, .required = true },
                .{ .name = "system", .kind = .string, .required = true },
                .{ .name = "kind", .kind = .string, .required = true },
            },
            .run = cli.handler(validate.handle),
        },
        .{
            .name = "init",
            .desc = "Extract default templates to disk.",
            .flags = &.{
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(init.handle),
        },
        .{
            .name = "path",
            .desc = "Show template resolution paths.",
            .flags = &.{
                .{ .long = "--system", .kind = .string },
                .{ .long = "--set", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(path.handle),
        },
    },
};
