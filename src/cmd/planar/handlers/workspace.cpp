/// @file workspace.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.workspace`.

module planar.cmd.planar.handlers.workspace;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.workspace;
import planar.cliapp.args;
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
      return std::unexpected(error_from_rendered(domain_error_kind::generic_failure, doctor::ambiguous_orgs_error()));
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

} // namespace planar::cmd::handlers
