/// @file completion_policy.cpp
/// @brief Implementation of `planar.cmd.planar_agent.completion_policy`.

module planar.cmd.planar_agent.completion_policy;

import std;
import planar.buildqueue.path;
import planar.buildqueue.reader;
import planar.db;
import planar.engine.config;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.completion;
import planar.cmd.planar_agent.brief;
import planar.cmd.planar_agent.claim_policy;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

namespace aa   = engine::runtime::agentactivity;
namespace comp = engine::runtime::completion;
namespace cfg  = engine::config;
namespace bq   = buildqueue;

namespace {

/// @brief Whether the flattened config carries any policy table, top level or per association.
/// @param doc The parsed config file.
/// @return True when a `policy.*` or `associations.<slug>.policy.*` key exists.
auto has_policy_tables(const cfg::toml_map& doc) -> bool {
  return std::ranges::any_of(doc, [](const auto& entry) {
    std::string_view const key = entry.first;
    return key.starts_with("policy.") || key.contains(".policy.");
  });
}

/// @brief Join clauses with `; `.
/// @param unmet The unmet requirements.
/// @return The joined text.
auto join_clauses(const std::vector<comp::unmet_requirement>& unmet) -> std::string {
  std::string out;
  for (auto const& item : unmet) {
    out += out.empty() ? "" : "; ";
    out += item.text;
  }
  return out;
}

} // namespace

auto history_gate_lookup(const internal::env_lookup& env) -> engine::runtime::completion::gate_run_lookup {
  auto path = bq::resolve_queue_db_path(env);
  return [path = std::move(path)](std::string_view tag) -> std::expected<std::vector<comp::gate_run>, std::string> {
    if (!path) {
      return std::unexpected(bq::error_message(path.error()));
    }
    auto const read = bq::read_tagged_runs(*path, tag);
    if (!read) {
      return std::unexpected(read.error().reason);
    }
    std::vector<comp::gate_run> runs;
    runs.reserve(read->runs.size());
    for (auto const& run : read->runs) {
      runs.push_back(comp::gate_run{.argv   = run.argv,
                                    .cwd    = run.cwd,
                                    .passed = run.outcome == "exited" && run.exit_code.has_value() && *run.exit_code == 0});
    }
    return runs;
  };
}

auto completion_check(std::optional<std::filesystem::path> config_path, completion_report* report, internal::env_lookup env)
    -> std::function<std::expected<void, engine::runtime::agentactivity::agent_error>(
        db::connection&, const engine::runtime::agentactivity::claim&)> {
  return [path = std::move(config_path), report,
          env  = std::move(env)](db::connection& conn, const aa::claim& held) -> std::expected<void, aa::agent_error> {
    if (held.kind != aa::entity_kind::task) {
      return {};
    }
    auto const doc = path.has_value() ? cfg::load_toml_file(*path) : std::expected<cfg::toml_map, std::string>{cfg::toml_map{}};
    if (!doc) {
      if (report != nullptr) {
        report->claim_token    = held.claim_token;
        report->config_problem = doc.error();
      }
      return std::unexpected(aa::agent_error::completion_unmet);
    }
    // No policy table, no policy: nothing below reads the database.
    if (!has_policy_tables(*doc)) {
      return {};
    }
    auto const association = policy_association_for_entity(conn, aa::entity_kind::task, held.entity_id);
    if (!association) {
      return std::unexpected(association.error());
    }
    std::optional<std::string_view> slug;
    if (association->has_value()) {
      slug = std::string_view{**association};
    }
    auto const resolved = cfg::effective_policy(*doc, slug);
    if (!resolved) {
      if (report != nullptr) {
        report->claim_token = held.claim_token;
        for (auto const& finding : resolved.error()) {
          report->config_problem += report->config_problem.empty() ? "" : "; ";
          report->config_problem += std::format("{}: {}", finding.key, finding.message);
        }
      }
      return std::unexpected(aa::agent_error::completion_unmet);
    }
    comp::completion_requirements const requirements{.review_required = resolved->values.review_required,
                                                     .task_gate       = resolved->values.task_gate};
    auto const unmet = comp::unmet_completion_requirements(conn, held.claim_token, requirements, history_gate_lookup(env));
    if (!unmet) {
      return std::unexpected(unmet.error());
    }
    if (unmet->empty()) {
      return {};
    }
    if (report != nullptr) {
      report->claim_token = held.claim_token;
      report->unmet       = *unmet;
      report->task_gate   = requirements.task_gate;
    }
    return std::unexpected(aa::agent_error::completion_unmet);
  };
}

auto completion_refusal(const completion_report& report) -> domain_error {
  auto const  short_token = std::string_view{report.claim_token}.substr(0, 8);
  auto const  token       = shell_quote(report.claim_token);
  std::string body;
  if (!report.config_problem.empty()) {
    body = std::format("complete: claim {} cannot complete: the policy in config.toml cannot be applied: {}\n"
                       "  fix the file, then check it with: planar config validate",
                       short_token, report.config_problem);
  } else {
    body             = std::format("complete: claim {} cannot complete: {}", short_token, join_clauses(report.unmet));
    bool review_hint = false;
    bool gate_hint   = false;
    bool locality    = false;
    for (auto const& item : report.unmet) {
      switch (item.kind) {
      case comp::requirement_kind::review_missing:
      case comp::requirement_kind::review_changes:
      case comp::requirement_kind::review_not_independent:
        review_hint = true;
        break;
      case comp::requirement_kind::gate_missing:
        gate_hint = true;
        break;
      case comp::requirement_kind::gate_no_locality:
        locality = true;
        break;
      case comp::requirement_kind::gate_unreadable:
        break;
      }
    }
    if (review_hint) {
      body += std::format("\n  to record an approving review, run from a different session: planar-agent review --claim {} "
                          "--verdict approve --summary {} --reviewer-claim {}",
                          token, shell_quote("<text>"), shell_quote("<other-token>"));
    }
    if (gate_hint && report.task_gate.has_value()) {
      std::string command;
      for (auto const& word : *report.task_gate) {
        command += command.empty() ? "" : " ";
        command += shell_quote(word);
      }
      body += std::format("\n  to record the gate run, run from the claim's repository root or worktree: planar-build-queue run "
                          "--tag {} -- {}",
                          token, command);
    }
    if (locality) {
      body += "\n  the claim has no recorded location: release it and claim the task again from inside the repository, "
              "without --no-locality-probe";
    }
  }
  auto err = error_from_body(domain_error_kind::generic_failure, std::move(body));
  err.tag  = "PolicyUnmet";
  return err;
}

} // namespace planar::cmd::agent
