/// @file brief.cpp
/// @brief Implementation of `planar.cmd.planar_agent.brief`.

module planar.cmd.planar_agent.brief;

import std;
import planar.db;
import planar.json_text;
import planar.engine.config;
import planar.engine.ingest.packet;
import planar.engine.planning;
import planar.engine.runtime.agentactivity;

namespace planar::cmd::agent {

namespace {

namespace pl  = engine::planning;
namespace aa  = engine::runtime::agentactivity;
namespace cfg = engine::config;
namespace pk  = engine::ingest::packet;

constexpr std::size_t k_max_chain_depth = 64;

auto plan_ref_of(const pl::plan& plan) -> brief_plan_ref {
  return brief_plan_ref{
      .id = plan.id, .slug = plan.slug, .title = plan.title, .status = std::string{pl::plan_status_to_text(plan.status)}};
}

auto add_count(brief_counts& counts, std::string_view status, std::int64_t n) -> void {
  if (status == "todo") {
    counts.todo += n;
  } else if (status == "doing") {
    counts.doing += n;
  } else if (status == "blocked") {
    counts.blocked += n;
  } else if (status == "done") {
    counts.done += n;
  } else if (status == "cancelled") {
    counts.cancelled += n;
  }
}

constexpr std::string_view k_plan_tree = "with recursive plan_tree(id) as (select id from plans where id = ? union all "
                                         "select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id) ";

/// Task counts over a plan and its descendants.
auto count_tree(db::connection& conn, std::int64_t plan_id) -> std::expected<brief_counts, brief_error> {
  auto stmt =
      conn.prepare(std::string{k_plan_tree} + "select status, count(*) from tasks where plan_id in (select id from plan_tree) "
                                              "group by status");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(brief_error::query_failed);
  }
  brief_counts counts;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(brief_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    add_count(counts, stmt->column_text(0), stmt->column_int64(1));
  }
  return counts;
}

struct task_info {
  std::optional<std::string> slug;
  std::string                title;
};

/// Slug and title of every task under the plan, keyed by id.
auto load_task_info(db::connection& conn, std::int64_t plan_id) -> std::expected<std::map<std::int64_t, task_info>, brief_error> {
  auto stmt =
      conn.prepare(std::string{k_plan_tree} + "select id, slug, title from tasks where plan_id in (select id from plan_tree)");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(brief_error::query_failed);
  }
  std::map<std::int64_t, task_info> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(brief_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out[stmt->column_int64(0)] =
        task_info{.slug  = stmt->is_null(1) ? std::optional<std::string>{} : std::optional{stmt->column_text(1)},
                  .title = stmt->column_text(2)};
  }
  return out;
}

auto strip_prefix(std::string reason) -> std::string {
  constexpr std::string_view prefix = "excluded by ";
  if (reason.starts_with(prefix)) {
    reason.erase(0, prefix.size());
  }
  return reason;
}

auto optional_text(const std::optional<std::string>& value) -> std::string {
  return value.has_value() ? *value : std::string{};
}

} // namespace

