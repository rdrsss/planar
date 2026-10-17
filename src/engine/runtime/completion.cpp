/// @file completion.cpp
/// @brief Implementation of `planar.engine.runtime.completion`.

module planar.engine.runtime.completion;

import std;
import planar.db;
import planar.engine.runtime.agentactivity;

namespace planar::engine::runtime::completion {

namespace aa = agentactivity;

namespace {

/// @brief The review half of the check.
/// @param conn The connection.
/// @param held The completing claim.
/// @return The unmet requirement, or unset when the review is satisfied.
auto check_review(db::connection& conn, const aa::claim& held)
    -> std::expected<std::optional<unmet_requirement>, aa::agent_error> {
  auto const reviews = aa::reviews_for_claim(conn, held.claim_token);
  if (!reviews) {
    return std::unexpected(reviews.error());
  }
  if (reviews->empty()) {
    return std::optional<unmet_requirement>{
        {requirement_kind::review_missing, "review_required: no review with a verdict is recorded for this claim"}};
  }
  auto const& latest = reviews->back();
  if (latest.verdict != aa::review_verdict::approve) {
    return std::optional<unmet_requirement>{
        {requirement_kind::review_changes,
         std::format("review_required: the latest review (action {}) requests changes", latest.action_id)}};
  }
  // The review verb enforces independence when it writes; re-check what a stored row says, because the
  // gate must not trust that every row came through the verb.
  bool const same_session_row    = latest.reviewer_session_id == held.session_id;
  bool const same_vendor_session = latest.reviewer_session.has_value() && held.vendor_session_id.has_value() &&
                                   *latest.reviewer_session == *held.vendor_session_id;
  if (same_session_row || same_vendor_session) {
    return std::optional<unmet_requirement>{
        {requirement_kind::review_not_independent,
         std::format("review_required: the latest approval (action {}) was written by the claim's own session",
                     latest.action_id)}};
  }
  return std::optional<unmet_requirement>{};
}

/// @brief The gate half of the check.
/// @param held The completing claim.
/// @param gate The policy's argv.
/// @param lookup The injected run lookup.
/// @return The unmet requirement, or unset when a passing run matches.
auto check_gate(const aa::claim& held, const std::vector<std::string>& gate, const gate_run_lookup& lookup)
    -> std::expected<std::optional<unmet_requirement>, aa::agent_error> {
  std::vector<std::string> roots;
  if (held.repo_root.has_value()) {
    roots.push_back(*held.repo_root);
  }
  if (held.worktree_path.has_value()) {
    roots.push_back(*held.worktree_path);
  }
  bool usable = false;
  for (auto const& root : roots) {
    usable = usable || normalize_directory(root).has_value();
  }
  if (!usable) {
    return std::optional<unmet_requirement>{
        {requirement_kind::gate_no_locality,
         "task_gate: the claim recorded no absolute repository root or worktree, so no run can be matched to it"}};
  }
  if (!lookup) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  auto const runs = lookup(held.claim_token);
  if (!runs) {
    return std::optional<unmet_requirement>{
        {requirement_kind::gate_unreadable, std::format("task_gate: the build queue database cannot be read: {}", runs.error())}};
  }
  for (auto const& run : *runs) {
    if (!run.passed || run.argv != gate) {
      continue;
    }
    for (auto const& root : roots) {
      if (same_directory(run.cwd, root)) {
        return std::optional<unmet_requirement>{};
      }
    }
  }
  return std::optional<unmet_requirement>{
      {requirement_kind::gate_missing,
       "task_gate: no passing queued run of the gate argv is recorded for this claim in its repository root or worktree"}};
}

} // namespace

auto normalize_directory(std::string_view text) -> std::optional<std::filesystem::path> {
  if (text.empty()) {
    return std::nullopt;
  }
  std::filesystem::path path{std::string{text}};
  if (!path.is_absolute()) {
    return std::nullopt;
  }
  std::error_code ec;
  auto            resolved = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    resolved = path.lexically_normal();
  }
  resolved = resolved.lexically_normal();
  if (!resolved.has_filename() && resolved != resolved.root_path()) {
    resolved = resolved.parent_path();
  }
  return resolved;
}

auto same_directory(std::string_view lhs, std::string_view rhs) -> bool {
  auto const a = normalize_directory(lhs);
  auto const b = normalize_directory(rhs);
  return a.has_value() && b.has_value() && *a == *b;
}

auto unmet_completion_requirements(db::connection& conn, std::string_view claim_token,
                                   const completion_requirements& requirements, const gate_run_lookup& lookup)
    -> std::expected<std::vector<unmet_requirement>, agentactivity::agent_error> {
  std::vector<unmet_requirement> unmet;
  if (!requirements.any()) {
    return unmet;
  }
  auto const held = aa::get_claim_by_token(conn, claim_token);
  if (!held) {
    return std::unexpected(held.error());
  }
  if (held->kind != aa::entity_kind::task) {
    return unmet;
  }
  if (requirements.review_required) {
    auto const review = check_review(conn, *held);
    if (!review) {
      return std::unexpected(review.error());
    }
    if (review->has_value()) {
      unmet.push_back(std::move(**review));
    }
  }
  if (requirements.task_gate.has_value()) {
    auto const gate = check_gate(*held, *requirements.task_gate, lookup);
    if (!gate) {
      return std::unexpected(gate.error());
    }
    if (gate->has_value()) {
      unmet.push_back(std::move(**gate));
    }
  }
  return unmet;
}

} // namespace planar::engine::runtime::completion
