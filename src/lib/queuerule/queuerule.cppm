/// @file queuerule.cppm
/// @brief `planar.queuerule` — the authored agent rule text of the host build
/// and test queue, embedded at layer 1 (plan 1080, task 7110).
///
/// The rule is ONE authored file, `queue-rule.md` in this directory, embedded
/// into the binary by `#embed`; `queue_rule_text()` is the file's bytes,
/// unchanged. It lives at layer 1 so that every engine bucket that needs it
/// (`engine_hostqueue`, `engine_workspace`) reaches it through a target edge
/// the architecture walk can see, instead of a cross-directory `#embed`.
///
/// Nothing here can fail and nothing opens a file or a database.
module;

export module planar.queuerule;

import std;

namespace planar::queuerule {

/// @brief The rule text: the bytes of `queue-rule.md`, unchanged.
///
/// It begins with the `##` heading "Builds and tests go through the host
/// queue" and ends with a newline, so it can be placed under a document's own
/// heading as it is. The view is over static storage and never dangles.
/// @return The embedded rule text.
export auto queue_rule_text() -> std::string_view;

} // namespace planar::queuerule
