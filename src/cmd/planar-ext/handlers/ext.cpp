/// @file ext.cpp
/// @brief Implementation of `planar.cmd.planar_ext.handlers.ext`. See ext.cppm
/// for the three-of-six cut and the two renderer traps.

module;

module planar.cmd.planar_ext.handlers.ext;

import std;
import cli11;
import planar.adapter;
import planar.cliapp.args;
import planar.db;
import planar.http;
import planar.json_text;
import planar.engine.external;
import planar.json_dom;
import planar.engine.config;
import planar.engine.config.effective;
import planar.engine.templates;
import planar.engine.extsync.propagate;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;
import planar.cmd.planar_ext.handler;
import planar.cmd.planar_ext.handlers.ext_adapter_factory;

namespace planar::cmd::ext::handlers {

namespace system_ns         = engine::external::system;
namespace jd                = json_dom;
namespace cfg               = engine::config;
namespace tmpl              = engine::templates;
namespace extsync_propagate = engine::extsync::propagate;

namespace {

/// @brief Expand a leading `~` against `$HOME`.
///
/// A local copy of `planar.cmd.planar.handlers.templates`'s
/// `expand_home` (task 6419 move) — that module stays on `planar` for the
/// `templates` verb family, which did not move, so the ten-line tilde
/// expansion is duplicated here rather than pulled across a `cmd_* ->
/// cmd_*` edge D18 forbids. The oracle itself keeps two copies of this
/// exact helper for the same reason (see the moved copy's own header).
/// `$HOME` and NOT `$PLANAR_HOME`.
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

/// @brief Resolve `$PLANAR_CONFIG_PATH`, or `$HOME/.planar/config.toml`.
///
/// A local copy of `planar.cmd.planar.cli_log`'s `resolve_config_path` —
/// see `expand_home`'s header for why this file carries its own rather
/// than importing across a `cmd_planar -> cmd_planar_ext` edge.
/// @param env The environment lookup.
/// @return The resolved path, or unset when neither variable is usable.
auto resolve_config_path(const env_lookup& env) -> std::optional<std::filesystem::path> {
  auto const home = env("HOME");
  if (auto const raw = env("PLANAR_CONFIG_PATH"); raw.has_value() && !raw->empty()) {
    if (*raw == "~") {
      return home.has_value() ? std::optional{std::filesystem::path{*home}} : std::nullopt;
    }
    if (raw->starts_with("~/")) {
      if (!home.has_value()) {
        return std::nullopt;
      }
      return std::filesystem::path{*home} / std::string_view{*raw}.substr(2);
    }
    return std::filesystem::path{*raw};
  }
  if (!home.has_value()) {
    return std::nullopt;
  }
  return std::filesystem::path{*home} / ".planar" / "config.toml";
}

/// @brief Resolve the operator's templates directory: `$PLANAR_TEMPLATES_DIR`
/// > `[templates] dir` in the config file > the embedded default, tilde
/// expanded. A local copy of `planar.cmd.planar.handlers.templates`'s
/// `resolve_templates_root` — see `expand_home`'s header.
/// @param ctx The invocation context.
/// @return The resolved root, or the exit-1 refusal.
auto templates_root_for_impl(context& ctx) -> std::expected<std::string, domain_error> {
  auto const cfg_path = resolve_config_path(ctx.env());

  std::optional<std::string> file_content;
  if (cfg_path.has_value()) {
    std::ifstream file(*cfg_path, std::ios::binary);
    if (file) {
      file_content = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
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

} // namespace

auto templates_root_for(context& ctx) -> std::expected<std::string, domain_error> {
  return templates_root_for_impl(ctx);
}

namespace {

/// @brief Map a registration failure onto this binary's error taxonomy.
///
/// `slug_exists` is `slug_conflict` (exit 6), not a generic failure — the
/// oracle-captured message and code are in ext.cppm's header.
/// @param err The engine failure.
/// @param slug The slug the operator supplied, for the message.
/// @return The domain error to report.
auto registration_error(system_ns::system_error err, std::string_view slug) -> domain_error {
  if (err == system_ns::system_error::slug_exists) {
    return error_from_body(domain_error_kind::slug_conflict, std::format("external system '{}' already registered", slug));
  }
  return error_from_body(domain_error_kind::generic_failure, std::format("ext register: registering '{}'", slug));
}

/// @brief Emit the shared post-registration payload.
///
/// Both `register jira` and `register github` render the SAME two lines,
/// which is why this is one function: the Zig originals are two files but
/// their print statements are identical modulo the values.
/// @param ctx The invocation context.
/// @param stored The row that was written.
/// @param as_json Whether `--json` was passed.
auto render_registered(context& ctx, const system_ns::external_system& stored, bool as_json) -> void {
  if (as_json) {
    std::string out = std::format(R"({{"ok":true,"id":{},"slug":)", stored.id);
    json_text::append_json_string(out, stored.slug);
    out += R"(,"kind":)";
    json_text::append_json_string(out, system_ns::system_kind_to_text(stored.kind));
    out += "}\n";
    ctx.out() << out;
    return;
  }
  // TWO spaces before the bracket — oracle-captured
  // `registered gh-demo  [github-issues, id:2]\n`.
  ctx.out() << std::format("registered {}  [{}, id:{}]\n", stored.slug, system_ns::system_kind_to_text(stored.kind), stored.id);
}

} // namespace

auto ext_register_jira(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  // CLI11 enforces `required()` on all three flags and on the positional, so
  // an absent value here cannot happen through the tree; the `value_or` is
  // for the direct-call path a handler test uses.
  auto const slug     = positional_string(args, "slug").value_or(std::string{});
  auto const base_url = flag_string(args, "--base-url").value_or(std::string{});
  auto const project  = flag_string(args, "--project").value_or(std::string{});
  auto const auth_env = flag_string(args, "--auth-env").value_or(std::string{});

  auto const stored =
      system_ns::register_jira(**conn, {.slug = slug, .base_url = base_url, .project = project, .auth_env = auth_env});
  if (!stored) {
    return std::unexpected(registration_error(stored.error(), slug));
  }
  render_registered(ctx, *stored, flag_bool(args, "--json"));
  return {};
}

auto ext_register_github(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const slug    = positional_string(args, "slug").value_or(std::string{});
  auto const project = flag_string(args, "--project").value_or(std::string{});
  // ABSENT, not empty: an unset `--auth-env` selects `gh-cli` auth, so the
  // optional has to survive the whole way into the engine rather than being
  // flattened with `value_or("")`.
  auto const auth_env = flag_string(args, "--auth-env");

  auto const stored = system_ns::register_github(
      **conn, {.slug     = slug,
               .project  = project,
               .auth_env = auth_env.has_value() ? std::optional<std::string_view>{*auth_env} : std::nullopt});
  if (!stored) {
    return std::unexpected(registration_error(stored.error(), slug));
  }
  render_registered(ctx, *stored, flag_bool(args, "--json"));
  return {};
}

auto ext_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const systems = system_ns::list(**conn);
  if (!systems) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "ext list: QueryFailed"));
  }

