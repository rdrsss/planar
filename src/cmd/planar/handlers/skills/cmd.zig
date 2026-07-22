//! handlers/skills/cmd.zig — `planar skills` (plan 918 M5: render/status/repair
//! retired; scriptorium is the sole renderer and owns install-drift detection
//! via its own manifest `check`/`status`). No subcommands remain under this
//! verb; the node stays registered as a placeholder so `planar skills` reports
//! a clear "no subcommands" rather than an unknown-verb error.

const cli = @import("cli");

pub const verb: cli.Cmd = .{
    .name = "skills",
    .desc = "Retired: rendering and drift detection now live in scriptorium.",
    .long_desc = "The unified skill source tree under skills/src/ is rendered by the\n  external scriptorium binary (plan 918). Planar no longer renders vendor\n  projections nor tracks their install-drift in-band; use `scriptorium\n  check`/`scriptorium status` instead. This command has no subcommands.",
};
