/// @file workspace.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.workspace`.

module planar.cmd.planar.handlers.workspace;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.workspace;
import planar.engine.identity.association;
import planar.engine.config.init;
import planar.db;
import planar.json_text;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.workspace.routing;
import planar.cmd.planar.handlers.workspace.routing_build;
import planar.cmd.planar.handlers.workspace.routing_show;
import planar.cmd.planar.handlers.workspace.init;
import planar.cmd.planar.handlers.workspace.doctor;
import planar.cmd.planar.handlers.workspace.regenerate;

namespace planar::cmd::handlers {

namespace doctor   = engine::workspace::doctor;
namespace identity = engine::workspace::identity;
namespace routing  = engine::workspace::routing;
namespace assoc    = engine::identity;
namespace cfg      = engine::config;

namespace {

auto git_marker_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec) || std::filesystem::is_symlink(path, ec);
}

auto scan_git_children(const std::filesystem::path& root, int max_depth) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> out;
  std::error_code                    ec;
  if (max_depth < 1 || !std::filesystem::is_directory(root, ec))
    return out;
  std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
  while (!ec && it != end) {
    const auto path = it->path();
    const auto name = path.filename().string();
    if (it.depth() >= max_depth || (!name.empty() && name.front() == '.') || name == "node_modules") {
      if (it->is_directory(ec))
        it.disable_recursion_pending();
    }
    if (!ec && it->is_directory(ec) && git_marker_exists(path / ".git")) {
      out.push_back(path);
      it.disable_recursion_pending();
    }
    it.increment(ec);
  }
  std::ranges::sort(out);
  return out;
}

auto workspace_config(const std::filesystem::path& root, bool meta) -> std::string {
  std::string out{"{\"root_path\":"};
  json_text::append_json_string(out, root.string());
  if (meta)
    out += ",\"workspace_shape\":\"meta-repo\"";
  out += '}';
  return out;
}

/// The meta-workspace root is a durable scope guard, not display metadata.
/// `workspace_config` is produced locally, so a small quoted-field reader is
/// sufficient here and intentionally refuses malformed/foreign blobs rather
/// than silently accepting a cross-root reuse.
auto recorded_workspace_root(const std::optional<std::string>& config) -> std::optional<std::string> {
  if (!config)
    return std::nullopt;
  constexpr std::string_view prefix = "\"root_path\":\"";
  const auto                 begin  = config->find(prefix);
  if (begin == std::string::npos)
    return std::nullopt;
  const auto value_begin = begin + prefix.size();
  const auto value_end   = config->find('"', value_begin);
  if (value_end == std::string::npos)
    return std::nullopt;
  return config->substr(value_begin, value_end - value_begin);
}

auto no_children_error(const std::filesystem::path& cwd, bool meta) -> domain_error {
  return error_from_body(domain_error_kind::invalid_input,
                         std::format("no {}directories with .git found under {}; nothing to initialize{}",
                                     meta ? "nested " : "child ", cwd.string(), meta ? " as a meta workspace" : ""));
}

struct init_project_result {
  std::string slug;
  std::string path;
  bool        created            = false;
  bool        membership_created = false;
};

/// Query this in the composition layer before calling `add_member`: that
/// engine API deliberately returns void, while `workspace init` promises the
/// two independent upsert facts in both renderers.
auto project_before(db::connection& conn, std::string_view path, std::int64_t org_id)
    -> std::expected<std::optional<std::pair<std::string, bool>>, db::db_error> {
  auto stmt = conn.prepare("select p.slug, exists(select 1 from project_associations pa where pa.project_id = p.id and "
                           "pa.association_id = ?) from projects p where p.root_path = ?");
  if (!stmt)
    return std::unexpected(stmt.error());
  if (auto bound = stmt->bind_int64(1, org_id); !bound)
    return std::unexpected(bound.error());
  if (auto bound = stmt->bind_text(2, path); !bound)
    return std::unexpected(bound.error());
  auto step = stmt->step();
  if (!step)
    return std::unexpected(step.error());
  if (*step == db::step_result::done)
    return std::optional<std::pair<std::string, bool>>{};
  return std::optional<std::pair<std::string, bool>>{std::pair{std::string{stmt->column_text(0)}, stmt->column_int64(1) != 0}};
}

