/// @file ext.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.ext`. See ext.cppm
/// for the three-of-six cut and the two renderer traps.

module planar.cmd.planar.handlers.ext;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
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

} // namespace planar::cmd::handlers