  if (flag_bool(args, "--json")) {
    // One object per line, NOT an array — and an empty database emits
    // absolutely nothing. Both oracle-captured; see ext.cppm.
    for (auto const& item : *systems) {
      std::string line = std::format(R"({{"id":{},"kind":)", item.id);
      json_text::append_json_string(line, system_ns::system_kind_to_text(item.kind));
      line += R"(,"slug":)";
      json_text::append_json_string(line, item.slug);
      // OMITTED when NULL rather than rendered as `null`.
      if (item.base_url.has_value()) {
        line += R"(,"base_url":)";
        json_text::append_json_string(line, *item.base_url);
      }
      if (item.default_project.has_value()) {
        line += R"(,"default_project":)";
        json_text::append_json_string(line, *item.default_project);
      }
      line += R"(,"auth_method":)";
      json_text::append_json_string(line, system_ns::auth_method_to_text(item.auth));
      line += R"(,"created_at":)";
      json_text::append_json_string(line, item.created_at);
      line += "}\n";
      ctx.out() << line;
    }
    return {};
  }

  if (systems->empty()) {
    ctx.out() << "no external systems registered\n";
    return {};
  }
  // The Zig widths are `{s:<20}  {s:<16}  {s:<36}  {s}` — LEFT-padded to 20,
  // 16 and 36 with TWO spaces between each pair, and the last column
  // unpadded, so a trailing column never leaves trailing whitespace. A value
  // longer than its width is NOT truncated; it simply pushes the rest right,
  // which is why the header row is emitted through the same format string.
  ctx.out() << std::format("{:<20}  {:<16}  {:<36}  {}\n", "slug", "kind", "base-url", "project");
  for (auto const& item : *systems) {
    ctx.out() << std::format("{:<20}  {:<16}  {:<36}  {}\n", item.slug, system_ns::system_kind_to_text(item.kind),
                             item.base_url.value_or(std::string{}), item.default_project.value_or(std::string{}));
  }
  return {};
}

