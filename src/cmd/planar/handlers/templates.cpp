/// @file templates.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.templates` (plan
/// 996, task 6190). See templates.cppm for the two-bucket split, the
/// `$HOME`-not-`$PLANAR_HOME` seam, and the flags two leaves ignore.

module planar.cmd.planar.handlers.templates;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.config.effective;
import planar.engine.config.templates;
import planar.engine.config.templates_embed;
import planar.json_dom;
import planar.engine.templates;
import planar.cmd.planar.cli_log;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace cfg  = engine::config;
namespace tmpl = engine::templates;
namespace jd   = planar::json_dom;

namespace {

/// @brief Expand a leading `~` against `$HOME`.
///
/// A local copy rather than a call into
/// `planar.engine.workbench.root`'s `expand_tilde`, which is the same ten
/// lines. Reusing it would put a `cmd_planar -> engine_workbench` link edge
/// in the graph whose entire justification is a tilde, and would name the
/// WORKBENCH bucket in a templates code path — the sort of edge that reads
/// as a real dependency to the next person to walk the graph. The oracle
/// keeps two copies for the same reason (`handlers/templates/common.zig`
/// has its own beside the workbench's).
///
/// `$HOME` and NOT `$PLANAR_HOME`. See templates.cppm.
/// @param path The configured path.
/// @param env The environment lookup.
/// @return The expanded path, or nullopt when `~` was used and `$HOME` is
/// unset or empty.
auto expand_home(std::string_view path, const env_lookup& env) -> std::optional<std::string> {
  if (path.empty()) {
    return std::string{};
  }
  if (path == "~" || path.starts_with("~/")) {
    auto const home = env("HOME");
    if (!home.has_value() || home->empty()) {
      return std::nullopt;
    }
    if (path == "~") {
      return *home;
    }
    return (std::filesystem::path{*home} / path.substr(2)).string();
  }
  return std::string{path};
}

/// @brief Resolve the operator's templates directory.
///
/// `$PLANAR_TEMPLATES_DIR` > `[templates] dir` in the config file >
/// the embedded default, then tilde-expanded.
///
/// Only ONE variable is forwarded into the config layer's `env_view`, and
/// that is exact rather than lazy: `templates.dir` has exactly one env
/// override, `PLANAR_TEMPLATES_DIR` (see `effective.cpp`'s `pick_str` call
/// for the key). Forwarding the whole process environment via
/// `env_view::from_process()` would reach `std::getenv` behind `ctx.env()`
/// and make every test in this family sensitive to the developer's own
/// shell — the exact hermeticity hole `env_view::empty()` exists to close.
/// @param ctx The invocation context.
/// @return The resolved root, or the exit-1 refusal.
auto resolve_templates_root(context& ctx) -> std::expected<std::string, domain_error> {
  auto const cfg_path = resolve_config_path(ctx.env());

  std::optional<std::string> file_content;
  if (cfg_path.has_value()) {
    std::ifstream file(*cfg_path, std::ios::binary);
    if (file) {
      file_content = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    // A missing file is not a failure — everything falls through to the
    // embedded defaults.
  }

  std::map<std::string, std::string, std::less<>> vars;
  if (auto const raw = ctx.env()("PLANAR_TEMPLATES_DIR"); raw.has_value() && !raw->empty()) {
    vars.emplace("PLANAR_TEMPLATES_DIR", *raw);
  }

  std::optional<std::string_view> content_view;
  if (file_content.has_value()) {
    content_view = std::string_view{*file_content};
  }
  auto resolved = cfg::resolve(content_view, cfg::env_view{std::move(vars)}, std::nullopt);
  if (!resolved.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving templates root: ParseFailed"));
  }

  auto expanded = expand_home(resolved->cfg.templates.dir, ctx.env());
  if (!expanded.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving templates root: HomeNotSet"));
  }
  return *expanded;
}

/// @brief Read the three required positionals shared by `show`, `render`
/// and `validate`.
struct triple {
  std::string set_name; ///< `<set>`.
  std::string system;   ///< `<system>`.
  std::string kind;     ///< `<kind>`.
};

/// @brief Harvest `<set> <system> <kind>`.
///
/// All three are declared required in the CLI tree, so an absent one is
/// unreachable through the parser; the empty-string fallback keeps this
/// total rather than defaulting to something that would silently resolve a
/// DIFFERENT template.
/// @param args The parsed arguments.
/// @return The three identifiers.
auto harvest_triple(const cliapp::parsed_args& args) -> triple {
  return triple{
      .set_name = cliapp::positional_string(args, "set").value_or(""),
      .system   = cliapp::positional_string(args, "system").value_or(""),
      .kind     = cliapp::positional_string(args, "kind").value_or(""),
  };
}

/// @brief Load a template through the resolution chain, mapping the
/// loader's failure onto the oracle's refusal.
/// @param root The templates root.
/// @param ids The requested triple.
/// @return The resolved template, or the exit-1 `not found` refusal.
auto load_or_refuse(std::string_view root, const triple& ids) -> std::expected<cfg::template_entry, domain_error> {
  auto loaded = cfg::load_template(ids.set_name, ids.system, ids.kind, root);
  if (!loaded.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::not_found, tmpl::not_found_message(ids.set_name, ids.system, ids.kind)));
  }
  return *loaded;
}

