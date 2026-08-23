/// @file action.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.action`.

module;

#include <glaze/json/read.hpp>

module planar.cmd.planar_agent.handlers.action;

import std;
import planar.cli;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentrender;
import planar.cmd.planar_agent.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.locality;
import planar.cmd.planar_agent.handlers.support;

namespace planar::cmd::agent::handlers {

namespace aa     = engine::runtime::agentactivity;
namespace render = engine::runtime::agentrender;

namespace {

/// @brief View an optional string as an optional string_view.
/// @param value The owning optional.
/// @return A view over it, or unset.
auto view(const std::optional<std::string>& value) -> std::optional<std::string_view> {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::string_view{*value};
}

/// @brief A parsed `action start --entity` value.
struct action_entity {
  aa::action_entity_kind kind{}; ///< The entity kind.
  std::int64_t           id{};   ///< The entity id.
};

/// @brief Parse `--entity` against the WIDER action-entity kind set.
///
/// Separate from `args`' `parse_entity_ref` on purpose — see the module
/// header. The three refusal messages below are literal oracle strings and
/// are distinct from the one `claim --entity` emits.
/// @param raw The raw value.
/// @return The parsed entity, or the refusal to report.
auto parse_action_entity(std::string_view raw) -> std::expected<action_entity, domain_error> {
  auto const colon = raw.find(':');
  if (colon == std::string_view::npos) {
    return std::unexpected(invalid_input_error(std::format("invalid --entity '{}'; expected kind:id", raw)));
  }
  auto const kind_text = raw.substr(0, colon);
  auto const id_text   = raw.substr(colon + 1);
  auto const id        = parse_int64_zig(id_text);
  if (!id.has_value()) {
    return std::unexpected(invalid_input_error(std::format("invalid --entity id '{}'", id_text)));
  }
  auto const kind = aa::action_entity_kind_from_text(kind_text);
  if (!kind.has_value()) {
    return std::unexpected(invalid_input_error(std::format("unknown entity kind '{}'", kind_text)));
  }
  return action_entity{.kind = *kind, .id = *id};
}

} // namespace

auto action_start(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const token = flag_string(args, "--claim").value_or(std::string{});
  auto const held  = aa::get_claim_by_token(**conn, token);
  if (!held) {
    return std::unexpected(verb_error("claim lookup", held.error()));
  }

  auto const kind_text = flag_string(args, "--kind").value_or(std::string{});
  auto const kind      = aa::action_kind_from_text(kind_text);
  if (!kind.has_value()) {
    return std::unexpected(invalid_input_error(std::format("unknown action kind '{}'", kind_text)));
  }

  auto const skip = flag_bool(args, "--no-locality-probe") || !aa::probe_default(*kind);
  auto const loc  = resolve_locality(flag_string(args, "--repo-root"), ctx.cwd(), skip);

  // The claim's newest still-open action becomes the parent, so nesting
  // builds itself. Absent (a claim with nothing open) means this action is
  // a root, which is not an error.
  auto const parent = aa::latest_open_action_for_claim(**conn, held->id);

  std::optional<aa::action_entity_kind> entity_kind;
  std::optional<std::int64_t>           entity_id;
  if (auto const raw = flag_string(args, "--entity"); raw.has_value()) {
    auto const parsed = parse_action_entity(*raw);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    entity_kind = parsed->kind;
    entity_id   = parsed->id;
  }

  auto const metadata = flag_string(args, "--metadata");
  if (metadata.has_value() && glz::validate_json(*metadata)) {
    return std::unexpected(invalid_input_error(std::format("--metadata is not valid JSON: {}", *metadata)));
  }

  auto const vendor_role = flag_string(args, "--vendor-role");
  auto const started     = aa::start_action(**conn, aa::start_action_args{
                                                        .session_id       = held->session_id,
                                                        .parent_action_id = parent,
                                                        .claim_id         = held->id,
                                                        .kind             = *kind,
                                                        .entity           = entity_kind,
                                                        .entity_id        = entity_id,
                                                        .vendor           = held->vendor,
                                                        .vendor_role      = view(vendor_role),
                                                        .loc              = loc,
                                                        .metadata         = view(metadata),
                                                    });
  if (!started) {
    return std::unexpected(verb_error("startAction", started.error()));
  }
  auto const row = aa::get_action_by_id(**conn, *started);
  if (!row) {
    return std::unexpected(verb_error("getActionById", row.error()));
  }

  ctx.out() << (flag_bool(args, "--json") ? render::action_json(*row) : render::action_start_text(*row, token));
  return {};
}

auto action_end(context& ctx, const cli::match_result& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const outcome_text = flag_string(args, "--outcome").value_or(std::string{"ok"});
  auto const result       = aa::outcome_from_text(outcome_text);
  if (!result.has_value()) {
    return std::unexpected(invalid_input_error(std::format("unknown --outcome '{}'", outcome_text)));
  }

  auto const action_id = flag_int(args, "--action").value_or(0);
  auto const summary   = flag_string(args, "--summary");
  auto const closed    = aa::end_action(**conn, action_id, *result, view(summary));
  if (!closed) {
    return std::unexpected(verb_error("endAction", closed.error()));
  }
  auto const row = aa::get_action_by_id(**conn, action_id);
  if (!row) {
    return std::unexpected(verb_error("getActionById", row.error()));
  }

  ctx.out() << (flag_bool(args, "--json") ? render::action_json(*row) : render::action_end_text(action_id, *result));
  return {};
}

} // namespace planar::cmd::agent::handlers
