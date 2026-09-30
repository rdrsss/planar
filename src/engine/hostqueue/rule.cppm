/// @file rule.cppm
/// @brief `planar.engine.hostqueue.rule` — the agent rule text of the host
/// build and test queue (plan 1080, tasks hq-rule-text and hq-queue-rule-verb;
/// tech spec 647 § The rule text has one authored source).
///
/// The rule tells an agent what counts as a build or test command, how to
/// submit one detached and poll for it, what to do on each outcome, when to
/// stop on exit 125, and how to tell a Planar without the queue from a queue
/// that refused. It is ONE authored file, `queue-rule.md` in this directory,
/// embedded into the binary at build time by `#embed`. The text is the
/// file's bytes, unchanged.
///
/// Later surfaces copy it (`agents/methodology.md`) or shorten it (role files,
/// the workspace guide template). Each compares against `queue_rule_text()`,
/// or against the output of `planar-agent queue rule`, so the file stays the
/// only place the rule is written.
///
/// Nothing here can fail and nothing opens a database.
module;

export module planar.engine.hostqueue.rule;

import std;

namespace planar::engine::hostqueue {

/// @brief The rule text: the bytes of `queue-rule.md`, unchanged.
///
/// It begins with the `##` heading "Builds and tests go through the host
/// queue" and ends with a newline, so it can be placed under a document's own
/// heading as it is. The view is over static storage and never dangles.
/// @return The embedded rule text.
export auto queue_rule_text() -> std::string_view;

} // namespace planar::engine::hostqueue
