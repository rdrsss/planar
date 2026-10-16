/// @file rule.cppm
/// @brief `planar.engine.hostqueue.rule` — the agent rule text of the host
/// build and test queue (plan 1080, tasks hq-rule-text and hq-queue-rule-verb;
/// tech spec 647 § The rule text has one authored source; plan 1142, task
/// mo-queue-rule-opt-in, tech spec 693 component 8).
///
/// The rule has two parts. The how-to (`queue_rule_text()`) tells an agent how
/// to submit a command detached and observe its ticket, what to do on each
/// outcome, and what an exit 125 refusal means; it is always printed. The
/// mandate (`queue_mandate_text()`) says what counts as a build or test
/// command, that every one goes through the queue and that a refusal is never
/// bypassed; it is the operator's policy and is printed only where the
/// caller's scope has `use_queue = true`. `render_queue_rule` joins them for
/// `planar-agent queue rule` once the caller has resolved that policy; this
/// module resolves nothing and reads no configuration.
///
/// The two parts are TWO authored files, `queue-rule.md` and
/// `queue-mandate.md` in `src/lib/queuerule/`, embedded into the binary at
/// build time by the layer-1 `planar.queuerule` module (task 7110), which this
/// module forwards to so `engine_workspace` can share the same bytes without
/// an `engine_* -> engine_*` edge. Each text is its file's bytes, unchanged.
///
/// Later surfaces copy them (`agents/methodology.md`) or shorten them (role
/// files). Each compares against these functions, or against the output of
/// `planar-agent queue rule`, so the files stay the only place the rule is
/// written.
///
/// Nothing here can fail and nothing opens a database.
module;

export module planar.engine.hostqueue.rule;

import std;

namespace planar::engine::hostqueue {

/// @brief The how-to text: the bytes of `queue-rule.md`, unchanged.
///
/// It begins with the `##` heading "Using the host queue" and ends with a
/// newline, so it can be placed under a document's own heading as it is. It
/// instructs nothing about whether to use the queue. The view is over static
/// storage and never dangles.
/// @return The embedded how-to text.
export auto queue_rule_text() -> std::string_view;

/// @brief The mandate text: the bytes of `queue-mandate.md`, unchanged.
///
/// It begins with the `##` heading "Builds and tests go through the host
/// queue" and ends with a newline. The view is over static storage and never
/// dangles.
/// @return The embedded mandate text.
export auto queue_mandate_text() -> std::string_view;

/// @brief What `render_queue_rule` needs to know about the caller's policy.
export struct rule_policy {
  bool mandate = false; ///< Whether the caller's scope has `use_queue = true`.
  /// Where the true value was set, for the `Source:` line, for example
  /// `/home/op/.planar/config.toml, [policy]`. Read only when `mandate`.
  std::string source;
  /// Lines printed last, each after a blank line: why a policy could not be
  /// read, or which part of the scope could not be resolved.
  std::vector<std::string> notes;
};

/// @brief The text `planar-agent queue rule` prints.
///
/// The how-to always. When `policy.mandate` is set, the mandate follows and
/// then one line, `Source: operator policy use_queue = true (<source>)`. Each
/// note comes last on its own line. With no mandate and no note the result is
/// exactly `queue_rule_text()`.
/// @param policy The caller's resolved policy.
/// @return The text to print, ending with a newline.
export auto render_queue_rule(const rule_policy& policy) -> std::string;

} // namespace planar::engine::hostqueue
