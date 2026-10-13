/// @file completion_policy.cpp
/// @brief Implementation of `planar.cmd.planar_agent.completion_policy`.

module planar.cmd.planar_agent.completion_policy;

import std;
import planar.db;
import planar.engine.config;
import planar.engine.hostqueue;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.completion;
import planar.cmd.planar_agent.brief;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

namespace aa   = engine::runtime::agentactivity;
namespace comp = engine::runtime::completion;
namespace cfg  = engine::config;
namespace hq   = engine::hostqueue;

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

/// @brief Read and parse the config file; a missing file is an empty document.
/// @param path The located path, or unset.
/// @return The document, or the reason it could not be read.
auto load_config(const std::optional<std::filesystem::path>& path) -> std::expected<cfg::toml_map, std::string> {
  if (!path.has_value()) {
    return cfg::toml_map{};
  }
  std::error_code ec;
  if (!std::filesystem::exists(*path, ec)) {
    return cfg::toml_map{};
  }
  std::ifstream in(*path, std::ios::binary);
  if (!in) {
    return std::unexpected(std::format("cannot read {}", path->string()));
  }
  std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  auto        parsed = cfg::parse_toml(content);
  if (!parsed) {
    return std::unexpected(std::format("{}: line {}: {}", path->string(), parsed.error().line, parsed.error().message));
  }
  return std::move(*parsed);
}

/// @brief The association whose policy table applies to the claim's task: the task's plan decides, and a
/// task without a plan falls back to its own scope.
/// @param conn The connection.
/// @param task_id The claim's task.
/// @return The association slug (unset for top-level policy only), or `query_failed`.
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

auto history_gate_lookup() -> engine::runtime::completion::gate_run_lookup {
  return [](db::connection& conn, std::string_view claim_token) -> std::expected<std::vector<comp::gate_run>, aa::agent_error> {
    auto const rows = hq::history_for_claim(conn, claim_token);
    if (!rows) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    std::vector<comp::gate_run> runs;
    runs.reserve(rows->size());
    for (auto const& row : *rows) {
      runs.push_back(comp::gate_run{.argv   = row.argv,
                                    .cwd    = row.cwd,
                                    .passed = row.outcome == hq::history_outcome::exited && row.exit_code.has_value() &&
                                              *row.exit_code == 0});
    }
    return runs;
  };
}

auto completion_check(std::optional<std::filesystem::path> config_path, completion_report* report)
    -> std::function<std::expected<void, engine::runtime::agentactivity::agent_error>(
        db::connection&, const engine::runtime::agentactivity::claim&)> {
  return [path = std::move(config_path), report](db::connection&  conn,
                                                 const aa::claim& held) -> std::expected<void, aa::agent_error> {
    if (held.kind != aa::entity_kind::task) {
      return {};
    }
    auto const doc = load_config(path);
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
    auto const association = task_association(conn, held.entity_id);
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
    auto const unmet = comp::unmet_completion_requirements(conn, held.claim_token, requirements, history_gate_lookup());
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
      body += std::format("\n  to record the gate run, run from the claim's repository root or worktree: planar-agent queue run "
                          "--claim {} -- {}",
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
