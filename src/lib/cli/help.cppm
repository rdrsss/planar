/// @file help.cppm
/// @brief `planar.cli.help` — renders the `--help` text for a command tree
/// node: USAGE/COMMANDS/FLAGS/FLAG GROUPS/POSITIONAL ARGUMENTS sections.
///
/// Behavior-preserving port of zig/vendor/etcli-zig/src/cli/help.zig (D2, D9).
/// etcli-zig builds this string entirely at comptime (`.rodata`, zero runtime
/// cost beyond the final write) because Zig's `comptime` block is the only
/// tool it has for "build this once, cheaply". C++26 has no equivalent
/// need here — help text is rendered on demand, once per `--help`
/// invocation, from ordinary runtime `cmd`/`flag`/`positional` data — so
/// this is plain `std::string`-building, matching the tech-spec
/// instruction to prefer runtime data "where constexpr does not buy real
/// compile-time structure". The section layout, header text, and column
/// padding scheme are ported field-for-field (verified byte-for-byte
/// against `./zig/zig-out/bin/planar task add --help` /
/// `task done --help` / `./zig/zig-out/bin/planar-agent fail --help` in
/// help.t.cpp — see that file for the captured reference text). Colorized
/// (ANSI) rendering, hidden/deprecated filtering, and the `width`-driven
/// compact layout are deferred — no modeled verb in this task's subset
/// needs them, and etcli-zig's own `Options` struct documents them as optional
/// knobs layered on top of the same section-builder shape this port
/// keeps.
module;

export module planar.cli.help;

import std;
import planar.cli.flag;
import planar.cli.cmd;

namespace planar::cli {

/// @brief Render the `--help` text for the command at `path` under `root`.
/// @param root The command tree root.
/// @param path The path to the node whose help page should render (empty
/// for the root's own page).
/// @return The rendered help text (matches etcli-zig's `helpText` section
/// layout: header/USAGE/COMMANDS/FLAGS/FLAG GROUPS/POSITIONAL ARGUMENTS).
export auto render_help(cmd const& root, std::span<std::string const> path) -> std::string;

} // namespace planar::cli