auto project_slug_after(db::connection& conn, std::string_view path) -> std::expected<std::string, db::db_error> {
  auto stmt = conn.prepare("select slug from projects where root_path = ?");
  if (!stmt)
    return std::unexpected(stmt.error());
  if (auto bound = stmt->bind_text(1, path); !bound)
    return std::unexpected(bound.error());
  auto step = stmt->step();
  if (!step)
    return std::unexpected(step.error());
  if (*step != db::step_result::row)
    return std::unexpected(db::db_error{.code_ = -1, .message_ = "project disappeared"});
  return std::string{stmt->column_text(0)};
}

} // namespace

auto workspace_doctor(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // `ctx.env()` passes straight through: `planar.engine.workspace.identity`
  // declares the same `env_lookup` signature a `context` carries, which is
  // the whole point of modelling the environment as a callable rather than
  // a snapshot (see context.cppm). It matters more here than usual — see
  // this leaf's module header for where doctor actually writes.
  auto reports = doctor::run(**conn, ctx.env());
  if (!reports.has_value()) {
    // The Zig handler dies with `listing org associations failed: {s}`
    // interpolating `@errorName(e)`. The C++ `run` collapses every failure
    // to `nullopt`, and `listOrgs`'s only failure tag is `QueryFailed`, so
    // the tag is transcribed rather than captured — the path needs a
    // broken `associations` table to reach and no oracle probe produced it.
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "listing org associations failed: QueryFailed"));
  }

  // Both renderers return COMPLETE payloads and this layer appends
  // nothing. They disagree on an empty database — `{"orgs":[]}\n` versus
  // zero bytes — and that disagreement is the oracle's, not a bug.
  ctx.out() << (flag_bool(args, "--json") ? doctor::doctor_json(*reports) : doctor::doctor_text(*reports));
  return {};
}