auto ext_test(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const slug = positional_string(args, "slug").value_or(std::string{});

  auto const sys = system_ns::show_by_slug(**conn, slug);
  if (!sys) {
    // NOT_FOUND is exit 1 here, not 2 — the credential refusals below are
    // the exit-2 arms. Both codes are oracle-captured against the same verb.
    return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("external system '{}' not found", slug)));
  }

  auto const built = build_adapter(*sys, default_deps(ctx.env()));
  if (!built) {
    return std::unexpected(factory_error_message(built.error(), *sys));
  }

  // The oracle's `probeOk` asks whether `build` produced an adapter, which
  // it necessarily did on this line — so the "FAIL (adapter not
  // configured)" arm beside it is UNREACHABLE in the oracle too, not just
  // here. It is deliberately not reproduced: a branch no input can select
  // cannot be pinned against the oracle, and writing one would imply a
  // failure mode this verb does not have.
  if (flag_bool(args, "--json")) {
    std::string out = R"({"slug":)";
    json_text::append_json_string(out, slug);
    out += R"(,"ok":true})"
           "\n";
    ctx.out() << out;
    return {};
  }
  // TWO spaces before the parenthesis — oracle-captured
  // `jira-demo: ok  (adapter wired)\n`.
  ctx.out() << std::format("{}: ok  (adapter wired)\n", slug);
  return {};
}

namespace {

namespace link_ns = engine::external::link;

/// @brief A `<kind>:<id>` ref with the kind kept as RAW TEXT.
///
/// Deliberately not `handlers::kind_id_ref`, whose kind is already an
/// `external_entity_kind` — see ext.cppm on why this verb parses loosely
/// and refuses late.
struct raw_ref {
  std::string  kind; ///< The kind token, unvalidated.
  std::int64_t id{}; ///< The local row id.
};

/// @brief Parse `<kind>:<id>`, splitting on the LAST colon.
///
/// Accepts any non-empty kind. The scan direction mirrors the oracle's
/// backward walk; as with `sync`'s parser it is currently unobservable,
/// because no kind this verb reads contains a colon.
/// @param text The raw `--from` value.
/// @return The ref, or unset when there is no colon, either side is empty,
/// or the id is not an integer.
auto parse_raw_ref(std::string_view text) -> std::optional<raw_ref> {
  auto const at = text.rfind(':');
  if (at == std::string_view::npos || at == 0 || at + 1 >= text.size()) {
    return std::nullopt;
  }
  auto const digits = text.substr(at + 1);
  auto const parsed = cliapp::parse_int64_zig(digits);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  return raw_ref{.kind = std::string{text.substr(0, at)}, .id = *parsed};
}

/// @brief Read the local entity `ref` names, for rendering.
///
/// Serves FOUR kinds where a link may point at seven; `decision`,
/// `test_scenario` and `session` refuse here even though they are valid
/// `external_entity_kind` values. The column each kind supplies as `body`
/// differs (`plans` has `summary`, not `body`), which is why this is a
/// per-kind statement rather than one query with a kind parameter.
/// @param conn An open connection.
/// @param ref The parsed reference.
/// @return The entity, or the Zig error TAG to report — `InvalidInput` for
/// an unreadable kind, `NotFound` for an absent row.
auto read_local_entity(db::connection& conn, const raw_ref& ref) -> std::expected<adapter::local_entity, std::string_view> {
  std::string_view sql;
  if (ref.kind == "task") {
    sql = "select coalesce(title,''), coalesce(body,''), coalesce(status,''), coalesce(priority,0) from tasks where id = ?";
  } else if (ref.kind == "plan") {
    sql = "select coalesce(title,''), coalesce(summary,''), coalesce(status,''), 0 from plans where id = ?";
  } else if (ref.kind == "question") {
    sql = "select coalesce(title,''), coalesce(body,''), coalesce(status,''), 0 from questions where id = ?";
  } else if (ref.kind == "artifact") {
    sql = "select coalesce(title,''), coalesce(body,''), coalesce(status,''), 0 from artifacts where id = ?";
  } else {
    return std::unexpected(std::string_view{"InvalidInput"});
  }

  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, ref.id)) {
    return std::unexpected(std::string_view{"QueryFailed"});
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(std::string_view{"QueryFailed"});
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(std::string_view{"NotFound"});
  }
  return adapter::local_entity{.kind     = ref.kind,
                               .id       = ref.id,
                               .title    = std::string{stmt->column_text(0)},
                               .body     = std::string{stmt->column_text(1)},
                               .status   = std::string{stmt->column_text(2)},
                               .priority = stmt->column_int64(3)};
}

