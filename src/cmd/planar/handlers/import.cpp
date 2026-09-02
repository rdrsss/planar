/// @file import.cpp
/// @brief Handler for the import staging and deterministic apply path.
module planar.cmd.planar.handlers.importer;

import std;
import planar.cliapp.args;
import planar.json_text;
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
    result->message = std::format("applied: anchor {}; 1 plans created/0 updated, 0 tasks created/0 updated/0 cancelled", *anchor);
  }
  if (flag_bool(args, "--json")) ctx.out() << json(*result, anchor);
  else ctx.out() << result->message << '\n';
  return {};
}
} // namespace planar::cmd::handlers
