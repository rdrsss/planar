/// @file claim_policy.cpp
/// @brief Implementation of `planar.cmd.planar_agent.claim_policy`.

module planar.cmd.planar_agent.claim_policy;

import std;
import planar.db;
import planar.engine.config;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

namespace aa     = engine::runtime::agentactivity;
namespace atomic = engine::runtime::agentatomic;
namespace cfg    = engine::config;

namespace {

/// @brief Whether the flattened config carries a per-association policy table.
/// @param doc The parsed config file.
/// @return True when an `associations.<slug>.policy.*` key exists.
auto has_association_policy(const cfg::toml_map& doc) -> bool {
  return std::ranges::any_of(doc, [](const auto& entry) {
    std::string_view const key = entry.first;
    return key.starts_with("associations.") && key.contains(".policy.");
  });
}

/// @brief The association of a plan's own scope; a plan that does not exist has top-level policy only.
/// @param conn The connection.
/// @param plan_id The plan.
/// @return The slug, or `query_failed`.
auto plan_association(db::connection& conn, std::int64_t plan_id) -> std::expected<std::optional<std::string>, aa::agent_error> {
  auto const found = cfg::policy_association_for_plan(conn, plan_id);
  if (!found) {
    if (found.error() == cfg::policy_scope_error::not_found) {
      return std::optional<std::string>{};
    }
    return std::unexpected(aa::agent_error::query_failed);
  }
  return found->slug;
}

/// @brief The association whose policy applies to a task: its plan decides, and a task without a plan
/// falls back to its own scope.
/// @param conn The connection.
/// @param task_id The task.
/// @return The slug (unset for top-level policy only), or `query_failed`.
auto task_association(db::connection& conn, std::int64_t task_id) -> std::expected<std::optional<std::string>, aa::agent_error> {
  if (auto const plan_id = aa::task_plan_id(conn, task_id); plan_id.has_value()) {
    auto const found = cfg::policy_association_for_plan(conn, *plan_id);
    if (!found) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    return found->slug;
  }
  auto stmt = conn.prepare("select scope_kind, scope_id from tasks where id = ?");
  if (!stmt || !stmt->bind_int64(1, task_id)) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  auto const stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::optional<std::string>{};
  }
  auto const kind  = stmt->column_text(0);
  auto const id    = stmt->is_null(1) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt->column_int64(1)};
  auto const found = cfg::policy_association_for_scope(conn, kind, id);
  if (!found) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  return found->slug;
}

/// @brief The association whose policy applies to a plan_step: its plan's.
/// @param conn The connection.
/// @param step_id The step.
/// @return The slug (unset for top-level policy only), or `query_failed`.
auto step_association(db::connection& conn, std::int64_t step_id) -> std::expected<std::optional<std::string>, aa::agent_error> {
  auto stmt = conn.prepare("select plan_id from plan_steps where id = ?");
  if (!stmt || !stmt->bind_int64(1, step_id)) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  auto const stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(aa::agent_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::optional<std::string>{};
  }
  return plan_association(conn, stmt->column_int64(0));
}

} // namespace

auto policy_association_for_entity(db::connection& conn, engine::runtime::agentactivity::entity_kind kind, std::int64_t entity_id)
    -> std::expected<std::optional<std::string>, engine::runtime::agentactivity::agent_error> {
  switch (kind) {
  case aa::entity_kind::task:
    return task_association(conn, entity_id);
  case aa::entity_kind::plan:
    return plan_association(conn, entity_id);
  case aa::entity_kind::plan_step:
    return step_association(conn, entity_id);
  }
  return std::optional<std::string>{};
}

auto claim_policy_decision(std::optional<std::filesystem::path> config_path, std::string verb, claim_policy_report* report)
    -> std::function<std::expected<std::optional<std::string>, engine::runtime::agentactivity::agent_error>(
        db::connection&, const engine::runtime::agentatomic::claim_request&)> {
  return [path = std::move(config_path), verb = std::move(verb),
          report](db::connection&              conn,
                  const atomic::claim_request& request) -> std::expected<std::optional<std::string>, aa::agent_error> {
    bool const coder = request.role.has_value() && *request.role == "coder";

    // A policy that cannot be applied records no digest. It refuses a coder claim, which a
    // `require_model` in the broken file might bind, and lets every other claim through.
    auto const unapplicable = [&](const std::string& problem) -> std::expected<std::optional<std::string>, aa::agent_error> {
      if (!coder) {
        return std::optional<std::string>{};
      }
      if (report != nullptr) {
        report->text = std::format("{}: the policy in config.toml cannot be applied: {}\n"
                                   "  fix the file, then check it with: planar config validate",
                                   verb, problem);
      }
      return std::unexpected(aa::agent_error::claim_policy_unmet);
    };

    auto const doc = path.has_value() ? cfg::load_toml_file(*path) : std::expected<cfg::toml_map, std::string>{cfg::toml_map{}};
    if (!doc) {
      return unapplicable(doc.error());
    }
    // Only a per-association table makes the scope matter; without one, no database row is read.
    std::optional<std::string> slug;
    if (has_association_policy(*doc)) {
      auto const association = policy_association_for_entity(conn, request.kind, request.entity_id);
      if (!association) {
        return std::unexpected(association.error());
      }
      slug = *association;
    }
    auto const resolved =
        cfg::effective_policy(*doc, slug.has_value() ? std::optional<std::string_view>{std::string_view{*slug}} : std::nullopt);
    if (!resolved) {
      std::string problem;
      for (auto const& finding : resolved.error()) {
        problem += problem.empty() ? "" : "; ";
        problem += std::format("{}: {}", finding.key, finding.message);
      }
      return unapplicable(problem);
    }
    if (coder && resolved->values.require_model && (!request.model.has_value() || request.model->empty())) {
      if (report != nullptr) {
        report->text = std::format("{}: policy require_model = true: coder claims must say which model they use "
                                   "(add --model '<id>')",
                                   verb);
      }
      return std::unexpected(aa::agent_error::claim_policy_unmet);
    }
    return std::optional<std::string>{cfg::policy_digest(resolved->values)};
  };
}

auto claim_policy_refusal(const claim_policy_report& report) -> domain_error {
  auto err = error_from_body(domain_error_kind::generic_failure, report.text);
  err.tag  = "PolicyUnmet";
  return err;
}

} // namespace planar::cmd::agent
