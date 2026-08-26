/// @file transitions.cpp
/// @brief Implementation of `planar.engine.planning.transitions` (see
/// transitions.cppm).

module;

module planar.engine.planning.transitions;

import std;

namespace planar::engine::planning {

namespace {

auto check_plan(std::string_view from, std::string_view to) -> std::expected<void, transition_error> {
  bool legal = false;
  if (from == "draft") {
    legal = to == "active";
  } else if (from == "active") {
    legal = to == "paused" || to == "done" || to == "abandoned";
  } else if (from == "paused") {
    legal = to == "active";
  } else if (from == "done" || from == "abandoned") {
    legal = false; // terminal for operator transitions.
  } else {
    return std::unexpected(transition_error::unknown_status);
  }
  if (!legal) {
    return std::unexpected(transition_error::illegal_transition);
  }
  return {};
}

auto check_task(std::string_view from, std::string_view to) -> std::expected<void, transition_error> {
  bool legal = false;
  if (from == "todo") {
    legal = to == "doing" || to == "blocked" || to == "cancelled";
  } else if (from == "doing") {
    legal = to == "todo" || to == "blocked" || to == "done" || to == "cancelled";
  } else if (from == "blocked") {
    legal = to == "doing" || to == "done" || to == "cancelled";
  } else if (from == "done" || from == "cancelled") {
    legal = false; // terminal for bare update; escape via reopen/force.
  } else {
    return std::unexpected(transition_error::unknown_status);
  }
  if (!legal) {
    return std::unexpected(transition_error::illegal_transition);
  }
  return {};
}

/// @brief The `question` arm. Status set: open, answered, wontfix.
///
/// Note the missing `unknown_status` arm — it is not an omission. zig's
/// `policy.status.check`'s `.question` branch returns `IllegalTransition`
/// for EVERY non-`open` source, including a source it does not recognize,
/// where the plan/task/annotation/handoff branches all distinguish the two.
/// Reproduced rather than regularized (D2): the error name reaches the
/// operator verbatim.
/// @param from The current status text.
/// @param to The desired status text.
/// @return Success, or `illegal_transition`.
auto check_question(std::string_view from, std::string_view to) -> std::expected<void, transition_error> {
  if (from == "open" && (to == "answered" || to == "wontfix")) {
    return {};
  }
  return std::unexpected(transition_error::illegal_transition);
}

/// @brief The `decision` arm. Status set: proposed, accepted, superseded,
/// withdrawn.
///
/// This arm DOES carry the `unknown_status` fallthrough the question arm
/// lacks, because zig's `.decision` branch has one. It is nonetheless
/// unreachable in practice: `decisions.status` carries a CHECK constraint
/// and `decision.cpp`'s `read_row` refuses an unparseable value as
/// `query_failed` before any transition is validated.
/// @param from The current status text.
/// @param to The desired status text.
/// @return Success, `illegal_transition`, or `unknown_status`.
auto check_decision(std::string_view from, std::string_view to) -> std::expected<void, transition_error> {
  bool legal = false;
  if (from == "proposed") {
    legal = to == "accepted" || to == "superseded" || to == "withdrawn";
  } else if (from == "accepted") {
    // No edge back to `proposed`: acceptance is not reversible.
    legal = to == "superseded" || to == "withdrawn";
  } else if (from == "superseded" || from == "withdrawn") {
    // Both terminal for a NON-identity move. `superseded -> superseded`
    // never reaches here: `check_transition` short-circuits first, which is
    // why re-superseding a decision by a different one succeeds.
    legal = false;
  } else {
    return std::unexpected(transition_error::unknown_status);
  }
  if (!legal) {
    return std::unexpected(transition_error::illegal_transition);
  }
  return {};
}

auto check_annotation(std::string_view from, std::string_view to) -> std::expected<void, transition_error> {
  bool legal = false;
  if (from == "active") {
    legal = to == "resolved" || to == "dismissed" || to == "archived";
  } else if (from == "resolved" || from == "dismissed") {
    // Outcome states under the retention-tier model (plan 692): only
    // progression to `archived` is legal -- never back to `active` and
    // never across to each other.
    legal = to == "archived";
  } else if (from == "archived") {
    legal = false; // the sole final state; no outgoing edges.
  } else {
    return std::unexpected(transition_error::unknown_status);
  }
  if (!legal) {
    return std::unexpected(transition_error::illegal_transition);
  }
  return {};
}

/// @brief The `handoff` arm. Status set: pending, validated, consumed,
/// abandoned.
///
/// `force` never reaches here: every handoff call site passes false, as
/// the Zig original's `handoff.validateTransition` does. The `--reason`
/// gate on `abandon` is a HANDLER-layer concern, not a matrix one — the
/// matrix permits `pending -> abandoned` and `validated -> abandoned`
/// unconditionally.
auto check_handoff(std::string_view from, std::string_view to) -> std::expected<void, transition_error> {
  bool legal = false;
  if (from == "pending") {
    legal = to == "validated" || to == "consumed" || to == "abandoned";
  } else if (from == "validated") {
    // No edge back to `pending`: validation is not reversible.
    legal = to == "consumed" || to == "abandoned";
  } else if (from == "consumed" || from == "abandoned") {
    legal = false; // both terminal; no outgoing edges at all.
  } else {
    return std::unexpected(transition_error::unknown_status);
  }
  if (!legal) {
    return std::unexpected(transition_error::illegal_transition);
  }
  return {};
}

} // namespace

auto check_transition(transition_kind kind, std::string_view from, std::string_view to, bool force)
    -> std::expected<void, transition_error> {
  if (from == to) {
    return {};
  }
  if (force && kind == transition_kind::task) {
    return {};
  }
  switch (kind) {
  case transition_kind::plan:
    return check_plan(from, to);
  case transition_kind::task:
    return check_task(from, to);
  case transition_kind::question:
    return check_question(from, to);
  case transition_kind::decision:
    return check_decision(from, to);
  case transition_kind::annotation:
    return check_annotation(from, to);
  case transition_kind::handoff:
    return check_handoff(from, to);
  }
  return std::unexpected(transition_error::unknown_status);
}

} // namespace planar::engine::planning