// `created_remote`, `trim_slash` and `create_remote` MOVED to
// `planar.cmd.planar_ext.handlers.ext_adapter_factory` at task 6335. They were
// TU-local here while `ext create` was their only caller; `ext propagate-one`
// and `workbench publish` are now the second and third, and all three must
// POST through the identical URL/header/parse shape. Duplicating it would
// have let the three drift apart silently — nothing in the state differential
// compares request shapes. See that module's header.

} // namespace

auto ext_create(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const from = flag_string(args, "--from").value_or(std::string{});
  auto const ref  = parse_raw_ref(from);
  if (!ref) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("invalid --from value '{}'; expected kind:integer-id", from)));
  }

  auto const slug = positional_string(args, "system-slug").value_or(std::string{});
  auto const sys  = system_ns::show_by_slug(**conn, slug);
  if (!sys) {
    return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("external system '{}' not found", slug)));
  }

  // Prose at exit 2 — see ext.cppm. NOT the sync trio's raw tag at exit 1.
  auto const built = build_adapter(*sys, default_deps(ctx.env()));
  if (!built) {
    return std::unexpected(factory_error_message(built.error(), *sys));
  }

  auto const local = read_local_entity(**conn, *ref);
  if (!local) {
    // ONE message template, TWO exit codes: `NotFound` is exit 1 and every
    // other tag is exit 2. Oracle-captured on `task:999` vs `foo:1`.
    auto const kind = local.error() == "NotFound" ? domain_error_kind::not_found : domain_error_kind::invalid_input;
    return std::unexpected(
        error_from_body(kind, std::format("ext create: read local {}:{}: {}", ref->kind, ref->id, local.error())));
  }

  // EVERY REFUSAL THAT CAN FIRE, FIRES BEFORE THE POST. This block used to
  // sit AFTER `create_remote`, which made `--role bogus` and a plain repeat
  // each mint a real ticket in the operator's Jira or GitHub and only then
  // refuse — tasks 6312 / 6313, fixed under decision 1067. See ext.cppm.
  auto const role_text = flag_string(args, "--role").value_or("mirror");
  auto const sync_text = flag_string(args, "--sync").value_or("two-way");

  auto const role = link_ns::link_role_from_text(role_text);
  if (!role) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("invalid --role '{}'", role_text)));
  }
  auto const direction = link_ns::sync_direction_from_text(sync_text);
  if (!direction) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("invalid --sync '{}'", sync_text)));
  }
  auto const entity_kind = link_ns::external_entity_kind_from_text(ref->kind);
  if (!entity_kind) {
    // Unreachable from the CLI: every kind `read_local_entity` accepts is
    // also one of the seven. Kept because the oracle spells it, and because
    // the two sets are independent — widening the read without widening
    // this would make it reachable.
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("unsupported entity kind '{}' for ext create", ref->kind)));
  }

  // THE IDEMPOTENCY GATE, keyed on `(entity_kind, entity_id, system_id,
  // link_role)`. Each of the four fields is load-bearing, and so is each of
  // the two exclusions:
  //
  //   * `link_role` is IN the key, and this verb's key is therefore narrower
  //     than `load_existing_mirror`'s (`propagate-one`'s gate, which hard-
  //     filters `link_role = 'mirror'`) but no wider. Dropping the field
  //     would be adopting a NEW cardinality rule — one link per entity per
  //     system — that nothing else in the tree enforces:
  //     `load_existing_mirror`'s own contract says a `parent` or `child` row
  //     for the same entity does not suppress propagation, and `planar link`
  //     writes a `reference` on an entity+system pair with no gate at all.
  //     A wide key here would refuse `--role reference` on an entity that
  //     already has a mirror while `planar link --role reference` still
  //     allowed it: one table, two verbs, two cardinality rules. Tasks
  //     6312 / 6313 do not need that, and decision 1067's FIX set does not
  //     authorise it.
  //   * `external_id` is OUT of the key, even though the table's `unique
  //     (entity_kind, entity_id, system_id, external_id)` includes it,
  //     because a real remote mints a FRESH id per POST. The constraint
  //     therefore catches a repeat only against a fixture that answers with
  //     a fixed id; against live Jira it would have let the second link
  //     INSERT. The post-insert `link_exists` branch below is a race
  //     backstop, not this check.
  //
  // `list_filter` carries no `link_role` field, so the role is applied to
  // the returned rows rather than to the SQL. That keeps the change inside
  // this handler: `link::list`'s clause-and-bind order is transcribed from
  // the Zig original and shared with `planar link list`, and the row set
  // being narrowed here is one entity on one system — a handful of rows at
  // most.
  auto const existing = link_ns::list(**conn, link_ns::list_filter{
                                                  .entity_kind = *entity_kind,
                                                  .entity_id   = ref->id,
                                                  .system_id   = sys->id,
                                              });
  if (!existing) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "ext create: read links: QueryFailed"));
  }
  auto const same_role = std::ranges::any_of(*existing, [&](const link_ns::ext_link& row) { return row.role == *role; });
  if (same_role) {
    // The message NAMES THE ROLE, because under this key the refusal is
    // about one role and a different `--role` on the same entity and system
    // is a legitimate next command.
    return std::unexpected(error_from_body(domain_error_kind::slug_conflict,
                                           std::format("{} external link for {}:{} on {} already exists",
                                                       link_ns::link_role_to_text(*role), ref->kind, ref->id, slug)));
  }

  adapter::create_options const opts{.issue_type = flag_string(args, "--type"), .project = sys->default_project};

  auto const payload = built->get()->instance().render(*local, opts);
  if (!payload) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("ext create: render payload: {}", adapter::adapter_error_name(payload.error()))));
  }

  // THE POST. Nothing below it can refuse except the insert itself, whose
  // failure is the one orphan this verb cannot design away — see ext.cppm.
  auto const created = create_remote(**built, *sys, *payload);
  if (!created) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("ext create: remote create: {}", created.error())));
  }

  auto const stored = link_ns::create(
      **conn, link_ns::create_args{
                  .entity_kind    = *entity_kind,
                  .entity_id      = ref->id,
                  .system_id      = sys->id,
                  .external_id    = created->external_id,
                  .external_url   = created->external_url.empty() ? std::nullopt : std::optional{created->external_url},
                  .role           = *role,
                  .direction      = *direction,
                  .initial_status = link_ns::sync_status::ok,
              });
  if (!stored) {
    if (stored.error() == link_ns::link_error::link_exists) {
      // Still reachable, and NOT dead code. The gate above keys on
      // `link_role`; this constraint keys on `external_id`. Two ways to
      // arrive here with the gate satisfied:
      //
      //   * a CONCURRENT create won the race between the read and the
      //     insert;
      //   * the remote answered with an id it has ALREADY handed this
      //     entity+system under a different role. A live Jira or GitHub
      //     mints a fresh key per POST so this does not arise there, but
      //     nothing in the adapter contract promises it, and the fixture
      //     server in `ext_create_leaf.t.cpp` is exactly such a remote.
      //
      // Either way the ticket this call just made is now orphaned — the
      // irreducible orphan documented in ext.cppm. The wording is the
      // ROLE-LESS one on purpose: unlike the gate above, this refusal is not
      // about the requested role, and naming it would misreport which row
      // collided.
      return std::unexpected(
          error_from_body(domain_error_kind::slug_conflict,
                          std::format("external link for {}:{} on {} already exists", ref->kind, ref->id, slug)));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "ext create: insert link: QueryFailed"));
  }

  if (flag_bool(args, "--json")) {
    std::string out = std::format(R"({{"ok":true,"link_id":{},"external_id":)", stored->id);
    json_text::append_json_string(out, created->external_id);
    out += R"(,"external_url":)";
    json_text::append_json_string(out, created->external_url);
    out += R"(,"sync_direction":)";
    json_text::append_json_string(out, sync_text);
    out += "}\n";
    ctx.out() << out;
    return {};
  }

  ctx.out() << std::format("created {} on {} for {}:{}\n", created->external_id, slug, ref->kind, ref->id);
  // TWO spaces after the id — oracle-captured `link id: 1  (two-way mirror)`.
  ctx.out() << std::format("link id: {}  ({} {})\n", stored->id, sync_text, role_text);
  return {};
}

