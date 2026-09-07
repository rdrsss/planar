/// @file synthesize.cpp
/// @brief Synthesis staging plus transactional planning/workbench composition.
module planar.cmd.planar.handlers.synthesize;

import std;
import planar.cliapp.args;
import planar.json_text;
import planar.json_dom;
import planar.db;
import planar.engine.importer;
import planar.engine.synthesize;
import planar.engine.planning;
import planar.engine.workbench.root;
import planar.engine.workbench.sync;
import planar.engine.workbench.terminal;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {
namespace sy = engine::synthesize;
namespace im = engine::importer;
namespace pl = engine::planning;
namespace {

struct apply_report {
  std::int64_t anchor_plan_id = 0;
  std::size_t  plans_created = 0, plans_updated = 0, plans_abandoned = 0;
  std::size_t  tasks_created = 0, tasks_updated = 0, tasks_cancelled = 0;
  std::size_t  artifacts_created = 0, artifacts_retired = 0;
  std::size_t  decisions_created = 0, decisions_superseded = 0;
};
auto failure(std::string body, domain_error_kind kind = domain_error_kind::generic_failure) -> handler_result {
  return std::unexpected(error_from_body(kind, std::move(body)));
}
auto home_for(context& ctx) -> std::expected<std::filesystem::path, domain_error> {
  if (auto value = ctx.env()("PLANAR_HOME"); value && !value->empty())
    return std::filesystem::path(*value);
  if (auto home = ctx.env()("HOME"); home && !home->empty())
    return std::filesystem::path(*home) / ".planar";
  return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving PLANAR_HOME: HomeNotSet"));
}
auto text(const json_dom::json_value& object, std::string_view key) -> std::optional<std::string> {
  auto const* value = object.find(key);
  return value && value->kind == json_dom::json_kind::string ? std::optional{value->string} : std::nullopt;
}
auto integer(const json_dom::json_value& object, std::string_view key, std::int64_t fallback = 0) -> std::int64_t {
  auto const* value = object.find(key);
  return value && value->kind == json_dom::json_kind::integer ? value->integer : fallback;
}
auto items(const json_dom::json_value& object, std::string_view key) -> std::span<const json_dom::json_value> {
  auto const* value = object.find(key);
  return value && value->kind == json_dom::json_kind::array ? std::span<const json_dom::json_value>{value->array}
                                                            : std::span<const json_dom::json_value>{};
}
auto slugify(std::string_view raw) -> std::string {
  std::string out;
  bool        dash = false;
  for (unsigned char c : raw) {
    auto lc = static_cast<char>(std::tolower(c));
    if (std::isalnum(static_cast<unsigned char>(lc))) {
      out.push_back(lc);
      dash = false;
    } else if (!out.empty() && !dash) {
      out.push_back('-');
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out.empty() ? "synthesized-plan" : out;
}
auto find_plan(db::connection& conn, std::string_view slug, std::optional<std::int64_t> parent)
    -> std::expected<std::optional<std::int64_t>, domain_error> {
  auto stmt =
      conn.prepare("select id from plans where slug=? and ((? is null and parent_plan_id is null) or parent_plan_id=?) limit 1");
  if (!stmt || !stmt->bind_text(1, slug) || !(parent ? stmt->bind_int64(2, *parent) : stmt->bind_null(2)) ||
      !(parent ? stmt->bind_int64(3, *parent) : stmt->bind_null(3)))
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  auto step = stmt->step();
  if (!step)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  return *step == db::step_result::row ? std::optional<std::int64_t>{stmt->column_int64(0)} : std::nullopt;
}
auto upsert_plan(db::connection& conn, std::string title, std::string slug, pl::plan_status status,
                 std::optional<std::int64_t> parent, std::optional<std::string> scope, apply_report& report)
    -> std::expected<std::int64_t, domain_error> {
  auto found = find_plan(conn, slug, parent);
  if (!found)
    return std::unexpected(found.error());
  if (*found) {
    auto stmt = conn.prepare("update plans set title=?,status=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?");
    if (!stmt || !stmt->bind_text(1, title) || !stmt->bind_text(2, pl::plan_status_to_text(status)) ||
        !stmt->bind_int64(3, **found) || !stmt->step())
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    ++report.plans_updated;
    return **found;
  }
  auto created = pl::create_plan(conn, {.title          = std::move(title),
                                        .slug           = std::move(slug),
                                        .status         = status,
                                        .parent_plan_id = parent,
                                        .scope          = std::move(scope)});
  if (!created)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  ++report.plans_created;
  return created->id;
}
auto upsert_task(db::connection& conn, const json_dom::json_value& value, std::int64_t plan, std::optional<std::string> scope,
                 apply_report& report) -> std::expected<std::int64_t, domain_error> {
  auto slug = text(value, "slug"), title = text(value, "title"), body = text(value, "body"), status = text(value, "status");
  if (!slug || !title || !status)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
  auto parsed = pl::task_status_from_text(*status);
  if (!parsed)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
  auto lookup = conn.prepare("select id from tasks where slug=? and plan_id=? limit 1");
  if (!lookup || !lookup->bind_text(1, *slug) || !lookup->bind_int64(2, plan))
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  auto step = lookup->step();
  if (!step)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  auto next     = text(value, "next_action");
  auto priority = integer(value, "priority");
  if (*step == db::step_result::row) {
    auto id   = lookup->column_int64(0);
    auto stmt = conn.prepare(
        "update tasks set title=?,body=?,status=?,priority=?,next_action=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where "
        "id=?");
    if (!stmt || !stmt->bind_text(1, *title) || !stmt->bind_text(2, body.value_or("")) || !stmt->bind_text(3, *status) ||
        !stmt->bind_int64(4, priority) || !(next && !next->empty() ? stmt->bind_text(5, *next) : stmt->bind_null(5)) ||
        !stmt->bind_int64(6, id) || !stmt->step())
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    ++report.tasks_updated;
    return id;
  }
  auto created = pl::create_task(conn, {.title       = *title,
                                        .body        = body && !body->empty() ? body : std::nullopt,
                                        .status      = *parsed,
                                        .priority    = priority,
                                        .plan_id     = plan,
                                        .next_action = next && !next->empty() ? next : std::nullopt,
                                        .slug        = *slug,
                                        .scope       = std::move(scope)});
  if (!created)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  ++report.tasks_created;
  return created->id;
}
auto artifact_kind(std::string_view value) -> pl::artifact_kind {
  return pl::artifact_kind_from_text(value).value_or(pl::artifact_kind::other);
}
auto artifact_kind_for(const std::filesystem::path& path) -> pl::artifact_kind {
  auto const name = path.filename().string();
  if (name == "README.md")
    return pl::artifact_kind::readme;
  if (name.contains("roadmap"))
    return pl::artifact_kind::roadmap;
  if (name.contains("adr"))
    return pl::artifact_kind::adr;
  if (name.contains("test"))
    return pl::artifact_kind::test_spec;
  if (name.contains("tech"))
    return pl::artifact_kind::tech_spec;
  return pl::artifact_kind::other;
}
auto create_artifact(db::connection& conn, std::string title, pl::artifact_kind kind, std::string body, std::string source,
                     std::int64_t plan, std::optional<std::string> scope, apply_report& report)
    -> std::expected<void, domain_error> {
  auto stmt =
      conn.prepare("select a.id from artifacts a join entity_links e on e.from_kind='artifact' and e.from_id=a.id and "
                   "e.to_kind='plan' and e.to_id=? and e.relationship='derives-from' where coalesce(a.source_path,'')=? limit 1");
  if (!stmt || !stmt->bind_int64(1, plan) || !stmt->bind_text(2, source))
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  auto step = stmt->step();
  if (!step)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  if (*step == db::step_result::row)
    return {};
  auto created = pl::create_artifact(conn, {.title       = std::move(title),
                                            .kind        = kind,
                                            .body        = std::move(body),
                                            .source_path = std::move(source),
                                            .plan_id     = plan,
                                            .scope       = std::move(scope)});
  if (!created)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  ++report.artifacts_created;
  return {};
}
auto seed_synthesis_artifacts(db::connection& conn, std::string_view title, std::string_view repo, std::int64_t plan,
                              std::optional<std::string> scope, apply_report& report) -> std::expected<void, domain_error> {
  for (auto const& [kind, source, suffix] :
       std::array{std::tuple{pl::artifact_kind::product_spec, "pl-synthesize://product_spec", "Product Spec"},
                  std::tuple{pl::artifact_kind::tech_spec, "pl-synthesize://tech_spec", "Tech Spec"},
                  std::tuple{pl::artifact_kind::roadmap, "pl-synthesize://roadmap", "Roadmap"}}) {
    auto created = create_artifact(conn, std::format("{} — {}", title, suffix), kind,
                                   std::format("# {} — {}\n\n> Synthesized by pl-synthesize from {}.\n", title, suffix, repo),
                                   source, plan, scope, report);
    if (!created)
      return std::unexpected(created.error());
  }
  return {};
}
auto seed_forward_artifacts(db::connection& conn, std::int64_t plan, std::string_view title, std::string_view slug,
                            std::string_view goal, std::string_view summary, std::optional<std::string> scope,
                            apply_report& report) -> std::expected<void, domain_error> {
  auto refined = summary.empty() ? "To be refined during specification." : summary;
  struct seed {
    pl::artifact_kind kind;
    std::string_view  source, suffix;
    std::string       body;
  };
  std::array<seed, 3> seeds{{
      {pl::artifact_kind::product_spec, "pl-forward-spec://product_spec", "Product Spec",
       std::format("# {} — Product Spec\n\n> Seeded from forward proposal `{}`.\n\n## Goal\n\n{}\n\n## Summary\n\n{}\n", title,
                   slug, goal, refined)},
      {pl::artifact_kind::tech_spec, "pl-forward-spec://tech_spec", "Tech Spec",
       std::format("# {} — Tech Spec\n\n> Seeded from forward proposal `{}`.\n\n## Context\n\n{}\n\n## Design\n\nTo be developed "
                   "during technical planning.\n",
                   title, slug, goal)},
      {pl::artifact_kind::roadmap, "pl-forward-spec://roadmap", "Roadmap",
       std::format("# {} — Roadmap\n\n> Seeded from forward proposal `{}`.\n\n## M1 — Refine and approve\n\n- [ ] Refine product "
                   "and technical specifications.\n- [ ] Review acceptance criteria and implementation sequence.\n",
                   title, slug)},
  }};
  for (auto& item : seeds) {
    auto made = create_artifact(conn, std::format("{} — {}", title, item.suffix), item.kind, std::move(item.body),
                                std::string{item.source}, plan, scope, report);
    if (!made)
      return std::unexpected(made.error());
  }
  return {};
}
auto reconcile(const sy::outcome& staged, db::connection& conn, std::optional<std::string> scope, bool removals,
               std::optional<std::string> accept_spec, std::string_view workbench) -> std::expected<apply_report, domain_error> {
  auto const& root  = *staged.result;
  auto        title = text(root, "anchor_title");
  auto const* specs = root.find("forward_specs");
  if (!title || !specs)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
  apply_report report;
  auto         anchor = upsert_plan(conn, *title, slugify(*title), pl::plan_status::draft, std::nullopt, scope, report);
  if (!anchor)
    return std::unexpected(anchor.error());
  report.anchor_plan_id = *anchor;
  std::set<std::int64_t> kept_plans{*anchor}, kept_tasks, kept_decisions;
  for (auto const& phase : items(root, "phases")) {
    auto slug = text(phase, "slug"), phase_title = text(phase, "title"), status = text(phase, "status");
    auto parsed = status ? pl::plan_status_from_text(*status) : std::nullopt;
    if (!slug || !phase_title || !parsed)
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
    auto plan = upsert_plan(conn, *phase_title, *slug, *parsed, *anchor, scope, report);
    if (!plan)
      return std::unexpected(plan.error());
    kept_plans.insert(*plan);
    for (auto const& task : items(phase, "tasks")) {
      auto id = upsert_task(conn, task, *plan, scope, report);
      if (!id)
        return std::unexpected(id.error());
      kept_tasks.insert(*id);
    }
  }
  auto seeded = seed_synthesis_artifacts(conn, *title, staged.request_.repo_slug, *anchor, scope, report);
  if (!seeded)
    return std::unexpected(seeded.error());
  for (auto const& ref : items(root, "reference_artifacts")) {
    auto path = text(ref, "path"), kind = text(ref, "kind"), ref_title = text(ref, "title");
    if (!path)
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
    auto made = create_artifact(
        conn, ref_title && !ref_title->empty() ? *ref_title : std::filesystem::path(*path).filename().string(),
        artifact_kind(kind.value_or("research")), std::format("Preserved from {} as input material to pl-synthesize.\n", *path),
        *path, *anchor, scope, report);
    if (!made)
      return std::unexpected(made.error());
  }
  for (auto const& decision : items(root, "decisions")) {
    auto dec_title = text(decision, "title"), body = text(decision, "body"), rationale = text(decision, "rationale");
    if (!dec_title)
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
    auto existing = pl::list_decisions(conn, {.plan_id = *anchor});
    if (!existing)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    auto found = std::ranges::find_if(*existing, [&](auto const& row) { return row.title == *dec_title; });
    if (found != existing->end())
      kept_decisions.insert(found->id);
    else {
      auto made = pl::create_decision(conn, {.title     = *dec_title,
                                             .body      = body && !body->empty() ? *body : *dec_title,
                                             .rationale = rationale && !rationale->empty() ? rationale : std::nullopt,
                                             .plan_id   = *anchor,
                                             .scope     = scope});
      if (!made)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      kept_decisions.insert(made->id);
      ++report.decisions_created;
    }
  }
  if (removals) {
    auto cancel = conn.prepare("update tasks set status='cancelled',updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?");
    auto rows   = conn.prepare(
        "select id from tasks where plan_id in (select id from plans where parent_plan_id=?) and status!='cancelled'");
    if (!cancel || !rows || !rows->bind_int64(1, *anchor))
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    while (true) {
      auto step = rows->step();
      if (!step)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      if (*step == db::step_result::done)
        break;
      auto id = rows->column_int64(0);
      if (!kept_tasks.contains(id)) {
        if (!cancel->reset() || !cancel->bind_int64(1, id) || !cancel->step())
          return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
        ++report.tasks_cancelled;
      }
    }
    auto abandon = conn.prepare("update plans set status='abandoned',updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?");
    auto plans   = conn.prepare("select id from plans where parent_plan_id=? and status!='abandoned'");
    if (!abandon || !plans || !plans->bind_int64(1, *anchor))
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    while (true) {
      auto step = plans->step();
      if (!step)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      if (*step == db::step_result::done)
        break;
      auto id = plans->column_int64(0);
      if (!kept_plans.contains(id)) {
        if (!abandon->reset() || !abandon->bind_int64(1, id) || !abandon->step())
          return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
        ++report.plans_abandoned;
      }
    }
    auto retire =
        conn.prepare("update artifacts set status='retired',updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?");
    auto artifacts = conn.prepare(
        "select a.id,coalesce(a.source_path,'') from artifacts a join entity_links e on e.from_kind='artifact' and "
        "e.from_id=a.id and e.to_kind='plan' and e.to_id=? and e.relationship='derives-from' where a.status!='retired'");
    if (!retire || !artifacts || !artifacts->bind_int64(1, *anchor))
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    while (true) {
      auto step = artifacts->step();
      if (!step)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      if (*step == db::step_result::done)
        break;
      auto id     = artifacts->column_int64(0);
      auto source = artifacts->column_text(1);
      bool keep   = source.starts_with("pl-synthesize://") ||
                    std::ranges::any_of(items(root, "reference_artifacts"),
                                        [&](auto const& r) { return text(r, "path").value_or("") == source; });
      if (!keep) {
        if (!retire->reset() || !retire->bind_int64(1, id) || !retire->step())
          return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
        ++report.artifacts_retired;
      }
    }
    auto supersede =
        conn.prepare("update decisions set status='superseded',updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?");
    auto decs = conn.prepare(
        "select d.id from decisions d join entity_links e on e.from_kind='decision' and e.from_id=d.id and e.to_kind='plan' and "
        "e.to_id=? and e.relationship='derives-from' where d.status in ('proposed','accepted')");
    if (!supersede || !decs || !decs->bind_int64(1, *anchor))
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    while (true) {
      auto step = decs->step();
      if (!step)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      if (*step == db::step_result::done)
        break;
      auto id = decs->column_int64(0);
      if (!kept_decisions.contains(id)) {
        if (!supersede->reset() || !supersede->bind_int64(1, id) || !supersede->step())
          return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
        ++report.decisions_superseded;
      }
    }
  }
  if (accept_spec) {
    auto raw   = *accept_spec;
    auto begin = raw.find_first_not_of(" \t\r\n"), end = raw.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos)
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
    raw = raw.substr(begin, end - begin + 1);
    std::set<std::string> selected;
    if (raw == "all")
      for (auto const& spec : specs->array)
        selected.insert(text(spec, "slug").value_or(""));
    else {
      std::string_view rest = raw;
      while (true) {
        auto comma = rest.find(',');
        auto part  = rest.substr(0, comma);
        auto b = part.find_first_not_of(" \t\r\n"), e = part.find_last_not_of(" \t\r\n");
        if (b == std::string_view::npos || !selected.insert(std::string{part.substr(b, e - b + 1)}).second)
          return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
        if (comma == std::string_view::npos)
          break;
        rest.remove_prefix(comma + 1);
      }
    }
    for (auto const& slug : selected)
      if (!std::ranges::any_of(specs->array, [&](auto const& s) { return text(s, "slug").value_or("") == slug; }))
        return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid synthesize arguments"));
    for (auto const& spec : specs->array) {
      auto slug = text(spec, "slug"), spec_title = text(spec, "title");
      if (!slug || !spec_title)
        continue;
      if (!selected.contains(*slug))
        continue;
      auto plan = upsert_plan(conn, *spec_title, *slug, pl::plan_status::draft, std::nullopt, scope, report);
      if (!plan)
        return std::unexpected(plan.error());
      auto seeds = seed_forward_artifacts(conn, *plan, *spec_title, *slug, text(spec, "goal").value_or(""),
                                          text(spec, "summary").value_or(""), scope, report);
      if (!seeds)
        return std::unexpected(seeds.error());
      if (!engine::workbench::sync::push(conn, *plan, workbench, engine::workbench::terminal::mode::failures, false))
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    }
  }
  return report;
}
auto report_json(const sy::outcome& out, const std::optional<apply_report>& applied) -> std::string {
  std::string value =
      std::format("{{\"mode\":{},\"provider\":{},\"repo_slug\":{},\"fingerprint\":{},\"cache_path\":{},\"pending_path\":{},"
                  "\"docs_count\":{},\"guide_files_count\":{},\"tree_entry_count\":{},\"greenfield\":{},\"message\":{}",
                  json_text::json_string(out.mode_ == sy::mode::pending ? "pending" : "cache_hit"),
                  json_text::json_string(sy::provider_name(out.provider_)), json_text::json_string(out.request_.repo_slug),
                  json_text::json_string(out.request_.fingerprint), json_text::json_string(out.cache_path.string()),
                  json_text::json_string(out.pending_path.string()), out.request_.docs.size(), out.request_.guide_files.size(),
                  out.request_.tree_summary.size(), out.request_.greenfield, json_text::json_string(out.message));
  if (applied)
    value += std::format(",\"applied\":{{\"anchor_plan_id\":{},\"plans_created\":{},\"plans_updated\":{},\"plans_abandoned\":{},"
                         "\"tasks_created\":{},\"tasks_updated\":{},\"tasks_cancelled\":{},\"artifacts_created\":{},\"artifacts_"
                         "retired\":{},\"decisions_created\":{},\"decisions_superseded\":{}}}",
                         applied->anchor_plan_id, applied->plans_created, applied->plans_updated, applied->plans_abandoned,
                         applied->tasks_created, applied->tasks_updated, applied->tasks_cancelled, applied->artifacts_created,
                         applied->artifacts_retired, applied->decisions_created, applied->decisions_superseded);
  else
    value += ",\"applied\":null";
  return value + "}\n";
}
auto literal(context& ctx, const cliapp::parsed_args& args, const std::filesystem::path& home, std::string root)
    -> handler_result {
  ctx.err() << "synthesize: --literal mode; delegating to import.\n";
  auto provider = ctx.env()("PLANAR_LLM_PROVIDER").value_or("shell");
  if (provider.empty())
    provider = "shell";
  if (provider != "shell" && provider != "anthropic" && provider != "openai")
    return failure("invalid synthesize --literal arguments", domain_error_kind::invalid_input);
  auto staged = im::run(root, home, false);
  if (!staged)
    return staged.error() == im::error::not_found
               ? failure(std::format("repo-root not found or not a directory: {}", root))
               : failure("invalid synthesize --literal arguments", domain_error_kind::invalid_input);
  std::optional<apply_report> report;
  if (flag_bool(args, "--apply")) {
    auto db = ctx.ensure_db();
    if (!db)
      return std::unexpected(db.error());
    auto txn = (**db).begin_transaction(db::lock_mode::immediate);
    if (!txn)
      return failure("literal delegation failed: QueryFailed");
    apply_report r;
    auto anchor = upsert_plan(**db, staged->request_.anchor_title, slugify(staged->request_.anchor_title), pl::plan_status::draft,
                              std::nullopt, flag_string(args, "--scope"), r);
    if (!anchor)
      return std::unexpected(anchor.error());
    r.anchor_plan_id = *anchor;
    for (std::filesystem::recursive_directory_iterator it(root), end; it != end; ++it)
      if (it->is_regular_file() && it->path().extension() == ".md" && it->path().filename() != "AGENTS.md" &&
          it->path().filename() != "CLAUDE.md") {
        std::ifstream in(it->path(), std::ios::binary);
        std::string   body{std::istreambuf_iterator<char>{in}, {}};
        auto          rel = std::filesystem::relative(it->path(), root).generic_string();
        auto made = create_artifact(**db, it->path().filename().string(), artifact_kind_for(it->path()), std::move(body), rel,
                                    *anchor, flag_string(args, "--scope"), r);
        if (!made)
          return std::unexpected(made.error());
      }
    if (!txn->commit())
      return failure("literal delegation failed: QueryFailed");
    staged->message =
        std::format("applied: anchor {}; {} plans created/{} updated, {} tasks created/{} updated/{} cancelled", r.anchor_plan_id,
                    r.plans_created, r.plans_updated, r.tasks_created, r.tasks_updated, r.tasks_cancelled);
    report = r;
  }
  if (flag_bool(args, "--json")) {
    std::string out = std::format("{{\"mode\":\"skipped\",\"provider\":{},\"repo_slug\":{},\"fingerprint\":{},\"cache_"
                                  "path\":null,\"pending_path\":null,\"message\":{}",
                                  json_text::json_string(provider), json_text::json_string(staged->request_.repo_slug),
                                  json_text::json_string(staged->request_.fingerprint), json_text::json_string(staged->message));
    if (report)
      out += std::format(",\"applied\":{{\"anchor_plan_id\":{},\"plans_created\":{},\"plans_updated\":{},\"plans_abandoned\":0,"
                         "\"tasks_created\":0,\"tasks_updated\":0,\"tasks_cancelled\":0,\"artifacts_created\":{},\"artifacts_"
                         "retired\":0,\"decisions_created\":0,\"decisions_superseded\":0}}",
                         report->anchor_plan_id, report->plans_created, report->plans_updated, report->artifacts_created);
    else
      out += ",\"applied\":null";
    ctx.out() << out << "}\n";
  } else
    ctx.out() << staged->message << '\n';
  return {};
}
} // namespace

auto synthesize(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const root_arg = positional_string(args, "repo-root").value_or("");
  // A relative repo-root (the common `synthesize .` shape) must be joined
  // against the operator's PWD-preserving cwd, not the raw process cwd --
  // a shell keeps a symlink-spelled PWD while getcwd()/std::filesystem::
  // current_path() resolves it (task 6453, plan 351 task 2378 parity).
  auto root  = root_arg.empty() || std::filesystem::path(root_arg).is_absolute() ? root_arg : (ctx.cwd() / root_arg).string();
  bool apply = flag_bool(args, "--apply");
  if ((flag_bool(args, "--apply-removals") && !apply) ||
      (flag_bool(args, "--treat-as-greenfield") && flag_bool(args, "--treat-as-nongreenfield")) ||
      (flag_string(args, "--accept-spec") && flag_bool(args, "--no-forward-specs")))
    return failure("invalid synthesize arguments", domain_error_kind::invalid_input);
  auto home = home_for(ctx);
  if (!home)
    return std::unexpected(home.error());
  if (flag_bool(args, "--literal"))
    return literal(ctx, args, *home, root);
  auto staged = sy::run(root, *home,
                        {.code_layout            = flag_string(args, "--code-layout"),
                         .apply                  = apply,
                         .treat_as_greenfield    = flag_bool(args, "--treat-as-greenfield"),
                         .treat_as_nongreenfield = flag_bool(args, "--treat-as-nongreenfield")},
                        ctx.env());
  if (!staged) {
    if (staged.error() == sy::error::not_found)
      return failure(std::format("repo-root not found or not a directory: {}", root));
    if (staged.error() == sy::error::invalid_input)
      return failure("invalid synthesize arguments", domain_error_kind::invalid_input);
    return failure("synthesize failed: Io");
  }
  std::optional<apply_report> report;
  if (apply) {
    if (staged->mode_ == sy::mode::pending)
      return failure(std::format("repo-root not found or not a directory: {}", root));
    auto db = ctx.ensure_db();
    if (!db)
      return std::unexpected(db.error());
    auto workbench = engine::workbench::root::resolve_root(ctx.env());
    if (!workbench)
      return failure("database apply failed");
    auto txn = (**db).begin_transaction(db::lock_mode::immediate);
    if (!txn)
      return failure("database apply failed");
    auto applied = reconcile(*staged, **db, flag_string(args, "--scope"), flag_bool(args, "--apply-removals"),
                             flag_string(args, "--accept-spec"), *workbench);
    if (!applied)
      return std::unexpected(applied.error());
    if (!txn->commit())
      return failure("database apply failed");
    staged->message = std::format("applied: anchor {}; {} plans created/{} updated, {} tasks created/{} updated/{} cancelled",
                                  applied->anchor_plan_id, applied->plans_created, applied->plans_updated, applied->tasks_created,
                                  applied->tasks_updated, applied->tasks_cancelled);
    report          = *applied;
  }
  if (flag_bool(args, "--json"))
    ctx.out() << report_json(*staged, report);
  else
    ctx.out() << staged->message << '\n';
  return {};
}
} // namespace planar::cmd::handlers
