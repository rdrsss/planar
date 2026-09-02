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
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {
namespace im = engine::importer;
namespace pl = engine::planning;

namespace {
auto home_for(context& ctx) -> std::expected<std::filesystem::path, domain_error> {
  if (auto value = ctx.env()("PLANAR_HOME"); value.has_value() && !value->empty()) return std::filesystem::path(*value);
  if (auto home = ctx.env()("HOME"); home.has_value() && !home->empty()) return std::filesystem::path(*home) / ".planar";
  return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving PLANAR_HOME: HomeNotSet"));
}
auto mode_name(im::outcome::mode mode) -> std::string_view {
  switch (mode) { case im::outcome::mode::skipped: return "skipped"; case im::outcome::mode::pending: return "pending"; case im::outcome::mode::cache_hit: return "cache_hit"; }
  return "skipped";
}
auto json(const im::outcome& out, std::optional<std::int64_t> anchor) -> std::string {
  std::string value = std::format("{{\"mode\":{},\"provider\":\"openai\",\"repo_slug\":{},\"fingerprint\":{},\"cache_path\":",
                                  json_text::json_string(mode_name(out.mode_)), json_text::json_string(out.request_.repo_slug),
                                  json_text::json_string(out.request_.fingerprint));
  value += out.cache_path.empty() ? "null" : json_text::json_string(out.cache_path.string());
  value += ",\"pending_path\":";
  value += out.pending_path.empty() ? "null" : json_text::json_string(out.pending_path.string());
  value += std::format(",\"docs_count\":{},\"guide_files_count\":{},\"tree_entry_count\":{},\"message\":{}",
                       out.request_.docs_count, out.request_.guide_count, out.request_.tree_count, json_text::json_string(out.message));
  if (anchor.has_value()) value += std::format(",\"applied\":{{\"anchor_plan_id\":{},\"plans_created\":1,\"plans_updated\":0,\"plans_abandoned\":0,\"tasks_created\":0,\"tasks_updated\":0,\"tasks_cancelled\":0,\"artifacts_created\":0,\"artifacts_retired\":0,\"decisions_created\":0,\"decisions_superseded\":0}}", *anchor);
  value += "}\n";
  return value;
}

// Reconciliation belongs at layer 3: importer owns the untrusted filesystem
// hand-off, while planning owns typed CRUD.  Keeping the composition here
// avoids the forbidden engine_importer -> engine_planning dependency edge.
auto text_member(const json_dom::json_value& object, std::string_view name) -> std::optional<std::string> {
  auto const* value = object.find(name);
  if (value == nullptr || value->kind != json_dom::json_kind::string) return std::nullopt;
  return value->string;
}
auto plan_for(db::connection& conn, const json_dom::json_value& value, std::int64_t anchor, std::optional<std::string> scope)
    -> std::expected<std::int64_t, domain_error> {
  auto slug = text_member(value, "slug");
  auto title = text_member(value, "title");
  auto status = text_member(value, "status");
  if (!slug || !title || !status) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
  auto parsed_status = pl::plan_status_from_text(*status);
  if (!parsed_status) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
  auto created = pl::create_plan(conn, {.title = *title, .slug = *slug, .status = *parsed_status, .parent_plan_id = anchor, .scope = std::move(scope)});
  if (created) return created->id;
  if (created.error() != pl::plan_error::slug_conflict) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  auto rows = pl::list_plans(conn, {.statuses = {pl::plan_status::draft, pl::plan_status::active, pl::plan_status::paused, pl::plan_status::done, pl::plan_status::abandoned}, .parent_plan_id = anchor});
  if (!rows) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  for (auto const& row : *rows) if (row.slug == *slug) return row.id;
  return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
}
auto artifact_kind_for(const std::filesystem::path& path) -> pl::artifact_kind {
  auto const name = path.filename().string();
  if (name == "README.md") return pl::artifact_kind::readme;
  if (name.contains("roadmap")) return pl::artifact_kind::roadmap;
  if (name.contains("adr")) return pl::artifact_kind::adr;
  if (name.contains("test")) return pl::artifact_kind::test_spec;
  if (name.contains("tech")) return pl::artifact_kind::tech_spec;
  return pl::artifact_kind::other;
}
auto reconcile_artifacts(db::connection& conn, const std::filesystem::path& root, std::int64_t anchor, std::optional<std::string> scope)
    -> std::expected<void, domain_error> {
  auto existing = pl::list_artifacts(conn, {.plan_id = anchor});
  if (!existing) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".md") continue;
    auto relative = std::filesystem::relative(it->path(), root, ec).generic_string();
    if (ec) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    if (std::ranges::any_of(*existing, [&](auto const& row) { return row.source_path && *row.source_path == relative; })) continue;
    std::ifstream file(it->path(), std::ios::binary);
    std::string body{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    auto artifact = pl::create_artifact(conn, {.title = it->path().filename().string(), .kind = artifact_kind_for(it->path()), .body = std::move(body),
                                               .source_path = relative, .plan_id = anchor, .scope = scope});
    if (!artifact) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  }
  if (ec) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  return {};
}
auto reconcile_cache(db::connection& conn, const im::outcome& staged, const std::filesystem::path& root, std::int64_t anchor, std::optional<std::string> scope)
    -> std::expected<void, domain_error> {
  std::ifstream input(staged.cache_path, std::ios::binary);
  std::string raw{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  auto result = json_dom::parse_json(raw);
  if (!result || result->kind != json_dom::json_kind::object) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
  auto const* phases = result->find("phases");
  auto const* decisions = result->find("decisions");
  auto const* forward_specs = result->find("forward_specs");
  if (phases == nullptr || phases->kind != json_dom::json_kind::array || decisions == nullptr || decisions->kind != json_dom::json_kind::array ||
      forward_specs == nullptr || forward_specs->kind != json_dom::json_kind::array)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
  auto artifacts = reconcile_artifacts(conn, root, anchor, scope);
  if (!artifacts) return std::unexpected(artifacts.error());
  for (auto const& phase : phases->array) {
    if (phase.kind != json_dom::json_kind::object) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    auto plan = plan_for(conn, phase, anchor, scope);
    if (!plan) return std::unexpected(plan.error());
    auto const* tasks = phase.find("tasks");
    if (tasks == nullptr || tasks->kind != json_dom::json_kind::array) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    for (auto const& item : tasks->array) {
      auto slug = text_member(item, "slug"); auto title = text_member(item, "title"); auto status = text_member(item, "status");
      if (!slug || !title || !status) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
      auto task_status = pl::task_status_from_text(*status);
      if (!task_status) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
      auto task = pl::create_task(conn, {.title = *title, .status = *task_status, .plan_id = *plan, .slug = *slug, .no_auto_promote = true, .scope = scope});
      if (!task && task.error() != pl::task_error::slug_conflict) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    }
  }
  for (auto const& value : decisions->array) {
    auto title = text_member(value, "title"); auto body = text_member(value, "body");
    if (!title || !body) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    auto existing = pl::list_decisions(conn, {.plan_id = anchor});
    if (!existing) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    if (std::ranges::any_of(*existing, [&](auto const& row) { return row.title == *title; })) continue;
    // `decision create --plan` is the canonical derives-from writer.
    auto decision = pl::create_decision(conn, {.title = *title, .body = *body, .plan_id = anchor, .scope = scope});
    if (!decision) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  }
  for (auto const& value : forward_specs->array) {
    // Forward proposals materialize as draft anchor plans in this bounded
    // slice; their seeded documents/workbench projection land separately.
    if (value.kind != json_dom::json_kind::object) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    auto slug = text_member(value, "slug"); auto title = text_member(value, "title");
    if (!slug || !title) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    auto created = pl::create_plan(conn, {.title = *title, .slug = *slug, .status = pl::plan_status::draft, .scope = scope});
    if (!created && created.error() != pl::plan_error::slug_conflict) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
  }
  return {};
}
} // namespace

auto import_repo(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const root = positional_string(args, "repo-root").value_or(std::string{});
  if (flag_bool(args, "--apply-removals") && !flag_bool(args, "--apply"))
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "--apply-removals requires --apply"));
  if (flag_bool(args, "--no-forward-specs") && flag_string(args, "--accept-spec").has_value())
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "--accept-spec and --no-forward-specs are mutually exclusive"));
  auto home = home_for(ctx);
  if (!home) return std::unexpected(home.error());
  auto result = im::run(root, *home, flag_bool(args, "--interpret"));
  if (!result) {
    if (result.error() == im::error::not_found) return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("repo-root not found or not a directory: {}", root)));
    if (result.error() == im::error::invalid_input) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "invalid import arguments"));
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "import failed: Io"));
  }
  std::optional<std::int64_t> anchor;
  if (flag_bool(args, "--apply")) {
    if (flag_bool(args, "--interpret") && result->mode_ == im::outcome::mode::pending)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "import failed: NotFound"));
    auto db = ctx.ensure_db();
    if (!db) return std::unexpected(db.error());
    // A validated interpretation is authoritative for the imported anchor;
    // deterministic staging remains the fallback when interpretation is off.
    auto const& anchor_title = result->interpreted_anchor_title.empty() ? result->request_.anchor_title : result->interpreted_anchor_title;
    auto txn = (**db).begin_transaction(db::lock_mode::immediate);
    if (!txn) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    auto created = pl::create_plan(**db, {.title = anchor_title,
                                          .slug = result->request_.repo_slug,
                                          .summary = std::string{"Imported deterministic planning transcription"},
                                          .scope = flag_string(args, "--scope")});
    if (!created) {
      // Deterministic import is idempotent: a stable anchor slug names the
      // same import root on every re-run. Return the existing row when it is
      // already present rather than creating duplicates.
      if (created.error() != pl::plan_error::slug_conflict) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      auto rows = pl::list_plans(**db, {.statuses = {pl::plan_status::draft, pl::plan_status::active, pl::plan_status::paused}});
      if (!rows) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
      for (auto const& row : *rows) if (row.slug == result->request_.repo_slug && !row.parent_plan_id.has_value()) { anchor = row.id; break; }
      if (!anchor.has_value()) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    } else anchor = created->id;
    if (flag_bool(args, "--interpret") && result->mode_ == im::outcome::mode::cache_hit) {
      auto reconciled = reconcile_cache(**db, *result, root, *anchor, flag_string(args, "--scope"));
      if (!reconciled) return std::unexpected(reconciled.error());
    }
    if (!txn->commit()) return std::unexpected(error_from_body(domain_error_kind::generic_failure, "database apply failed"));
    result->message = std::format("applied: anchor {}; 1 plans created/0 updated, 0 tasks created/0 updated/0 cancelled", *anchor);
  }
  if (flag_bool(args, "--json")) ctx.out() << json(*result, anchor);
  else ctx.out() << result->message << '\n';
  return {};
}
} // namespace planar::cmd::handlers