/// @brief Map `(system_kind, role)` to the template kind to render.
///
/// `strategy_kind` is deliberately NOT a parameter: the oracle takes it and
/// discards it for GitHub (`_ = strategy_kind;`), and Jira never branches on
/// it either. Taking it would imply an influence that does not exist — see
/// ext.cppm. Exported (task 6451) — see ext.cppm's `entity_role` header.
auto template_kind_for_entity(std::string_view system_kind, entity_role role) -> std::optional<std::string_view> {
  if (system_kind == "jira") {
    switch (role) {
    case entity_role::plan_anchor:
      return "epic";
    case entity_role::plan_child:
      return "story";
    case entity_role::task:
      return "sub-task";
    }
  }
  if (system_kind == "github-issues") {
    switch (role) {
    case entity_role::plan_anchor:
      return "parent-issue";
    case entity_role::plan_child:
      return "issue";
    case entity_role::task:
      return "sub-task";
    }
  }
  return std::nullopt;
}

namespace {

/// @brief Decide whether a plan ref is the anchor or a child.
///
/// A task is always `task`. A plan is a CHILD when `parent_plan_id` is
/// non-null and an ANCHOR otherwise — the oracle reads the nullable column
/// rather than walking to a root.
auto determine_role(db::connection& conn, std::string_view kind, std::int64_t id)
    -> std::expected<entity_role, std::string_view> {
  if (kind == "task") {
    return entity_role::task;
  }
  auto stmt = conn.prepare("select parent_plan_id from plans where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(std::string_view{"QueryFailed"});
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(std::string_view{"QueryFailed"});
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(std::string_view{"NotFound"});
  }
  return stmt->is_null(0) ? entity_role::plan_anchor : entity_role::plan_child;
}

/// @brief Read a plan's or task's title, for the rendered result line.
auto load_entity_title(db::connection& conn, std::string_view kind, std::int64_t id)
    -> std::expected<std::string, std::string_view> {
  auto stmt = conn.prepare(kind == "task" ? "select coalesce(title,'') from tasks where id = ?"
                                          : "select coalesce(title,'') from plans where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(std::string_view{"QueryFailed"});
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(std::string_view{"QueryFailed"});
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(std::string_view{"NotFound"});
  }
  return std::string{stmt->column_text(0)};
}

} // namespace

