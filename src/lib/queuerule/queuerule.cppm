/// @file queuerule.cppm
/// @brief `planar.queuerule` — the authored agent rule text of the host build
/// and test queue, embedded at layer 1 (plan 1080, task 7110; plan 1142, task
/// mo-queue-rule-opt-in).
///
/// The rule is two authored files in this directory, embedded into the binary
/// by `#embed`, each returned as its bytes unchanged. `queue-rule.md` says how
/// to use the queue and is printed always (`queue_rule_text()`).
/// `queue-mandate.md` is the instruction that every build and test goes
/// through the queue; it applies only where the operator's policy sets
/// `use_queue = true`, so only the caller that has resolved that policy prints
/// it (`queue_mandate_text()`). It lives at layer 1 so that every engine bucket that needs it
/// (`engine_hostqueue`, `engine_workspace`) reaches it through a target edge
/// the architecture walk can see, instead of a cross-directory `#embed`.
///
/// Nothing here can fail and nothing opens a file or a database.
module;

export module planar.queuerule;

import std;

namespace planar::queuerule {

/// @brief The how-to text: the bytes of `queue-rule.md`, unchanged.
///
/// It begins with the `##` heading "Using the host queue" and ends with a
/// newline, so it can be placed under a document's own heading as it is. It
/// carries no instruction to use the queue. The view is over static storage
/// and never dangles.
/// @return The embedded how-to text.
export auto queue_rule_text() -> std::string_view;

/// @brief The mandate text: the bytes of `queue-mandate.md`, unchanged.
///
/// It begins with the `##` heading "Builds and tests go through the host
/// queue" and ends with a newline. It states the operator's policy as its
/// source, so print it only where `use_queue` resolves to true. The view is
/// over static storage and never dangles.
/// @return The embedded mandate text.
export auto queue_mandate_text() -> std::string_view;

} // namespace planar::queuerule
