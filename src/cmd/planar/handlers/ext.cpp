/// @file ext.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.ext`. See ext.cppm
/// for the three-of-six cut and the two renderer traps.

module;

// Glaze is not a module, so it comes in through the global module fragment
// rather than an import. `ext create` is this file's only consumer — see
// ext.cppm on why the creation path reads the provider's response itself
// instead of going through the adapter interface.
//
// `engine/extsync/json_read.hpp` has the same three accessors and is
// deliberately NOT reused: it belongs to a different TARGET, and including
// a private header across that boundary is the `cmd_* -> engine_*` file
// edge D18 exists to keep out. The guards below reproduce its semantics —
// a wrong-typed value reads as ABSENT, never as a parse failure.
#include <glaze/glaze.hpp>

module planar.cmd.planar.handlers.ext;

import std;
import cli11;
import planar.adapter;
import planar.cliapp.args;
import planar.db;
import planar.http;
import planar.json_text;
import planar.engine.external;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.ext_adapter_factory;

namespace planar::cmd::handlers {

namespace system_ns = engine::external::system;

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

/// @brief What a successful remote creation yielded.
struct created_remote {
  std::string external_id;  ///< The provider-side id, in Planar's spelling.
  std::string external_url; ///< The ticket URL; EMPTY when the provider gave none.
};

/// @brief Trim ONE trailing slash, as the oracle's `trimSlash` does.
///
/// One, not all: `http://h//` keeps a slash. Preserved rather than
/// "improved" because the resulting URL is observable in `external_url`.
/// @param text The base URL.
/// @return The trimmed view.
auto trim_slash(std::string_view text) -> std::string_view {
  if (!text.empty() && text.back() == '/') {
    return text.substr(0, text.size() - 1);
  }
  return text;
}

/// @brief POST `payload` to the provider's create endpoint and read back the
/// id it assigned.
///
/// Does NOT go through the adapter interface — see `adapter_handle`'s
/// header. The two providers differ in every part: the URL, the `Accept`
/// header, the response field carrying the id, and how the id is spelled
/// locally (Jira's bare `key`, GitHub's `<project>#<number>`).
/// @param handle The built adapter handle.
/// @param sys The registered system row.
/// @param payload The rendered request body.
/// @return The created ticket, or the Zig error TAG to report.
auto create_remote(const adapter_handle& handle, const system_ns::external_system& sys, std::string_view payload)
    -> std::expected<created_remote, std::string_view> {
  bool const is_jira = handle.kind() == adapter_kind::jira;

  // Jira has NO default base URL and refuses without one; GitHub falls back
  // to the public API host. GitHub additionally requires a project, Jira
  // does not (its project rides inside the rendered payload).
  std::string base;
  if (is_jira) {
    if (!sys.base_url.has_value()) {
      return std::unexpected(std::string_view{"InvalidInput"});
    }
    base = *sys.base_url;
  } else {
    base = sys.base_url.value_or("https://api.github.com");
  }

  std::string url;
  if (is_jira) {
    url = std::format("{}/rest/api/3/issue", trim_slash(base));
  } else {
    if (!sys.default_project.has_value()) {
      return std::unexpected(std::string_view{"InvalidInput"});
    }
    url = std::format("{}/repos/{}/issues", trim_slash(base), *sys.default_project);
  }

  http::request req{
      .verb    = http::method::post,
      .url     = url,
      .headers = {http::header{.name = "Content-Type", .value = "application/json"},
                  http::header{.name = "Authorization", .value = std::format("Bearer {}", handle.token())},
                  http::header{.name = "Accept", .value = is_jira ? "application/json" : "application/vnd.github+json"}},
      .body    = std::string{payload}};

  auto sent = handle.transport().send(req);
  if (!sent) {
    return std::unexpected(std::string_view{"TransportFailed"});
  }
  if (sent->status < 200 || sent->status >= 300) {
    return std::unexpected(std::string_view{"UnexpectedStatus"});
  }

  auto parsed = glz::read_json<glz::generic>(sent->body);
  if (!parsed || !parsed->is_object()) {
    return std::unexpected(std::string_view{"ParseFailed"});
  }
  glz::generic const& root = *parsed;

  if (is_jira) {
    // The `key` must be PRESENT and a STRING. A Jira 2xx carrying no key is
    // `ParseFailed`, not an empty id.
    if (!root.contains("key") || !root.at("key").is_string()) {
      return std::unexpected(std::string_view{"ParseFailed"});
    }
    auto const key = root.at("key").get<std::string>();
    return created_remote{.external_id = key, .external_url = std::format("{}/browse/{}", trim_slash(base), key)};
  }

  if (!root.contains("number") || !root.at("number").is_number()) {
    return std::unexpected(std::string_view{"ParseFailed"});
  }
  auto const number = static_cast<std::int64_t>(root.at("number").get<double>());

  // A MISSING or non-string `html_url` is the EMPTY string, NOT a failure —
  // the oracle's nested `if`. The empty URL is then stored as SQL NULL. So
  // the two fields are asymmetric: the id is required, the URL is not.
  std::string html_url;
  if (root.contains("html_url") && root.at("html_url").is_string()) {
    html_url = root.at("html_url").get<std::string>();
  }
  return created_remote{.external_id = std::format("{}#{}", *sys.default_project, number), .external_url = std::move(html_url)};
}

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

  adapter::create_options const opts{.issue_type = flag_string(args, "--type"), .project = sys->default_project};

  auto const payload = built->get()->instance().render(*local, opts);
  if (!payload) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("ext create: render payload: {}", adapter::adapter_error_name(payload.error()))));
  }

  // THE POST HAPPENS HERE, BEFORE `--role` / `--sync` ARE VALIDATED AND
  // BEFORE THE DUPLICATE CHECK. Both orderings are the oracle's and both are
  // observable through the remote's request log; see ext.cppm.
  auto const created = create_remote(**built, *sys, *payload);
  if (!created) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("ext create: remote create: {}", created.error())));
  }

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

} // namespace planar::cmd::handlers