/// @brief Definition of the shared per-entity propagation body declared in
/// `ext.cppm`; see that header for the full contract.
///
/// Documented again at the definition because Doxygen matches a declaration
/// to its definition by written signature, and this one spells its parameter
/// types through the `system_ns` / `link_ns` aliases rather than the fully
/// qualified `engine::external::*` names the interface uses. The two forms
/// name the same types but do not compare equal, so the interface's block
/// does not carry over the way `templates_root_for`'s does.
/// @param conn An open, migrated database connection.
/// @param handle The built adapter handle, or `nullptr` under `dry_run`.
/// @param sys The target external system.
/// @param entity_kind `"plan"` or `"task"`.
/// @param entity_id The local row id.
/// @param role How the entity sits in the feature tree.
/// @param strategy_kind The resolved strategy name, cached into the anchor's `config_json` on first link.
/// @param template_kind The template kind to render, from `template_kind_for_entity`.
/// @param direction The sync direction recorded on a newly created link.
/// @param dry_run When true, no adapter is contacted and no row is written.
/// @param templates_root The resolved templates root.
/// @return The outcome, or the failure.
auto propagate_one_entity(db::connection& conn, adapter_handle* handle, const system_ns::external_system& sys,
                          std::string_view entity_kind, std::int64_t entity_id, entity_role role, std::string_view strategy_kind,
                          std::string_view template_kind, link_ns::sync_direction direction, bool dry_run,
                          std::string_view templates_root) -> std::expected<propagate_one_outcome, domain_error> {
  // THE IDEMPOTENCY GATE. It precedes the template load and the POST, which
  // is what makes a repeat send nothing — see ext.cppm on the three
  // different answers this tree gives to "it already exists".
  auto const existing = link_ns::load_existing_mirror(conn, entity_kind, entity_id, sys.id);
  if (!existing) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "ext propagate-one: QueryFailed"));
  }
  if (!existing->empty()) {
    return propagate_one_outcome{.op = "skipped", .external_id = *existing};
  }

  auto const kind_text = system_ns::system_kind_to_text(sys.kind);
  auto built_ctx = entity_kind == "task" ? tmpl::build_task_context(conn, entity_id) : tmpl::build_plan_context(conn, entity_id);
  if (!built_ctx) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "ext propagate-one: building render context"));
  }
  auto const entry = cfg::load_template("default", kind_text, template_kind, templates_root);
  if (!entry) {
    return std::unexpected(error_from_body(domain_error_kind::not_found,
                                           std::format("ext propagate-one: no template for {}/{}", kind_text, template_kind)));
  }
  auto const decoded = jd::parse_json(entry->raw);
  if (!decoded) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("templates: {}: InvalidJson", entry->path)));
  }
  auto const rendered = tmpl::render_template(*decoded, *built_ctx);
  if (!rendered) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("rendering template: {}", tmpl::error_name(rendered.error()))));
  }
  // indent-2, matching the oracle's `Stringify.value(.. .indent_2)`. The
  // provider receives these exact bytes.
  auto const payload = jd::stringify_indent2(*rendered);

  if (dry_run) {
    return propagate_one_outcome{.op = "planned", .external_id = std::format("<{}>", template_kind)};
  }

  auto const created = create_remote(*handle, sys, payload);
  if (!created) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("ext propagate-one: {}", created.error())));
  }
  auto const entity_kind_enum = link_ns::external_entity_kind_from_text(entity_kind);
  if (!entity_kind_enum) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("unsupported entity kind '{}'", entity_kind)));
  }
  // Only the ANCHOR caches the strategy, so a later `ext propagate` hits
  // the stickiness path. Raw interpolation matches the oracle; the value
  // comes from a closed set.
  auto const config_json =
      role == entity_role::plan_anchor ? std::optional{std::format(R"({{"strategy":"{}"}})", strategy_kind)} : std::nullopt;
  auto const stored = link_ns::create(
      conn, link_ns::create_args{
                .entity_kind    = *entity_kind_enum,
                .entity_id      = entity_id,
                .system_id      = sys.id,
                .external_id    = created->external_id,
                .external_url   = created->external_url.empty() ? std::nullopt : std::optional{created->external_url},
                .role           = link_ns::link_role::mirror,
                .direction      = direction,
                .initial_status = link_ns::sync_status::ok,
                .config_json    = config_json,
            });
  if (!stored) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "ext propagate-one: insert link"));
  }
  return propagate_one_outcome{.op = "created", .external_id = created->external_id};
}