auto shell_quote(std::string_view word) -> std::string {
  std::string out;
  out.reserve(word.size() + 2);
  out += '\'';
  for (auto const c : word) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

auto format_duration(std::int64_t seconds) -> std::string {
  if (seconds <= 0) {
    return "0s";
  }
  if (seconds % 3600 == 0) {
    return std::format("{}h", seconds / 3600);
  }
  if (seconds % 60 == 0) {
    return std::format("{}m", seconds / 60);
  }
  return std::format("{}s", seconds);
}

auto build_protocol(const engine::config::policy_values& policy, std::int64_t lease_ttl_secs) -> brief_protocol {
  brief_protocol out;
  out.lease_ttl_secs       = lease_ttl_secs;
  out.heartbeat_every_secs = lease_ttl_secs / 2;
  auto const token         = shell_quote("<token>");
  auto const text          = shell_quote("<text>");
  out.templates.push_back(
      brief_template{"claim", std::format("planar-agent claim --entity task:<id> --role coder --model {} --ttl {} --worktree {}",
                                          shell_quote("<model>"), lease_ttl_secs, shell_quote("<path>"))});
  out.templates.push_back(brief_template{"heartbeat", std::format("planar-agent heartbeat --claim {}", token)});
  out.templates.push_back(brief_template{"complete", std::format("planar-agent complete --claim {} --summary {}", token, text)});
  out.templates.push_back(brief_template{"fail", std::format("planar-agent fail --claim {} --reason {}", token, text)});
  out.templates.push_back(brief_template{"release", std::format("planar-agent release --claim {} --reason {}", token, text)});
  out.templates.push_back(
      brief_template{"block", std::format("planar-agent block --claim {} --blocker <task-id> --reason {}", token, text)});
  out.templates.push_back(
      brief_template{"review", std::format("planar-agent review --claim {} --verdict approve|request-changes --summary {}",
                                           shell_quote("<reviewed-token>"), text)});
  if (policy.use_queue) {
    std::string gate = std::format("planar-agent queue run --claim {} -- ", token);
    if (policy.task_gate.has_value()) {
      bool first = true;
      for (auto const& word : *policy.task_gate) {
        gate += std::format("{}{}", first ? "" : " ", shell_quote(word));
        first = false;
      }
    } else {
      gate += "<command>";
    }
    out.templates.push_back(brief_template{"gate", std::move(gate)});
  }
  return out;
}

auto gather_brief(db::connection& conn, std::int64_t plan_id, const engine::config::toml_map& config_file)
    -> std::expected<brief, brief_error> {
  auto const plan = pl::show_plan(conn, plan_id);
  if (!plan) {
    return std::unexpected(plan.error() == pl::plan_error::not_found ? brief_error::plan_not_found : brief_error::query_failed);
  }
  brief out;
  out.plan = plan_ref_of(*plan);

  // The anchor is the root of the parent chain; the milestones are its children.
  pl::plan root = *plan;
  for (std::size_t depth = 0; root.parent_plan_id.has_value(); ++depth) {
    if (depth >= k_max_chain_depth) {
      return std::unexpected(brief_error::query_failed);
    }
    auto const parent = pl::show_plan(conn, *root.parent_plan_id);
    if (!parent) {
      return std::unexpected(brief_error::query_failed);
    }
    root = *parent;
  }
  if (root.id != plan->id) {
    out.anchor = plan_ref_of(root);
  }
  auto children = pl::list_plans(
      conn, pl::plan_list_filter{.statuses       = {pl::plan_status::draft, pl::plan_status::active, pl::plan_status::paused,
                                                    pl::plan_status::done, pl::plan_status::abandoned},
                                 .parent_plan_id = root.id});
  if (!children) {
    return std::unexpected(brief_error::query_failed);
  }
  std::ranges::sort(*children, {}, &pl::plan::id);
  for (auto const& child : *children) {
    auto counts = count_tree(conn, child.id);
    if (!counts) {
      return std::unexpected(counts.error());
    }
    out.milestones.push_back(brief_milestone{.plan = plan_ref_of(child), .counts = *counts});
  }

  auto counts = count_tree(conn, plan_id);
  if (!counts) {
    return std::unexpected(counts.error());
  }
  out.counts = *counts;

  auto info = load_task_info(conn, plan_id);
  if (!info) {
    return std::unexpected(info.error());
  }

  // Claims, from the claim-aware bucketing `plan next` and `peek` use.
  auto work = aa::next_work(conn, plan_id);
  if (!work) {
    return std::unexpected(brief_error::query_failed);
  }
  std::map<std::int64_t, aa::next_work_bucket> bucket_of;
  std::map<std::int64_t, brief_claim>          live;
  std::map<std::int64_t, brief_claim>          stale;
  for (auto const& row : *work) {
    bucket_of[row.task_id] = row.bucket;
    if (!row.covering.has_value()) {
      continue;
    }
    auto&      target = row.bucket == aa::next_work_bucket::stale ? stale : live;
    auto const id     = row.covering->id;
    auto       it     = target.find(id);
    if (it == target.end()) {
      auto const& c = *row.covering;
      brief_claim entry;
      entry.claim_id   = c.id;
      entry.entity     = std::format("{}:{}", aa::to_text(c.kind), c.entity_id);
      entry.state      = row.bucket == aa::next_work_bucket::stale ? "stale" : "live";
      entry.role       = c.role;
      entry.model      = c.model;
      entry.worktree   = c.worktree_path;
      entry.vendor     = c.vendor;
      entry.expires_at = c.lease_expires_at;
      it               = target.emplace(id, std::move(entry)).first;
    }
    it->second.covered_tasks.push_back(row.task_id);
  }
  for (auto& [id, entry] : live) {
    std::ranges::sort(entry.covered_tasks);
    out.claims.push_back(std::move(entry));
  }
  for (auto& [id, entry] : stale) {
    std::ranges::sort(entry.covered_tasks);
    out.claims.push_back(std::move(entry));
  }

  // Readiness of every unclaimed todo task, in the order `next_work` ranks them.
  std::map<std::int64_t, std::vector<std::string>> reasons_of;
  for (auto const& row : *work) {
    // A todo task with an unfinished dependency lands in the `blocked` bucket; it is still a todo task
    // whose readiness the brief reports. Only a task a claim covers is left out.
    if (row.status != "todo" || row.bucket == aa::next_work_bucket::claimed || row.bucket == aa::next_work_bucket::stale) {
      continue;
    }
    auto const assembled = pk::assemble_task(conn, row.task_id);
    if (!assembled) {
      return std::unexpected(brief_error::query_failed);
    }
    std::vector<std::string> names;
    for (auto const reason : assembled->reasons) {
      names.emplace_back(pk::reason_name(reason));
    }
    auto const& task = (*info)[row.task_id];
    out.readiness.push_back(brief_readiness{.id = row.task_id, .slug = task.slug, .reasons = names});
    reasons_of[row.task_id] = std::move(names);
  }

  // The wave: the parallelizability rules, then the packet readiness (rule 7) and any covering claim (rule 8).
  auto rec = pl::strategy::recommend(conn, plan_id);
  if (!rec) {
    return std::unexpected(rec.error() == pl::strategy::strategy_error::not_found ? brief_error::plan_not_found
                                                                                  : brief_error::query_failed);
  }
  auto to_ref = [](const pl::strategy::task_ref& task) {
    brief_task_ref ref{.id = task.id, .slug = task.slug, .title = task.title, .why = {}};
    for (auto const& e : task.excluded_by) {
      ref.why.push_back(brief_exclusion{.rule = e.rule, .reason = strip_prefix(e.reason)});
    }
    return ref;
  };
  for (auto const& task : rec->serialized) {
    out.serialized.push_back(to_ref(task));
  }
  for (auto const& task : rec->parallel_eligible) {
    auto ref = to_ref(task);
    auto bk  = bucket_of.find(task.id);
    if (bk != bucket_of.end() && (bk->second == aa::next_work_bucket::claimed || bk->second == aa::next_work_bucket::stale)) {
      ref.why.push_back(brief_exclusion{.rule = 8, .reason = "covered by a claim"});
    } else if (auto const it = reasons_of.find(task.id); it != reasons_of.end() && !it->second.empty()) {
      std::string joined;
      for (auto const& name : it->second) {
        joined += joined.empty() ? name : ", " + name;
      }
      ref.why.push_back(brief_exclusion{.rule = 7, .reason = std::format("packet not ready: {}", joined)});
    }
    if (ref.why.empty()) {
      out.wave.push_back(std::move(ref));
    } else {
      out.serialized.push_back(std::move(ref));
    }
  }
  std::ranges::sort(out.serialized, {}, &brief_task_ref::id);
  out.fan_out_available = out.wave.size() >= 2;

  // The effective policy for the plan's own scope.
  auto const association = cfg::policy_association_for_plan(conn, plan_id);
  if (!association) {
    return std::unexpected(association.error() == cfg::policy_scope_error::not_found ? brief_error::plan_not_found
                                                                                     : brief_error::query_failed);
  }
  out.policy.association  = association->slug;
  out.policy.scope_reason = association->reason;
  std::optional<std::string_view> slug_view;
  if (association->slug.has_value()) {
    slug_view = *association->slug;
  }
  auto resolved = cfg::effective_policy(config_file, slug_view);
  if (resolved) {
    out.policy.resolved = std::move(*resolved);
  } else {
    out.policy.valid    = false;
    out.policy.findings = std::move(resolved.error());
    out.policy.resolved = cfg::resolve_policy_lenient(config_file, slug_view);
  }
  out.policy.digest  = cfg::policy_digest(out.policy.resolved.values);
  out.confirm_before = out.policy.resolved.values.confirm_before;
  out.protocol       = build_protocol(out.policy.resolved.values, aa::default_lease_secs);
  return out;
}

namespace {

auto task_label(std::int64_t id, const std::optional<std::string>& slug) -> std::string {
  return slug.has_value() && !slug->empty() ? std::format("{} {}", id, *slug) : std::to_string(id);
}

auto source_text(const cfg::policy_key_source& source) -> std::string {
  return source.kind == cfg::policy_source_kind::embedded_default ? std::string{"default"} : source.section;
}

auto join(const std::vector<std::string>& words, std::string_view separator) -> std::string {
  std::string out;
  for (auto const& word : words) {
    if (!out.empty()) {
      out += separator;
    }
    out += word;
  }
  return out;
}

auto counts_text(const brief_counts& c) -> std::string {
  return std::format("todo {} doing {} blocked {} done {}{}", c.todo, c.doing, c.blocked, c.done,
                     c.cancelled > 0 ? std::format(" cancelled {}", c.cancelled) : std::string{});
}

} // namespace

auto render_brief_text(const brief& value) -> std::string {
  std::string out;
  out += std::format("brief v{} plan {} {} [{}]\n", brief_version, value.plan.id, value.plan.slug, value.plan.status);
  out += std::format("tasks {}: {}\n", value.counts.total(), counts_text(value.counts));
  if (value.anchor.has_value()) {
    out += std::format("anchor: {} {} [{}]\n", value.anchor->id, value.anchor->slug, value.anchor->status);
  }
  if (!value.milestones.empty()) {
    out += "milestones:";
    for (auto const& m : value.milestones) {
      out += std::format(" {} {} [{}] {}/{};", m.plan.id, m.plan.slug, m.plan.status, m.counts.done, m.counts.total());
    }
    out += '\n';
  }

  out += "claims:";
  if (value.claims.empty()) {
    out += " none\n";
  } else {
    out += '\n';
    for (auto const& c : value.claims) {
      out += std::format("  {} #{} {} role={} model={} worktree={} expires {}\n", c.state, c.claim_id, c.entity,
                         c.role.value_or("-"), c.model.value_or("-"), c.worktree.has_value() ? shell_quote(*c.worktree) : "-",
                         c.expires_at);
    }
  }

  out += std::format("wave ({} parallel):", value.wave.size());
  if (value.wave.empty()) {
    out += " none\n";
  } else {
    for (auto const& t : value.wave) {
      out += std::format(" {};", task_label(t.id, t.slug));
    }
    out += '\n';
  }
  if (!value.serialized.empty()) {
    out += std::format("serialized ({}):\n", value.serialized.size());
    for (auto const& t : value.serialized) {
      std::vector<std::string> reasons;
      for (auto const& why : t.why) {
        reasons.push_back(why.reason);
      }
      out += std::format("  {}: {}\n", task_label(t.id, t.slug), join(reasons, "; "));
    }
  }

  std::size_t unready = 0;
  for (auto const& r : value.readiness) {
    unready += r.reasons.empty() ? 0 : 1;
  }
  out += std::format("ready: {} of {} unclaimed todo", value.readiness.size() - unready, value.readiness.size());
  if (unready > 0) {
    out += "; not ready:\n";
    for (auto const& r : value.readiness) {
      if (!r.reasons.empty()) {
        out += std::format("  {}: {}\n", task_label(r.id, r.slug), join(r.reasons, ", "));
      }
    }
  } else {
    out += '\n';
  }

  out += std::format("policy digest {}; scope {} ({})\n", value.policy.digest,
                     value.policy.association.has_value() ? shell_quote(*value.policy.association) : std::string{"top-level"},
                     cfg::policy_scope_reason_name(value.policy.scope_reason));
  if (!value.policy.valid) {
    out += std::format("  INVALID config: {} finding(s), run `planar config validate`; values fall through per key\n",
                       value.policy.findings.size());
  }
  for (auto const key : cfg::policy_keys) {
    auto const source = value.policy.resolved.sources.find(std::string{key});
    out += std::format("  {}={} [{}]\n", key, cfg::render_policy_value(value.policy.resolved.values, key),
                       source == value.policy.resolved.sources.end() ? std::string{"default"} : source_text(source->second));
  }
  if (!value.confirm_before.empty()) {
    out += std::format("confirm before: {}\n", join(value.confirm_before, ", "));
  }

  out += std::format("protocol (lease {}s, heartbeat every {}):\n", value.protocol.lease_ttl_secs,
                     format_duration(value.protocol.heartbeat_every_secs));
  for (auto const& t : value.protocol.templates) {
    out += std::format("  {}: {}\n", t.name, t.command);
  }
  return out;
}

namespace {

auto key(std::string& out, std::string_view name) -> void {
  json_text::append_json_string(out, name);
  out += ':';
}

auto str(std::string& out, std::string_view text) -> void {
  json_text::append_json_string(out, text);
}

auto opt_str(std::string& out, const std::optional<std::string>& text) -> void {
  if (text.has_value()) {
    str(out, *text);
  } else {
    out += "null";
  }
}

auto plan_json(std::string& out, const brief_plan_ref& p) -> void {
  out += std::format("{{\"id\":{},", p.id);
  key(out, "slug");
  str(out, p.slug);
  out += ',';
  key(out, "title");
  str(out, p.title);
  out += ',';
  key(out, "status");
  str(out, p.status);
  out += '}';
}

auto counts_json(std::string& out, const brief_counts& c) -> void {
  out += std::format("{{\"todo\":{},\"doing\":{},\"blocked\":{},\"done\":{},\"cancelled\":{}}}", c.todo, c.doing, c.blocked,
                     c.done, c.cancelled);
}

auto task_json(std::string& out, const brief_task_ref& t) -> void {
  out += std::format("{{\"id\":{},", t.id);
  key(out, "slug");
  opt_str(out, t.slug);
  out += ',';
  key(out, "title");
  str(out, t.title);
  out += ",\"why\":[";
  bool first = true;
  for (auto const& w : t.why) {
    out += first ? "" : ",";
    first = false;
    out += std::format("{{\"rule\":{},", w.rule);
    key(out, "reason");
    str(out, w.reason);
    out += '}';
  }
  out += "]}";
}

auto strings_json(std::string& out, const std::vector<std::string>& words) -> void {
  out += '[';
  bool first = true;
  for (auto const& word : words) {
    out += first ? "" : ",";
    first = false;
    str(out, word);
  }
  out += ']';
}

} // namespace

auto render_brief_json(const brief& value) -> std::string {
  std::string out;
  out += std::format("{{\"brief_version\":{},", brief_version);
  key(out, "plan");
  plan_json(out, value.plan);
  out += ',';
  key(out, "anchor");
  if (value.anchor.has_value()) {
    plan_json(out, *value.anchor);
  } else {
    out += "null";
  }
  out += ",\"counts\":";
  counts_json(out, value.counts);
  out += ",\"milestones\":[";
  bool first = true;
  for (auto const& m : value.milestones) {
    out += first ? "" : ",";
    first = false;
    out += "{\"plan\":";
    plan_json(out, m.plan);
    out += ",\"counts\":";
    counts_json(out, m.counts);
    out += '}';
  }
  out += "],\"claims\":[";
  first = true;
  for (auto const& c : value.claims) {
    out += first ? "" : ",";
    first = false;
    out += std::format("{{\"claim_id\":{},", c.claim_id);
    key(out, "entity");
    str(out, c.entity);
    out += ',';
    key(out, "state");
    str(out, c.state);
    out += ',';
    key(out, "role");
    opt_str(out, c.role);
    out += ',';
    key(out, "model");
    opt_str(out, c.model);
    out += ',';
    key(out, "worktree");
    opt_str(out, c.worktree);
    out += ',';
    key(out, "vendor");
    str(out, c.vendor);
    out += ',';
    key(out, "expires_at");
    str(out, c.expires_at);
    out += ",\"covered_tasks\":[";
    bool inner = true;
    for (auto const id : c.covered_tasks) {
      out += inner ? "" : ",";
      inner = false;
      out += std::to_string(id);
    }
    out += "]}";
  }
  out += std::format("],\"wave\":{{\"fan_out_available\":{},\"parallel\":[", value.fan_out_available ? "true" : "false");
  first = true;
  for (auto const& t : value.wave) {
    out += first ? "" : ",";
    first = false;
    task_json(out, t);
  }
  out += "],\"serialized\":[";
  first = true;
  for (auto const& t : value.serialized) {
    out += first ? "" : ",";
    first = false;
    task_json(out, t);
  }
  out += "]},\"readiness\":[";
  first = true;
  for (auto const& r : value.readiness) {
    out += first ? "" : ",";
    first = false;
    out += std::format("{{\"id\":{},", r.id);
    key(out, "slug");
    opt_str(out, r.slug);
    out += std::format(",\"ready\":{},\"reasons\":", r.reasons.empty() ? "true" : "false");
    strings_json(out, r.reasons);
    out += '}';
  }
  out += "],\"policy\":{\"association\":";
  opt_str(out, value.policy.association);
  out += ",\"scope_reason\":";
  str(out, cfg::policy_scope_reason_name(value.policy.scope_reason));
  out += std::format(",\"valid\":{},\"findings\":[", value.policy.valid ? "true" : "false");
  first = true;
  for (auto const& f : value.policy.findings) {
    out += first ? "" : ",";
    first = false;
    out += "{\"key\":";
    str(out, f.key);
    out += ",\"message\":";
    str(out, f.message);
    out += '}';
  }
  out += "],\"digest\":";
  str(out, value.policy.digest);
  out += ",\"values\":";
  out += cfg::policy_canonical_json(value.policy.resolved.values);
  out += ",\"sources\":{";
  first = true;
  for (auto const policy_key : cfg::policy_keys) {
    auto const it = value.policy.resolved.sources.find(std::string{policy_key});
    out += first ? "" : ",";
    first = false;
    key(out, policy_key);
    out += "{\"kind\":";
    auto const kind = it == value.policy.resolved.sources.end() ? cfg::policy_source_kind::embedded_default : it->second.kind;
    str(out, kind == cfg::policy_source_kind::embedded_default
                 ? "default"
                 : (kind == cfg::policy_source_kind::top_level ? "top_level" : "association"));
    out += ",\"section\":";
    str(out, it == value.policy.resolved.sources.end() ? std::string_view{} : std::string_view{it->second.section});
    out += '}';
  }
  out += "}},\"confirm_before\":";
  strings_json(out, value.confirm_before);
  out += std::format(",\"protocol\":{{\"lease_ttl_secs\":{},\"heartbeat_every_secs\":{},\"templates\":[",
                     value.protocol.lease_ttl_secs, value.protocol.heartbeat_every_secs);
  first = true;
  for (auto const& t : value.protocol.templates) {
    out += first ? "" : ",";
    first = false;
    out += "{\"name\":";
    str(out, t.name);
    out += ",\"command\":";
    str(out, t.command);
    out += '}';
  }
  out += "]}}\n";
  return out;
}

} // namespace planar::cmd::agent
