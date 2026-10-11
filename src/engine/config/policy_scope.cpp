/// @file policy_scope.cpp
/// @brief Implementation of `planar.engine.config.policy_scope`.

module planar.engine.config.policy_scope;

import std;
import planar.db;

namespace planar::engine::config {

auto policy_scope_reason_name(policy_scope_reason reason) -> std::string_view {
  switch (reason) {
  case policy_scope_reason::global_scope:
    return "global_scope";
  case policy_scope_reason::association_scope:
    return "association_scope";
  case policy_scope_reason::project_membership:
    return "project_membership";
  case policy_scope_reason::project_unassociated:
    return "project_unassociated";
  case policy_scope_reason::project_ambiguous:
    return "project_ambiguous";
  }
  return "global_scope";
}

auto policy_association_for_scope(db::connection& conn, std::string_view scope_kind, std::optional<std::int64_t> scope_id)
    -> std::expected<policy_association, policy_scope_error> {
  if (scope_kind == "global") {
    return policy_association{.slug = std::nullopt, .reason = policy_scope_reason::global_scope};
  }
  if (!scope_id.has_value()) {
    return std::unexpected(policy_scope_error::not_found);
  }
  if (scope_kind == "association") {
    auto stmt = conn.prepare("select slug from associations where id = ?");
    if (!stmt || !stmt->bind_int64(1, *scope_id)) {
      return std::unexpected(policy_scope_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(policy_scope_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(policy_scope_error::not_found);
    }
    return policy_association{.slug = stmt->column_text(0), .reason = policy_scope_reason::association_scope};
  }
  if (scope_kind == "repo") {
    auto exists = conn.prepare("select 1 from projects where id = ?");
    if (!exists || !exists->bind_int64(1, *scope_id)) {
      return std::unexpected(policy_scope_error::query_failed);
    }
    auto found = exists->step();
    if (!found) {
      return std::unexpected(policy_scope_error::query_failed);
    }
    if (*found != db::step_result::row) {
      return std::unexpected(policy_scope_error::not_found);
    }
    auto stmt = conn.prepare("select a.slug from project_associations pa join associations a on a.id = pa.association_id "
                             "where pa.project_id = ? order by a.slug limit 2");
    if (!stmt || !stmt->bind_int64(1, *scope_id)) {
      return std::unexpected(policy_scope_error::query_failed);
    }
    std::vector<std::string> slugs;
    for (;;) {
      auto step = stmt->step();
      if (!step) {
        return std::unexpected(policy_scope_error::query_failed);
      }
      if (*step != db::step_result::row) {
        break;
      }
      slugs.push_back(stmt->column_text(0));
    }
    if (slugs.empty()) {
      return policy_association{.slug = std::nullopt, .reason = policy_scope_reason::project_unassociated};
    }
    if (slugs.size() > 1) {
      return policy_association{.slug = std::nullopt, .reason = policy_scope_reason::project_ambiguous};
    }
    return policy_association{.slug = std::move(slugs.front()), .reason = policy_scope_reason::project_membership};
  }
  return std::unexpected(policy_scope_error::not_found);
}

auto policy_association_for_plan(db::connection& conn, std::int64_t plan_id)
    -> std::expected<policy_association, policy_scope_error> {
  auto stmt = conn.prepare("select scope_kind, scope_id from plans where id = ?");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(policy_scope_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(policy_scope_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(policy_scope_error::not_found);
  }
  auto const kind = stmt->column_text(0);
  auto const id   = stmt->is_null(1) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt->column_int64(1)};
  return policy_association_for_scope(conn, kind, id);
}

} // namespace planar::engine::config
