//! handlers/spec/cmd.zig — `planar spec {ingest}`

const cli = @import("cli");

const ingest = @import("ingest.zig");

pub const verb: cli.Cmd = .{
    .name = "spec",
    .desc = "Spec pipeline commands (draft, ingest).",
    .cmds = &.{
        .{
            .name = "ingest",
            .desc = "Decompose workbench spec documents into the task graph.",
            .flags = &.{
                .{ .long = "--apply", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--apply-removals", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--format", .kind = .string, .default = .{ .string = "text" } },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--strict", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan", .kind = .string, .required = true }},
            // Extra positionals beyond `plan` are batched: `spec ingest <p1>
            // <p2> <p3>` processes each in turn and (in --format json mode)
            // emits an array. Mirrors Go's `ingest <plan> [plan ...]`.
            .rest_field = "extra_plans",
            .run = cli.handler(ingest.handle),
        },
    },
};
