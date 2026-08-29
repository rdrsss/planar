/// @file workspace.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.workspace`.

module planar.cmd.planar.handlers.workspace;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.workspace;
import planar.json_text;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace doctor   = engine::workspace::doctor;
namespace identity = engine::workspace::identity;
namespace routing  = engine::workspace::routing;

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
    ctx.out() << "warning: --enrich is not yet implemented in Zig; skipping enrichment pass\n";
  }
  ctx.out() << std::format("built {} ({} projects, {} cross-repo deps)\n", path, projects, edges);
  return {};
}

} // namespace planar::cmd::handlers
