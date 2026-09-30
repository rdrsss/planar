/// @file rule.cppm
/// @brief `planar.cmd.planar_agent.handlers.queue.rule` — the `planar-agent
/// queue rule` handler (plan 1080, task hq-queue-rule-verb; tech spec 647 §
/// CLI surface).
///
/// `queue rule` prints the agent rule text that `planar.engine.hostqueue.rule`
/// embeds, for pasting into a project's own agent guide. The bytes go to
/// standard output unchanged and the exit status is 0.
///
/// ## No database
///
/// The handler opens no database: neither `planar.db` (`uses_main_database`
/// exempts the whole `queue` domain) nor the agent database. It reads no
/// environment variable and no configuration file, so it works where nothing
/// else does: a locked-out main database, a sandbox that cannot reach the
/// agent store, or a missing `HOME`. It takes no argument and no flag.
module;

export module planar.cmd.planar_agent.handlers.queue.rule;

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief `planar-agent queue rule`: print the embedded rule text.
/// @param ctx The invocation context; only its standard output is used.
/// @param args The parsed arguments; none are declared.
/// @return Success; the text has been written to standard output.
export auto queue_rule(context& ctx, const cliapp::parsed_args& args) -> handler_outcome;

} // namespace planar::cmd::agent::handlers