auto workspace_init(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  if (flag_bool(args, "--no-scan") && flag_bool(args, "--enrich")) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "cannot combine --no-scan and --enrich"));
  }
  const auto cwd        = ctx.cwd();
  const bool meta       = flag_bool(args, "--meta-repo");
  const bool cwd_is_git = git_marker_exists(cwd / ".git");
  if (cwd_is_git && !meta) {
    return std::unexpected(error_from_body(
        domain_error_kind::invalid_input,
        std::format("current directory {} is a git repository; use `planar init` for single repos", cwd.string())));
  }
  if (!cwd_is_git && meta) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input,
                        std::format("`--meta-repo` requires current directory {} to be a git repository", cwd.string())));
  }
  const auto depth    = static_cast<int>(cliapp::flag_int(args, "--scan").value_or(1));
  auto       children = scan_git_children(cwd, depth > 0 ? depth : 1);
  if (children.empty())
    return std::unexpected(no_children_error(cwd, meta));

  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  const auto slug    = cliapp::flag_string(args, "--slug").value_or(cfg::derive_slug(cwd.filename().string()));
  const auto name    = cliapp::flag_string(args, "--name").value_or(cwd.filename().string());
  auto       org     = assoc::show_by_slug(**conn, slug);
  bool       created = false;
  if (!org) {
    if (org.error() != assoc::association_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "registering workspace failed: QueryFailed"));
    }
    auto made = assoc::create(**conn, assoc::create_args{.slug        = slug,
                                                         .name        = name,
                                                         .kind        = assoc::association_kind::org,
                                                         .config_json = workspace_config(cwd, meta)});
    if (!made)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "registering workspace failed: QueryFailed"));
    org     = std::move(made);
    created = true;
  }
  if (org->kind != assoc::association_kind::org) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "registering workspace failed: InvalidInput"));
  }
  if (meta) {
    auto const recorded_root = recorded_workspace_root(org->config_json);
    if (!recorded_root) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                             std::format("org:{} has invalid workspace config; repair or choose a different "
                                                         "--slug before running `workspace init --meta-repo`",
                                                         slug)));
    }
    if (*recorded_root != cwd.string()) {
      return std::unexpected(error_from_body(
          domain_error_kind::invalid_input,
          std::format(
              "org:{} already exists with a different workspace root; choose a different --slug or run from the recorded root",
              slug)));
    }
  }

  std::vector<std::filesystem::path> projects = children;
  if (meta)
    projects.insert(projects.begin(), cwd);
  std::vector<init_project_result> project_results;
  project_results.reserve(projects.size());
  for (const auto& project : projects) {
    auto before = project_before(**conn, project.string(), org->id);
    if (!before)
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "registering workspace failed: QueryFailed"));
    auto member = assoc::add_member(**conn, slug, project.string());
    if (!member && member.error() != assoc::association_error::already_member) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "registering workspace failed: QueryFailed"));
    }
    auto project_slug = before->has_value() ? before->value().first : project_slug_after(**conn, project.string()).value_or("");
    if (project_slug.empty())
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "registering workspace failed: QueryFailed"));
    project_results.push_back({.slug               = std::move(project_slug),
                               .path               = project.string(),
                               .created            = !before->has_value(),
                               .membership_created = !before->has_value() || !before->value().second});
  }

  bool         pipeline_error      = false;
  bool         layout_setup_failed = false;
  std::size_t  routing_projects = 0, routing_edges = 0;
  std::int64_t agents_bytes = 0;
  std::string  agents_path;
  std::string  strategy;
  if (!flag_bool(args, "--no-scan")) {
    auto layout = identity::ensure_layout(ctx.env(), org->id);
    auto home   = identity::planar_home(ctx.env());
    if (!layout || !home) {
      pipeline_error      = true;
      layout_setup_failed = true;
    } else {
      auto rules = routing::load_capability_rules(*home / "templates" / "workspace-capabilities.toml");
      if (!rules)
        pipeline_error = true;
      else {
        if (rules->empty())
          rules = routing::default_capability_rules();
        auto overrides = routing::load_overrides(layout->dir / "routing-table-overrides.json");
        if (!overrides)
          pipeline_error = true;
        else {
          auto table = routing::build(**conn, org->id, org->slug, org->name, *rules);
          if (!table) {
            pipeline_error = true;
          } else {
            routing::apply_overrides(*table, *overrides);
            routing_projects = table->projects.size();
            routing_edges    = table->cross.dependency_edges.size();
            if (!routing::write_table(layout->routing_table, *table))
              pipeline_error = true;
            if (!pipeline_error) {
              auto regen = engine::workspace::regenerate::regenerate(**conn, ctx.env(), org->id);
              if (!regen)
                pipeline_error = true;
              else {
                agents_bytes = regen->bytes_written;
                agents_path  = regen->agents_path;
                if (meta)
                  strategy = "skipped-meta-repo";
                else if (auto installed = identity::install_symlinks(cwd, *layout))
                  strategy = std::string{*installed};
                else
                  pipeline_error = true;
              }
            }
          }
        }
      }
    }
  }
  // Mirrors the Zig oracle's `workspace init` partial-failure reporting: a
  // failed pipeline pass never rolls back the org/project registration that
  // already committed above -- it only surfaces a warning plus a recovery
  // hint on stderr (or the `error` field in JSON) naming the verb(s) an
  // operator needs to re-run. When the layout directory itself could not be
  // created/opened (e.g. `$PLANAR_HOME` resolves to a file), the recovery
  // must start with `workspace doctor`, since routing/regenerate cannot run
  // without a working layout; any later pipeline step failing with a
  // working layout can skip straight to routing build + regenerate.
  const std::string pipeline_error_kind = !pipeline_error ? "" : layout_setup_failed ? "layout-create-failed" : "QueryFailed";
  if (pipeline_error && !flag_bool(args, "--no-scan") && !flag_bool(args, "--json")) {
    ctx.err() << "warning: pipeline pass failed: " << pipeline_error_kind << "\n";
    if (layout_setup_failed)
      ctx.err() << "hint: re-run `planar workspace doctor` then `planar workspace routing build` && `planar workspace "
                   "regenerate`\n";
    else
      ctx.err() << "hint: re-run `planar workspace routing build` && `planar workspace regenerate`\n";
  }
  if (flag_bool(args, "--json")) {
    std::string out = "{\"org\":{\"id\":" + std::format("{}", org->id) + ",\"slug\":";
    json_text::append_json_string(out, org->slug);
    out += ",\"name\":";
    json_text::append_json_string(out, org->name);
    out += std::format(",\"created\":{}}},\"projects\":[", created ? "true" : "false");
    for (std::size_t i = 0; i < project_results.size(); ++i) {
      if (i)
        out += ',';
      out += "{\"slug\":";
      json_text::append_json_string(out, project_results[i].slug);
      out += ",\"path\":";
      json_text::append_json_string(out, project_results[i].path);
      out += std::format(",\"created\":{},\"membership_created\":{}}}", project_results[i].created ? "true" : "false",
                         project_results[i].membership_created ? "true" : "false");
    }
    out += std::format("],\"pipeline\":{{\"skipped\":{},\"routing\":{{\"project_count\":{},\"cross_repo_deps\":{},\"enrich_"
                       "enabled\":false,\"enrich_misses\":0}},\"regenerate\":{{\"agents_path\":",
                       flag_bool(args, "--no-scan") ? "true" : "false", routing_projects, routing_edges);
    json_text::append_json_string(out, agents_path);
    out += std::format(",\"bytes\":{}}},\"symlinks\":{{\"strategy\":", agents_bytes);
    json_text::append_json_string(out, strategy);
    out += meta ? ",\"installed\":[]}" : ",\"installed\":[\"AGENTS.md\",\"CLAUDE.md\"]}";
    out += ",\"error\":";
    json_text::append_json_string(out, pipeline_error_kind);
    out += "}}\n";
    ctx.out() << out;
    return {};
  }
  ctx.out() << (created ? "created" : "reused") << " org:" << slug << " (" << name << ")\n";
  for (std::size_t i = 0; i < project_results.size(); ++i) {
    const auto status = !project_results[i].created && !project_results[i].membership_created ? "already present"
                        : !project_results[i].created                                         ? "linked existing project"
                                                                                              : "auto-created";
    ctx.out() << "  " << (i + 1 == project_results.size() ? "└─" : "├─") << " project:" << project_results[i].slug << "   ["
              << project_results[i].path << "]   (" << status << ", member-of org:" << slug << ")\n";
  }
  ctx.out() << "\n" << projects.size() << " repos initialized as projects, all members of org:" << slug << ".\n";
  if (flag_bool(args, "--no-scan"))
    ctx.out() << "Run `planar workspace routing build` and `planar workspace regenerate` to populate the state directory.\n";
  else if (!pipeline_error) {
    ctx.out() << "Routing table refreshed (" << routing_projects << " projects, " << routing_edges << " cross-repo deps).\n";
    ctx.out() << "AGENTS.md regenerated (" << agents_bytes << " bytes).\n";
    ctx.out() << "Symlinks installed: AGENTS.md, CLAUDE.md (strategy: " << strategy << ").\n";
  }
  ctx.out() << "Run `planar assoc tree` to view the hierarchy.\n";
  return {};
}