/// @brief Decode a resolved template's raw bytes into the render DOM.
///
/// This should be UNREACHABLE. The loader proved these exact bytes parse
/// before letting the candidate win its resolution level, and since task
/// 6190 it proves it with `json_dom::parse_json` — the very call below.
/// Two different parsers here is what made the duplicate-key divergence
/// possible in the first place; there is now one.
///
/// It is still checked rather than asserted, because "unreachable" is a
/// claim about today's loader and a silent `*decoded` on a failed parse
/// would be a use of a default-constructed value rather than a refusal.
/// @param entry The resolved template.
/// @return The decoded tree, or the exit-1 refusal.
auto decode_or_refuse(const cfg::template_entry& entry) -> std::expected<jd::json_value, domain_error> {
  auto decoded = jd::parse_json(entry.raw);
  if (!decoded.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("templates: {}: InvalidJson", entry.path)));
  }
  return *decoded;
}

/// @brief Map a `config::template_entry` onto this family's `list_row`.
///
/// The one place the two layer-2 buckets' vocabularies meet. See
/// templates.cppm.
/// @param entries The loader's entries.
/// @return The rows.
auto to_rows(std::span<const cfg::template_entry> entries) -> std::vector<tmpl::list_row> {
  std::vector<tmpl::list_row> rows;
  rows.reserve(entries.size());
  for (auto const& e : entries) {
    rows.push_back({.set_name = e.set_name,
                    .system   = e.system,
                    .kind     = e.kind,
                    .source   = e.source_ == cfg::template_source::disk ? "disk" : "embedded",
                    .path     = e.path});
  }
  return rows;
}

/// @brief A flag's value as a filter, where EMPTY means "no filter".
/// @param args The parsed arguments.
/// @param name The flag name.
/// @param storage Backing store for the returned view.
/// @return The filter, or nullopt.
auto filter_flag(const cliapp::parsed_args& args, std::string_view name, std::optional<std::string>& storage)
    -> std::optional<std::string_view> {
  storage = cliapp::flag_string(args, name);
  if (!storage.has_value()) {
    return std::nullopt;
  }
  return std::string_view{*storage};
}

} // namespace

auto templates_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto root = resolve_templates_root(ctx);
  if (!root.has_value()) {
    return std::unexpected(root.error());
  }

  auto const disk     = to_rows(cfg::list_disk_entries(*root));
  auto const embedded = to_rows(cfg::list_embedded_entries());

  std::optional<std::string> system_store;
  std::optional<std::string> set_store;
  auto const                 rows =
      tmpl::merge_list_rows(disk, embedded, filter_flag(args, "--system", system_store), filter_flag(args, "--set", set_store));

  ctx.out() << (cliapp::flag_bool(args, "--json") ? tmpl::list_json(rows) : tmpl::list_text(rows));
  return {};
}

auto templates_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto root = resolve_templates_root(ctx);
  if (!root.has_value()) {
    return std::unexpected(root.error());
  }
  auto const ids   = harvest_triple(args);
  auto       entry = load_or_refuse(*root, ids);
  if (!entry.has_value()) {
    return std::unexpected(entry.error());
  }
  // RAW bytes, never a DOM round trip — see templates.cppm. `--json` is
  // declared, accepted, and changes nothing.
  ctx.out() << tmpl::show_text(entry->raw);
  return {};
}

