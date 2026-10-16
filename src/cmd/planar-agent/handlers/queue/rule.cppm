/// @file rule.cppm
/// @brief `planar.cmd.planar_agent.handlers.queue.rule` — the `planar-agent
/// queue rule` handler (plan 1080, task hq-queue-rule-verb; tech spec 647 §
/// CLI surface).
///
/// `queue rule` prints the agent rule text that `planar.engine.hostqueue.rule`
/// embeds, for pasting into a project's own agent guide. The how-to part (how
/// to submit, observe and read the outcome of a queued command, and what an
/// exit 125 refusal means) is always printed. The mandate part (every build and
/// test goes through the queue; do not run it yourself) is the operator's
/// policy and is printed, with a `Source:` line naming the config file and
/// table, only when `use_queue` resolves to true for the caller's scope.
///
/// ## Policy and scope
///
/// The policy is the `[policy]` table of `config.toml` (`PLANAR_CONFIG_PATH`,
/// else `$HOME/.planar/config.toml`), refined by the
/// `[associations."<slug>".policy]` table of the caller's association. No
/// environment variable participates. The association comes from the working
/// directory, as every write verb derives its scope (`derive_from_cwd`), and is
/// looked up only when some association table sets `use_queue`, so the usual
/// invocation still opens no database. That lookup opens `planar.db` read-only
/// (never creating it, applying nothing); if it cannot be made the top-level
/// table alone decides and a trailing line says so.
///
/// ## Never fails on configuration
///
/// A config file that cannot be read or parsed, or a `[policy]` table with an
/// unknown or mistyped key, prints the how-to and one `policy unreadable: ...`
/// line, and the exit status is 0: the queue's usage text is available where
/// nothing else is. The verb takes no argument and no flag.
module;

export module planar.cmd.planar_agent.handlers.queue.rule;

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief `planar-agent queue rule`: print the how-to, and the mandate when the caller's policy sets `use_queue`.
/// @param ctx The invocation context: standard output, the environment and the working directory.
/// @param args The parsed arguments; none are declared.
/// @return Success; the text has been written to standard output.
export auto queue_rule(context& ctx, const cliapp::parsed_args& args) -> handler_outcome;

} // namespace planar::cmd::agent::handlers