auto workspace_routing_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // The `[workspace]` selector. `resolve_org` treats nullopt and a blank
  // string alike, so passing the optional straight through is correct.
  auto target = cliapp::positional_string(args, "workspace");
  auto org    = identity::resolve_org(**conn, target.has_value() ? std::optional<std::string_view>{*target} : std::nullopt);
  if (!org.has_value()) {
    switch (org.error()) {
    case identity::resolve_error::not_found:
      // `error_from_rendered`, not `_from_body`: both helpers already carry
      // the `error: ` prefix AND the terminator, so composing around them
      // would emit `error: error: ...`.
      //
      // Shared byte-for-byte with `doctor`'s refusal, and note it also
      // covers an UNMATCHED slug on a populated database — see
      // identity.cppm's "the refusal that lies".
      return std::unexpected(error_from_rendered(domain_error_kind::not_found, doctor::no_orgs_error()));
    case identity::resolve_error::ambiguous:
      // `invalid_input` (exit 2), NOT `generic_failure` (exit 1). Corrected
      // at task 6275 — the oracle dies through `error.InvalidInput` here.
      // See this leaf's header for the side-by-side capture.
      return std::unexpected(error_from_rendered(domain_error_kind::invalid_input, doctor::ambiguous_orgs_error()));
    case identity::resolve_error::query_failed:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving workspace failed: QueryFailed"));
    }
  }

  // `load_layout`, deliberately NOT `ensure_layout`: reading must not create
  // the state directory. See this leaf's header.
  auto layout = identity::load_layout(ctx.env(), org->id);
  if (!layout.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "loading workspace layout failed: NotFound"));
  }

  std::ifstream input(layout->routing_table, std::ios::binary);
  if (!input) {
    return std::unexpected(
        error_from_body(domain_error_kind::not_found, routing::missing_table_error(layout->routing_table.string())));
  }
  const std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

  // The JSON arm never decodes — it cannot reach either failure below.
  if (flag_bool(args, "--json")) {
    ctx.out() << routing::show_json(raw);
    return {};
  }

  auto table = routing::decode(raw);
  if (!table.has_value()) {
    // ONE message template, FOUR tags, TWO exit codes. The three PARSE
    // failures (`SyntaxError`, `UnexpectedEndOfInput`, `DuplicateField`)
    // land in the generic bucket at exit 1; only `InvalidInput` — a
    // document that parsed but is not a usable table — reaches the
    // user-input bucket at exit 2.
    const auto kind =
        table.error() == routing::decode_error::invalid ? domain_error_kind::invalid_input : domain_error_kind::generic_failure;
    return std::unexpected(
        error_from_body(kind, std::format("decoding routing table failed: {}", routing::decode_error_name(table.error()))));
  }

  ctx.out() << routing::render_text(*table);
  return {};
}