auto templates_render(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto root = resolve_templates_root(ctx);
  if (!root.has_value()) {
    return std::unexpected(root.error());
  }
  auto const ids   = harvest_triple(args);
  auto       entry = load_or_refuse(*root, ids);
  if (!entry.has_value()) {
    return std::unexpected(entry.error());
  }
  auto decoded = decode_or_refuse(*entry);
  if (!decoded.has_value()) {
    return std::unexpected(decoded.error());
  }

  // Parse `--entity <kind>:<id>`. Both refusals are exit 2 and both name
  // the offending text back to the operator; captured from the oracle.
  auto const ref   = cliapp::flag_string(args, "--entity").value_or("");
  auto const colon = ref.find(':');
  if (colon == std::string::npos) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("--entity must be kind:id (got '{}')", ref)));
  }
  auto const kind    = std::string_view{ref}.substr(0, colon);
  auto const id_text = std::string_view{ref}.substr(colon + 1);
  auto const id      = cliapp::parse_int64_zig(id_text);
  if (!id.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("--entity id must be an integer (got '{}')", id_text)));
  }

  // The entity kind is validated BEFORE the database is opened, so a typo
  // refuses identically with or without a usable database.
  auto const known = kind == "task" || kind == "plan" || kind == "scenario" || kind == "test_scenario";
  if (!known) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("unsupported entity kind '{}' (use task, plan, scenario)", kind)));
  }

  auto conn = ctx.ensure_db();
  if (!conn.has_value()) {
    return std::unexpected(conn.error());
  }

  auto built = [&] {
    if (kind == "task") {
      return tmpl::build_task_context(**conn, *id);
    }
    if (kind == "plan") {
      return tmpl::build_plan_context(**conn, *id);
    }
    return tmpl::build_scenario_context(**conn, *id);
  }();
  if (!built.has_value()) {
    auto const noun = kind == "plan" ? "plan" : (kind == "task" ? "task" : "scenario");
    switch (built.error()) {
    case tmpl::builder_error::not_found:
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("building {} context: NotFound", noun)));
    case tmpl::builder_error::anchor_plan_not_found:
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, std::format("building {} context: AnchorPlanNotFound", noun)));
    case tmpl::builder_error::query_failed:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("building {} context: QueryFailed", noun)));
    }
  }

  auto rendered = tmpl::render_template(*decoded, *built);
  if (!rendered.has_value()) {
    // This is the arm the reproduced 128-byte defect exits through:
    // `error: rendering template: OutOfMemory`, exit 1. See
    // `planar.engine.templates`'s CMakeLists.
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("rendering template: {}", tmpl::error_name(rendered.error()))));
  }

  ctx.out() << jd::stringify_indent2(*rendered) << "\n";
  return {};
}

auto templates_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto root = resolve_templates_root(ctx);
  if (!root.has_value()) {
    return std::unexpected(root.error());
  }
  auto const ids   = harvest_triple(args);
  auto       entry = load_or_refuse(*root, ids);
  if (!entry.has_value()) {
    return std::unexpected(entry.error());
  }
  auto decoded = decode_or_refuse(*entry);
  if (!decoded.has_value()) {
    return std::unexpected(decoded.error());
  }

  auto const as_json = cliapp::flag_bool(args, "--json");
  auto const issues  = tmpl::validate_template(*decoded);

  // The identifiers reported are the ones the operator ASKED for, not the
  // set the template actually resolved from — `validate myset jira epic`
  // says `myset` even when the file came from the embedded defaults. The
  // loader stamps the requested set onto the resolved entry, so
  // `entry->set_name` and `ids.set_name` agree; `ids` is used to make the
  // intent explicit at the call site.
  if (issues.empty()) {
    ctx.out() << tmpl::validate_ok(ids.set_name, ids.system, ids.kind, as_json);
    return {};
  }

  // Detail on STDOUT, count on STDERR, exit 2. Both halves are written —
  // a caller reading only one stream sees only half the story, and that
  // asymmetry is the oracle's.
  ctx.out() << tmpl::validate_issues(ids.set_name, ids.system, ids.kind, entry->path, issues, as_json);
  return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                         tmpl::validate_summary(issues.size(), ids.set_name, ids.system, ids.kind)));
}

auto templates_init(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto root = resolve_templates_root(ctx);
  if (!root.has_value()) {
    return std::unexpected(root.error());
  }

  // `--force` is deliberately NOT read. See templates.cppm.
  std::vector<tmpl::embedded_file> embedded;
  for (auto const& e : cfg::embedded_templates()) {
    embedded.push_back({.system = e.system, .kind = e.kind, .body = e.body});
  }

  auto created = tmpl::extract_defaults(std::filesystem::path{*root}, embedded);
  if (!created.has_value()) {
    auto const tag = created.error() == tmpl::extract_error::invalid_input ? "InvalidInput" : "WriteFailed";
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("templates init: {}", tag)));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? tmpl::init_json(*created) : tmpl::init_text(*created));
  return {};
}

auto templates_path(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `--system`, `--set` and `--json` are all declared and all ignored. The
  // parameter is named and unused rather than dropped so the signature
  // still matches `handler_fn`.
  static_cast<void>(args);
  auto root = resolve_templates_root(ctx);
  if (!root.has_value()) {
    return std::unexpected(root.error());
  }
  ctx.out() << *root << "\n";
  return {};
}

auto templates_root_for(context& ctx) -> std::expected<std::string, domain_error> {
  return resolve_templates_root(ctx);
}

} // namespace planar::cmd::handlers
