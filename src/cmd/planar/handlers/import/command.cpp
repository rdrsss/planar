/// @file import.cpp
/// @brief Handler for the import staging and deterministic apply path.
module planar.cmd.planar.handlers.importer;

import std;
import planar.cliapp.args;
import planar.json_text;
import planar.json_dom;
import planar.db;
import planar.engine.importer;
import planar.engine.planning;
import planar.engine.workbench.root;
import planar.engine.workbench.sync;
import planar.engine.workbench.terminal;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {
namespace im = engine::importer;
namespace pl = engine::planning;

namespace {
auto home_for(context& ctx) -> std::expected<std::filesystem::path, domain_error> {
  if (auto value = ctx.env()("PLANAR_HOME"); value.has_value() && !value->empty())
    return std::filesystem::path(*value);
  if (auto home = ctx.env()("HOME"); home.has_value() && !home->empty())
    return std::filesystem::path(*home) / ".planar";
  return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving PLANAR_HOME: HomeNotSet"));
}
auto mode_name(im::outcome::mode mode) -> std::string_view {
  switch (mode) {
  case im::outcome::mode::skipped:
    return "skipped";
  case im::outcome::mode::pending:
    return "pending";
  case im::outcome::mode::cache_hit:
    return "cache_hit";
  }
  return "skipped";
}
/// Real reconciliation counters accumulated while applying an import/synthesize
/// cache payload. These feed both the human-readable message and the JSON
/// `applied` block -- previously both were hardcoded stubs that never
/// reflected the actual database mutations (task 6453).
struct apply_counts {
  int plans_created        = 0;
  int plans_updated        = 0;
  int plans_abandoned      = 0;
  int tasks_created        = 0;
  int tasks_updated        = 0;
  int tasks_cancelled      = 0;
  int artifacts_created    = 0;
  int artifacts_retired    = 0;
  int decisions_created    = 0;
  int decisions_superseded = 0;
};
auto json(const im::outcome& out, std::optional<std::int64_t> anchor, const apply_counts& counts) -> std::string {
  std::string value = std::format("{{\"mode\":{},\"provider\":\"openai\",\"repo_slug\":{},\"fingerprint\":{},\"cache_path\":",
                                  json_text::json_string(mode_name(out.mode_)), json_text::json_string(out.request_.repo_slug),
                                  json_text::json_string(out.request_.fingerprint));
  value += out.cache_path.empty() ? "null" : json_text::json_string(out.cache_path.string());
  value += ",\"pending_path\":";
  value += out.pending_path.empty() ? "null" : json_text::json_string(out.pending_path.string());
  value +=
      std::format(",\"docs_count\":{},\"guide_files_count\":{},\"tree_entry_count\":{},\"message\":{}", out.request_.docs_count,
                  out.request_.guide_count, out.request_.tree_count, json_text::json_string(out.message));
  if (anchor.has_value())
    value += std::format(",\"applied\":{{\"anchor_plan_id\":{},\"plans_created\":{},\"plans_updated\":{},\"plans_abandoned\":{},"
                         "\"tasks_created\":{},\"tasks_updated\":{},\"tasks_cancelled\":{},\"artifacts_created\":{},\"artifacts_"
                         "retired\":{},\"decisions_created\":{},\"decisions_superseded\":{}}}",
                         *anchor, counts.plans_created, counts.plans_updated, counts.plans_abandoned, counts.tasks_created,
                         counts.tasks_updated, counts.tasks_cancelled, counts.artifacts_created, counts.artifacts_retired,
                         counts.decisions_created, counts.decisions_superseded);
  value += "}\n";
  return value;
}

// Reconciliation belongs at layer 3: importer owns the untrusted filesystem
// hand-off, while planning owns typed CRUD.  Keeping the composition here
// avoids the forbidden engine_importer -> engine_planning dependency edge.
auto text_member(const json_dom::json_value& object, std::string_view name) -> std::optional<std::string> {
  auto const* value = object.find(name);
  if (value == nullptr || value->kind != json_dom::json_kind::string)
    return std::nullopt;
  return value->string;
}
auto plan_for(db::connection& conn, const json_dom::json_value& value, std::int64_t anchor, std::optional<std::string> scope,
              apply_counts& counts) -> std::expected<std::int64_t, domain_error> {
  auto slug   = text_member(value, "slug");
  auto title  = text_member(value, "title");
  auto status = text_member(value, "status");
  if (!slug || !title || !status)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
  auto parsed_status = pl::plan_status_from_text(*status);
  if (!parsed_status)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
  auto created = pl::create_plan(
      conn, {.title = *title, .slug = *slug, .status = *parsed_status, .parent_plan_id = anchor, .scope = std::move(scope)});
  if (created) {
    ++counts.plans_created;
    return created->id;
  }
  if (created.error() != pl::plan_error::slug_conflict)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  auto rows = pl::list_plans(conn, {.statuses       = {pl::plan_status::draft, pl::plan_status::active, pl::plan_status::paused,
                                                       pl::plan_status::done, pl::plan_status::abandoned},
                                    .parent_plan_id = anchor});
  if (!rows)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  for (auto const& row : *rows)
    if (row.slug == *slug)
      return row.id;
  return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
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
auto reconcile_artifacts(db::connection& conn, const std::filesystem::path& root, std::int64_t anchor,
                         std::optional<std::string> scope, bool apply_removals, apply_counts& counts)
    -> std::expected<void, domain_error> {
  auto existing = pl::list_artifacts(conn, {.plan_id = anchor});
  if (!existing)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".md")
      continue;
    auto relative = std::filesystem::relative(it->path(), root, ec).generic_string();
    if (ec)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    if (std::ranges::any_of(*existing, [&](auto const& row) { return row.source_path && *row.source_path == relative; }))
      continue;
    std::ifstream file(it->path(), std::ios::binary);
    std::string   body{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    auto          artifact = pl::create_artifact(conn, {.title       = it->path().filename().string(),
                                                        .kind        = artifact_kind_for(it->path()),
                                                        .body        = std::move(body),
                                                        .source_path = relative,
                                                        .plan_id     = anchor,
                                                        .scope       = scope});
    if (!artifact)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    ++counts.artifacts_created;
  }
  if (ec)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  // Retire only source-backed artifacts that disappeared from this import
  // root.  The status preserves the audit trail; removal never deletes rows.
  if (apply_removals)
    for (auto const& row : *existing) {
      if (!row.source_path || std::filesystem::exists(root / *row.source_path))
        continue;
      auto retired = conn.prepare("update artifacts set status='retired', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where "
                                  "id=? and status!='retired'");
      if (!retired || !retired->bind_int64(1, row.id) || !retired->step())
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      // `list_artifacts` with no --status filter already narrows to
      // {draft, active} (see artifact.cppm), so every row reaching here
      // genuinely transitions to retired.
      ++counts.artifacts_retired;
    }
  return {};
}
auto seed_forward_artifacts(db::connection& conn, std::int64_t plan, std::string_view title, std::string_view slug,
                            std::string_view goal, std::string_view summary, std::optional<std::string> scope)
    -> std::expected<void, domain_error> {
  struct seed {
    pl::artifact_kind kind;
    std::string_view  source;
    std::string       suffix;
    std::string       body;
  };
  auto const          refined = summary.empty() ? "To be refined during specification." : summary;
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
  auto                existing = pl::list_artifacts(conn, {.plan_id = plan});
  if (!existing)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  for (auto const& item : seeds) {
    if (std::ranges::any_of(*existing, [&](auto const& row) { return row.source_path && *row.source_path == item.source; }))
      continue;
    auto created = pl::create_artifact(conn, {.title       = std::format("{} — {}", title, item.suffix),
                                              .kind        = item.kind,
                                              .body        = item.body,
                                              .source_path = std::string{item.source},
                                              .plan_id     = plan,
                                              .scope       = scope});
    if (!created)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  }
  return {};
}
auto reconcile_cache(db::connection& conn, const im::outcome& staged, const std::filesystem::path& root, std::int64_t anchor,
                     std::optional<std::string> scope, bool apply_removals, std::optional<std::string> accept_spec,
                     bool no_forward_specs, std::string_view workbench_root, apply_counts& counts)
    -> std::expected<void, domain_error> {
  std::ifstream input(staged.cache_path, std::ios::binary);
  std::string   raw{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  auto          result = json_dom::parse_json(raw);
  if (!result || result->kind != json_dom::json_kind::object)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
  auto const* phases        = result->find("phases");
  auto const* decisions     = result->find("decisions");
  auto const* forward_specs = result->find("forward_specs");
  if (phases == nullptr || phases->kind != json_dom::json_kind::array || decisions == nullptr ||
      decisions->kind != json_dom::json_kind::array || forward_specs == nullptr ||
      forward_specs->kind != json_dom::json_kind::array)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
  auto artifacts = reconcile_artifacts(conn, root, anchor, scope, apply_removals, counts);
  if (!artifacts)
    return std::unexpected(artifacts.error());
  std::set<std::int64_t> kept_plans;
  std::set<std::int64_t> kept_tasks;
  for (auto const& phase : phases->array) {
    if (phase.kind != json_dom::json_kind::object)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    auto plan = plan_for(conn, phase, anchor, scope, counts);
    if (!plan)
      return std::unexpected(plan.error());
    kept_plans.insert(*plan);
    auto const* tasks = phase.find("tasks");
    if (tasks == nullptr || tasks->kind != json_dom::json_kind::array)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    for (auto const& item : tasks->array) {
      auto slug   = text_member(item, "slug");
      auto title  = text_member(item, "title");
      auto status = text_member(item, "status");
      if (!slug || !title || !status)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
      auto task_status = pl::task_status_from_text(*status);
      if (!task_status)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
      auto task = pl::create_task(
          conn,
          {.title = *title, .status = *task_status, .plan_id = *plan, .slug = *slug, .no_auto_promote = true, .scope = scope});
      if (task) {
        kept_tasks.insert(task->id);
        ++counts.tasks_created;
      } else if (task.error() == pl::task_error::slug_conflict) {
        auto found = conn.prepare("select id from tasks where plan_id=? and slug=? limit 1");
        if (!found || !found->bind_int64(1, *plan) || !found->bind_text(2, *slug))
          return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
        auto stepped = found->step();
        if (!stepped || *stepped != db::step_result::row)
          return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
        kept_tasks.insert(found->column_int64(0));
      } else if (task.error() == pl::task_error::busy_source)
        // Task 6908: bucket a post-timeout SQLITE_BUSY the same way the
        // task verbs do, not with the generic failure every other engine
        // error here shares -- an orchestrator can retry a busy import
        // apply, so it should see `busy_source`, not `generic_failure`.
        return std::unexpected(error_from_body(domain_error_kind::busy_source, "database apply failed: Busy"));
      else
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    }
  }
  if (apply_removals) {
    auto rows = conn.prepare(
        "select id from tasks where plan_id in (select id from plans where parent_plan_id=?) and status!='cancelled'");
    if (!rows || !rows->bind_int64(1, anchor))
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    while (true) {
      auto stepped = rows->step();
      if (!stepped)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      if (*stepped == db::step_result::done)
        break;
      auto const id = rows->column_int64(0);
      if (kept_tasks.contains(id))
        continue;
      auto cancelled =
          conn.prepare("update tasks set status='cancelled', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?");
      if (!cancelled || !cancelled->bind_int64(1, id) || !cancelled->step())
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      ++counts.tasks_cancelled;
    }
    auto plans = conn.prepare("select id from plans where parent_plan_id=? and status!='abandoned'");
    if (!plans || !plans->bind_int64(1, anchor))
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    while (true) {
      auto stepped = plans->step();
      if (!stepped)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      if (*stepped == db::step_result::done)
        break;
      auto const id = plans->column_int64(0);
      if (kept_plans.contains(id))
        continue;
      auto abandoned =
          conn.prepare("update plans set status='abandoned', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?");
      if (!abandoned || !abandoned->bind_int64(1, id) || !abandoned->step())
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      ++counts.plans_abandoned;
    }
  }
  std::set<std::int64_t> kept_decisions;
  for (auto const& value : decisions->array) {
    auto title = text_member(value, "title");
    auto body  = text_member(value, "body");
    if (!title || !body)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    auto existing = pl::list_decisions(conn, {.plan_id = anchor});
    if (!existing)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    auto matched = std::ranges::find_if(*existing, [&](auto const& row) { return row.title == *title; });
    if (matched != existing->end()) {
      kept_decisions.insert(matched->id);
      continue;
    }
    // `decision create --plan` is the canonical derives-from writer.
    auto decision = pl::create_decision(conn, {.title = *title, .body = *body, .plan_id = anchor, .scope = scope});
    if (!decision)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    kept_decisions.insert(decision->id);
    ++counts.decisions_created;
  }
  if (apply_removals) {
    auto rows = conn.prepare(
        "select d.id from decisions d join entity_links e on e.from_kind='decision' and e.from_id=d.id and e.to_kind='plan' and "
        "e.to_id=? and e.relationship='derives-from' where d.status in ('proposed','accepted')");
    if (!rows || !rows->bind_int64(1, anchor))
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    while (true) {
      auto stepped = rows->step();
      if (!stepped)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      if (*stepped == db::step_result::done)
        break;
      auto const id = rows->column_int64(0);
      if (kept_decisions.contains(id))
        continue;
      auto superseded =
          conn.prepare("update decisions set status='superseded', updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') where id=?");
      if (!superseded || !superseded->bind_int64(1, id) || !superseded->step())
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      ++counts.decisions_superseded;
    }
  }
  if (no_forward_specs)
    return {};
  std::set<std::string> accepted;
  // Zig's forward-spec selector is opt-in: absence preserves the deferred
  // interactive phase and materializes nothing in a non-interactive call.
  if (!accept_spec)
    return {};
  if (*accept_spec == "all") {
    for (auto const& value : forward_specs->array) {
      auto slug = text_member(value, "slug");
      if (!slug)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
      accepted.insert(*slug);
    }
  } else {
    std::string_view rest = *accept_spec;
    while (!rest.empty()) {
      auto comma = rest.find(',');
      auto slug  = rest.substr(0, comma);
      if (slug.empty() || !accepted.insert(std::string{slug}).second)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
      if (comma == std::string_view::npos)
        break;
      rest.remove_prefix(comma + 1);
    }
  }
  for (auto const& value : forward_specs->array) {
    // Forward proposals materialize as draft anchor plans in this bounded
    // slice; their seeded documents/workbench projection land separately.
    if (value.kind != json_dom::json_kind::object)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    auto slug    = text_member(value, "slug");
    auto title   = text_member(value, "title");
    auto goal    = text_member(value, "goal").value_or("");
    auto summary = text_member(value, "summary").value_or("");
    if (!slug || !title)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    if (!accepted.contains(*slug))
      continue;
    auto created = pl::create_plan(conn, {.title = *title, .slug = *slug, .status = pl::plan_status::draft, .scope = scope});
    if (!created && created.error() != pl::plan_error::slug_conflict)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    if (created)
      ++counts.plans_created;
    std::int64_t plan_id = created ? created->id : 0;
    if (!created) {
      auto rows = pl::list_plans(conn, {.statuses = {pl::plan_status::draft, pl::plan_status::active, pl::plan_status::paused,
                                                     pl::plan_status::done, pl::plan_status::abandoned}});
      if (!rows)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      for (auto const& row : *rows)
        if (!row.parent_plan_id && row.slug == *slug) {
          plan_id = row.id;
          break;
        }
    }
    if (plan_id == 0)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    auto seeded = seed_forward_artifacts(conn, plan_id, *title, *slug, goal, summary, scope);
    if (!seeded)
      return std::unexpected(seeded.error());
    if (!engine::workbench::sync::push(conn, plan_id, workbench_root, engine::workbench::terminal::mode::failures, false))
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  }
  return {};
}
} // namespace

auto import_repo(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const root_arg = positional_string(args, "repo-root").value_or(std::string{});
  // A relative repo-root (the common `import .` shape) must be joined
  // against the operator's PWD-preserving cwd, not the raw process cwd --
  // a shell keeps a symlink-spelled PWD while getcwd()/std::filesystem::
  // current_path() resolves it, and `im::run` reports whatever spelling it
  // is handed verbatim (task 6453, plan 351 task 2378 parity).
  auto const root =
      root_arg.empty() || std::filesystem::path(root_arg).is_absolute() ? root_arg : (ctx.cwd() / root_arg).string();
  if (flag_bool(args, "--apply-removals") && !flag_bool(args, "--apply"))
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "--apply-removals requires --apply"));
  if (flag_bool(args, "--no-forward-specs") && flag_string(args, "--accept-spec").has_value())
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "--accept-spec and --no-forward-specs are mutually exclusive"));
  auto home = home_for(ctx);
  if (!home)
    return std::unexpected(home.error());
  auto result = im::run(root, *home, flag_bool(args, "--interpret"));
  if (!result) {
    if (result.error() == im::error::not_found)
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("repo-root not found or not a directory: {}", root)));
    if (result.error() == im::error::invalid_input)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "import failed: Io"));
  }
  std::optional<std::int64_t> anchor;
  apply_counts                counts;
  if (flag_bool(args, "--apply")) {
    if (flag_bool(args, "--interpret") && result->mode_ == im::outcome::mode::pending)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "import failed: NotFound"));
    auto db = ctx.db().ensure_db();
    if (!db)
      return std::unexpected(db.error());
    // A validated interpretation is authoritative for the imported anchor;
    // deterministic staging remains the fallback when interpretation is off.
    auto const& anchor_title =
        result->interpreted_anchor_title.empty() ? result->request_.anchor_title : result->interpreted_anchor_title;
    auto txn = (**db).begin_transaction(db::lock_mode::immediate);
    if (!txn)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    auto created = pl::create_plan(**db, {.title   = anchor_title,
                                          .slug    = result->request_.repo_slug,
                                          .summary = std::string{"Imported deterministic planning transcription"},
                                          .scope   = flag_string(args, "--scope")});
    if (!created) {
      // Deterministic import is idempotent: a stable anchor slug names the
      // same import root on every re-run. Return the existing row when it is
      // already present rather than creating duplicates.
      if (created.error() != pl::plan_error::slug_conflict)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      auto rows = pl::list_plans(**db, {.statuses = {pl::plan_status::draft, pl::plan_status::active, pl::plan_status::paused}});
      if (!rows)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      for (auto const& row : *rows)
        if (row.slug == result->request_.repo_slug && !row.parent_plan_id.has_value()) {
          anchor = row.id;
          break;
        }
      if (!anchor.has_value())
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    } else {
      anchor = created->id;
      ++counts.plans_created;
    }
    auto workbench_root = engine::workbench::root::resolve_root(ctx.env());
    if (!workbench_root)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    if (flag_bool(args, "--interpret") && result->mode_ == im::outcome::mode::cache_hit) {
      auto reconciled =
          reconcile_cache(**db, *result, root, *anchor, flag_string(args, "--scope"), flag_bool(args, "--apply-removals"),
                          flag_string(args, "--accept-spec"), flag_bool(args, "--no-forward-specs"), *workbench_root, counts);
      if (!reconciled)
        return std::unexpected(reconciled.error());
    }
    if (!txn->commit())
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    result->message = std::format("applied: anchor {}; {} plans created/{} updated, {} tasks created/{} updated/{} cancelled",
                                  *anchor, counts.plans_created, counts.plans_updated, counts.tasks_created, counts.tasks_updated,
                                  counts.tasks_cancelled);
  }
  if (flag_bool(args, "--json"))
    ctx.out() << json(*result, anchor, counts);
  else
    ctx.out() << result->message << '\n';
  return {};
}
/// @brief Declare the `import` leaf.
///
/// The local is named `importer`, not `import`: a line beginning with
/// the token `import` is a module-import directive, and naming the
/// variable after its verb invites a future edit to move it to the
/// start of a line.
auto declare_import(CLI::App& root) -> void {
  CLI::App* importer = root.add_subcommand(
      "import", "import translates the planning artefacts of an existing\n  repository into Planar's data model. It discovers\n  "
                "tech specs, roadmap milestones, ADRs, and backlog files,\n  infers completion status from checkbox state and "
                "git history,\n  and produces an ImportPlan for review before committing.");
  add_bool(*importer, "--from-github", "Pull source from GitHub issues");
  add_bool(*importer, "--dry-run");
  add_bool(*importer, "--strict");
  add_string(*importer, "--roadmap", "Path to a roadmap source");
  add_bool(*importer, "--apply");
  add_bool(*importer, "--apply-removals");
  add_bool(*importer, "--no-status-inference");
  add_bool(*importer, "--interpret");
  add_string(*importer, "--accept-spec", "Non-interactive forward-spec selection — slug, comma-separated slugs, or 'all'");
  add_bool(*importer, "--no-forward-specs", "Skip forward-spec processing entirely");
  add_string(*importer, "--scope");
  add_json(*importer);
  add_positional(*importer, "repo-root");
}

} // namespace planar::cmd::handlers