auto workspace_routing_build(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto target = cliapp::positional_string(args, "workspace");
  auto org    = identity::resolve_org(**conn, target.has_value() ? std::optional<std::string_view>{*target} : std::nullopt);
  if (!org.has_value()) {
    switch (org.error()) {
    case identity::resolve_error::not_found:
      return std::unexpected(error_from_rendered(domain_error_kind::not_found, doctor::no_orgs_error()));
    case identity::resolve_error::ambiguous:
      // Exit 2. Same arm, same reasoning, as the correction in `show` above.
      return std::unexpected(error_from_rendered(domain_error_kind::invalid_input, doctor::ambiguous_orgs_error()));
    case identity::resolve_error::query_failed:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving workspace failed: QueryFailed"));
    }
  }

  // `ensure_layout`, deliberately NOT `load_layout`: building CREATES the
  // state directory. That is the difference from `show` beside it.
  auto layout = identity::ensure_layout(ctx.env(), org->id);
  if (!layout.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "ensuring workspace state directory failed: NotFound"));
  }

  auto home = identity::planar_home(ctx.env());
  if (!home.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "resolving capability rules path failed: NotFound"));
  }
  auto rules = routing::load_capability_rules(*home / "templates" / "workspace-capabilities.toml");
  if (!rules.has_value()) {
    // TWO exit codes behind one template: `ParseFailed` at 1, `InvalidInput`
    // at 2. Same split the overrides load and the table decode both carry.
    const auto kind =
        rules.error() == routing::rules_error::invalid ? domain_error_kind::invalid_input : domain_error_kind::generic_failure;
    return std::unexpected(
        error_from_body(kind, std::format("loading capability rules failed: {}", routing::rules_error_name(rules.error()))));
  }
  // An EMPTY result — an absent file, or one with no `[[rule]]` header —
  // means "use the built-ins", NOT "match nothing". Oracle-captured on both.
  if (rules->empty()) {
    rules = routing::default_capability_rules();
  }

  auto loaded_overrides = routing::load_overrides(layout->dir / "routing-table-overrides.json");
  if (!loaded_overrides.has_value()) {
    const auto kind = loaded_overrides.error() == routing::decode_error::invalid ? domain_error_kind::invalid_input
                                                                                 : domain_error_kind::generic_failure;
    return std::unexpected(error_from_body(
        kind, std::format("loading routing overrides failed: {}", routing::decode_error_name(loaded_overrides.error()))));
  }

  auto table = routing::build(**conn, org->id, org->slug, org->name, *rules);
  if (!table.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "building routing table failed: QueryFailed"));
  }
  routing::apply_overrides(*table, *loaded_overrides);

  if (!routing::write_table(layout->routing_table, *table)) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "writing routing table failed: AccessDenied"));
  }

  const auto path     = layout->routing_table.string();
  const auto projects = table->projects.size();
  const auto edges    = table->cross.dependency_edges.size();

  if (flag_bool(args, "--json")) {
    // `enrich_enabled` and `enrich_misses` are hardcoded in the oracle too —
    // the enrichment pass does not exist. Emitted so the shape is stable for
    // whoever eventually implements it.
    std::string out = "{";
    out += "\"path\":";
    json_text::append_json_string(out, path);
    out += std::format(",\"projects\":{},\"dependency_edges\":{},", projects, edges);
    out += "\"enrich_enabled\":false,\"enrich_misses\":0}\n";
    ctx.out() << out;
    return {};
  }

  // The warning precedes the result line and goes to STDOUT, not stderr.
  // Both captured.
  if (flag_bool(args, "--enrich")) {
    ctx.out() << "warning: --enrich is not yet implemented; skipping enrichment pass\n";
  }
  ctx.out() << std::format("built {} ({} projects, {} cross-repo deps)\n", path, projects, edges);
  return {};
}