auto ext_propagate_one(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const from = flag_string(args, "--from").value_or(std::string{});
  auto const ref  = parse_raw_ref(from);
  if (!ref) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("invalid --from value '{}'; expected kind:integer-id", from)));
  }
  if (ref->kind != "plan" && ref->kind != "task") {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("unsupported entity kind '{}'; accepted: plan, task", ref->kind)));
  }

  // `parent-issue` / `projects-v2` are named ONLY to refuse them: both need
  // the feature-tree walk this per-entity primitive does not do.
  std::string strategy_override;
  if (auto const strat = flag_string(args, "--strategy"); strat.has_value()) {
    if (*strat == "parent-issue" || *strat == "projects-v2") {
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input,
                          std::format("strategy '{}' is not supported by propagate-one; use ext propagate --github-strategy {}",
                                      *strat, *strat)));
    }
    if (*strat != "tracking-issue") {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                             std::format("invalid --strategy '{}'; accepted: tracking-issue", *strat)));
    }
    strategy_override = "github-tracking-issue";
  }

  auto const sync_text = flag_string(args, "--sync").value_or(std::string{"read-only"});
  auto const direction = link_ns::sync_direction_from_text(sync_text);
  if (!direction) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input,
                        std::format("invalid --sync '{}'; accepted: read-only, write-back, two-way", sync_text)));
  }

  auto const slug = positional_string(args, "system").value_or(std::string{});
  auto const sys  = system_ns::show_by_slug(**conn, slug);
  if (!sys) {
    return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("external system '{}' not found", slug)));
  }

  auto const title = load_entity_title(**conn, ref->kind, ref->id);
  if (!title) {
    auto const kind = title.error() == "NotFound" ? domain_error_kind::not_found : domain_error_kind::generic_failure;
    return std::unexpected(error_from_body(kind, title.error() == "NotFound"
                                                     ? std::format("{}:{} not found", ref->kind, ref->id)
                                                     : std::format("ext propagate-one: read entity: {}", title.error())));
  }

  auto const role = determine_role(**conn, ref->kind, ref->id);
  if (!role) {
    auto const kind = role.error() == "NotFound" ? domain_error_kind::not_found : domain_error_kind::generic_failure;
    return std::unexpected(error_from_body(kind, role.error() == "NotFound"
                                                     ? std::format("{}:{} not found", ref->kind, ref->id)
                                                     : std::format("ext propagate-one: determine role: {}", role.error())));
  }

  auto const       kind_text = system_ns::system_kind_to_text(sys->kind);
  std::string_view strategy_kind;
  if (!strategy_override.empty()) {
    strategy_kind = strategy_override;
  } else {
    auto const picked = extsync_propagate::strategy_for_system(kind_text);
    if (!picked) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, "ext propagate-one: resolve strategy: UnsupportedSystemKind"));
    }
    strategy_kind = picked->kind;
  }

  auto const template_kind = template_kind_for_entity(kind_text, *role);
  if (!template_kind) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           "ext propagate-one: map strategy to template: UnsupportedSystemKind"));
  }

  // ORDER IS OBSERVABLE AND IS THE ORACLE'S, not the tidiest one. The
  // templates root and the adapter resolve BEFORE the idempotency check, so
  // a REPEAT with a missing credential still refuses on the credential
  // rather than reporting a successful skip. Checking the mirror first would
  // read better and would diverge.
  auto root = templates_root_for(ctx);
  if (!root) {
    return std::unexpected(root.error());
  }

  bool const                      dry_run = flag_bool(args, "--dry-run");
  std::unique_ptr<adapter_handle> handle;
  if (!dry_run) {
    auto built = build_adapter(*sys, default_deps(ctx.env()));
    if (!built) {
      return std::unexpected(factory_error_message(built.error(), *sys));
    }
    handle = std::move(*built);
  }

  // The idempotency check, render, POST, and `external_links` write are the
  // shared body `ext propagate`'s generic per-entity loop ALSO calls (task
  // 6451) — see `propagate_one_entity`'s header for why sharing the actual
  // function, not just the shape, is the point.
  auto outcome = propagate_one_entity(**conn, handle.get(), *sys, ref->kind, ref->id, *role, strategy_kind, *template_kind,
                                      *direction, dry_run, *root);
  if (!outcome) {
    return std::unexpected(outcome.error());
  }

  if (flag_bool(args, "--json")) {
    std::string out = R"({"ok":true,"entity_kind":)";
    json_text::append_json_string(out, ref->kind);
    out += std::format(R"(,"entity_id":{},"title":)", ref->id);
    json_text::append_json_string(out, *title);
    out += R"(,"op":)";
    json_text::append_json_string(out, outcome->op);
    out += R"(,"external_id":)";
    json_text::append_json_string(out, outcome->external_id);
    out += R"(,"system":)";
    json_text::append_json_string(out, sys->slug);
    out += R"(,"strategy":)";
    json_text::append_json_string(out, strategy_kind);
    out += "}";
    ctx.out() << out << '\n';
  } else {
    ctx.out() << std::format("{}{} {}:{} ({}) -> {}\n", dry_run ? "(dry-run) " : "", outcome->op, ref->kind, ref->id, *title,
                             outcome->external_id);
  }
  return {};
}

} // namespace planar::cmd::ext::handlers
