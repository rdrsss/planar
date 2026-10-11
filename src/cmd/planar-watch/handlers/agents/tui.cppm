/// @file tui.cppm
/// @brief `planar.cmd.planar_watch.handlers.agents.tui` — the interactive
/// agent view a bare `planar-watch` opens on a terminal.
///
/// `wants_interactive` decides whether an invocation opens the view: only a
/// bare invocation, only with stdin and stdout both terminals, and never
/// under `TERM=dumb`. Everything else keeps the non-interactive `feed`
/// default, so scripts, agents and pipes see no change. `run_interactive`
/// draws the view with FTXUI and refreshes it from the read-only database
/// once a second; a database failure is shown in the footer and the last
/// good snapshot stays on screen. In-progress marks pulse dark to bright
/// green on terminals that report 24-bit colour; `p` turns that off.
module;
export module planar.cmd.planar_watch.handlers.agents.tui;
import std;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handlers.agents.model;

namespace planar::cmd::watch::agents {

/// @brief Whether this invocation should open the interactive view.
/// @param argv The full argv, binary name first.
/// @param stdin_tty Whether stdin is a terminal.
/// @param stdout_tty Whether stdout is a terminal.
/// @param env The process environment.
/// @return `true` only for a bare invocation on a capable terminal.
export auto wants_interactive(std::span<const std::string> argv, bool stdin_tty, bool stdout_tty, const env_lookup& env) -> bool;

/// @brief Whether stdin is a terminal.
/// @return The `isatty` result for stdin.
export auto stdin_is_tty() -> bool;

/// @brief Whether `out` is the process's stdout and that stdout is a terminal.
///
/// Comparing against `std::cout` first keeps every in-process test, which
/// writes to a string stream, out of the interactive path.
/// @param out The stream the invocation writes to.
/// @return `true` only when both hold.
export auto stdout_is_tty(std::ostream& out) -> bool;

/// @brief One full pulse, dark to bright and back.
export inline constexpr auto pulse_period = std::chrono::milliseconds{1600};

/// @brief Brightness of the in-progress pulse at a point in its cycle.
/// @param elapsed Time since the view started.
/// @return 0 (darkest) at the start of each period, 1 (brightest) halfway through.
export auto pulse_level(std::chrono::milliseconds elapsed) -> float;

/// @brief Whether a row has a mark that pulses: a `doing` task's glyph, or a
/// caret held by a working agent.
/// @param r The row.
/// @return `true` when the row animates while the pulse is on.
export auto row_pulses(const row& r) -> bool;

/// @brief Run the interactive agent view until the operator quits.
/// @param ctx The read-only invocation context.
/// @return The process exit code.
export auto run_interactive(context& ctx) -> int;

} // namespace planar::cmd::watch::agents