auto workspace_regenerate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto target = cliapp::positional_string(args, "workspace");
  auto org    = identity::resolve_org(**conn, target.has_value() ? std::optional<std::string_view>{*target} : std::nullopt);
  if (!org.has_value()) {
    switch (org.error()) {
    case identity::resolve_error::not_found:
      return std::unexpected(error_from_rendered(domain_error_kind::not_found, doctor::no_orgs_error()));
    case identity::resolve_error::ambiguous:
      return std::unexpected(error_from_rendered(domain_error_kind::invalid_input, doctor::ambiguous_orgs_error()));
    case identity::resolve_error::query_failed:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving workspace failed: QueryFailed"));
    }
  }

  auto outcome = engine::workspace::regenerate::regenerate(**conn, ctx.env(), org->id);
  if (!outcome.has_value()) {
    const auto kind = outcome.error().kind == engine::workspace::regenerate::error_kind::not_found ? domain_error_kind::not_found
                      : outcome.error().kind == engine::workspace::regenerate::error_kind::invalid_input
                          ? domain_error_kind::invalid_input
                          : domain_error_kind::generic_failure;
    return std::unexpected(error_from_body(kind, outcome.error().message));
  }

  if (flag_bool(args, "--json")) {
    std::string out = "{\"agents_path\":";
    json_text::append_json_string(out, outcome->agents_path);
    out += ",\"manifest_path\":";
    json_text::append_json_string(out, outcome->manifest_path);
    out += std::format(",\"project_count\":{},\"bytes_written\":{},\"manifest_root\":", outcome->project_count,
                       outcome->bytes_written);
    json_text::append_json_string(out, outcome->manifest_root);
    out += "}\n";
    ctx.out() << out;
    return {};
  }

  ctx.out() << std::format("regenerated AGENTS.md for org:{} ({} projects, {} bytes)\n", org->slug, outcome->project_count,
                           outcome->bytes_written);
  return {};
}

namespace {

/// @brief Declare the nested `workspace routing` group and its children.
///
/// A file-local helper rather than an inline block so that `show`, a
/// child name this tree uses at several depths, is scoped to the group
/// it belongs to.
/// @param workspace The `workspace` group node.
auto declare_workspace_routing(CLI::App& workspace) -> void {
  CLI::App* routing = workspace_cli::attach_routing(workspace);

  workspace_cli::attach_routing_build(routing);

  workspace_cli::attach_routing_show(routing);
}

} // namespace

auto declare_workspace(CLI::App& root) -> void {
  CLI::App* workspace = root.add_subcommand(
      "workspace", "Workspace administration.\n\n  A workspace is identified by an associations row of kind=org. Each\n  "
                   "workspace owns a state directory under\n  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the "
                   "canonical\n  AGENTS.md surface for the org.");
  workspace->require_subcommand(0);

  workspace_cli::attach_init(workspace);

  workspace_cli::attach_doctor(workspace);
  declare_workspace_routing(*workspace);

  workspace_cli::attach_regenerate(workspace);
}

} // namespace planar::cmd::handlers
