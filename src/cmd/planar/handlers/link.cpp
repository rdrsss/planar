/// @file link.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.link`. See link.cppm
/// for the stale-oracle-comment finding, the recorded `--propagate`
/// divergence, and the two refs that parse in opposite directions.

module planar.cmd.planar.handlers.link;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.external;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace link_ns   = engine::external::link;
namespace system_ns = engine::external::system;

namespace {

/// @brief A `<kind>:<id>` entity ref, kind kept as RAW TEXT.
struct entity_ref {
  std::string  kind; ///< The kind token, not yet validated.
  std::int64_t id{}; ///< The local row id.
};

/// @brief Parse `<kind>:<id>`, splitting on the LAST colon.
/// @param text The positional ref.
/// @return The ref, or unset when malformed.
auto parse_entity_ref(std::string_view text) -> std::optional<entity_ref> {
  auto const at = text.rfind(':');
  if (at == std::string_view::npos || at == 0 || at + 1 >= text.size()) {
    return std::nullopt;
  }
  auto const parsed = cliapp::parse_int64_zig(text.substr(at + 1));
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  return entity_ref{.kind = std::string{text.substr(0, at)}, .id = *parsed};
}

/// @brief A `<slug>:<external-id>` target ref.
struct to_ref {
  std::string slug;        ///< The registered system's slug.
  std::string external_id; ///< The external ticket id; MAY contain colons.
};

/// @brief Parse `<slug>:<external-id>`, splitting on the FIRST colon.
///
/// The opposite direction from `parse_entity_ref` above, in the same
/// handler, and not an inconsistency: an external id may itself contain
/// colons while a slug never does, so the first colon is the only correct
/// boundary here and the last is the only correct one there.
/// @param text The `--to` value.
/// @return The ref, or unset when there is no colon or either side is empty.
auto parse_to_ref(std::string_view text) -> std::optional<to_ref> {
  auto const at = text.find(':');
  if (at == std::string_view::npos || at == 0 || at + 1 >= text.size()) {
    return std::nullopt;
  }
  return to_ref{.slug = std::string{text.substr(0, at)}, .external_id = std::string{text.substr(at + 1)}};
}

} // namespace

auto link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // BEFORE any write. See link.cppm: writing the row first would make the
  // message's own "re-run without --propagate" advice fail at exit 6.
  if (cliapp::flag_bool(args, "--propagate")) {
    return std::unexpected(error_from_body(domain_error_kind::not_implemented,
                                           "link --propagate: not implemented in this build (the `ext propagate` "
                                           "surface is unported); re-run without --propagate"));
  }

  auto const raw_ref = positional_string(args, "ref").value_or(std::string{});
  auto const ref     = parse_entity_ref(raw_ref);
  if (!ref) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("invalid entity ref '{}'; expected kind:integer-id", raw_ref)));
  }

  auto const raw_to = flag_string(args, "--to").value_or(std::string{});
  auto const target = parse_to_ref(raw_to);
  if (!target) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("invalid --to value '{}'; expected <system-slug>:<external-id>", raw_to)));
  }

  auto const sys = system_ns::show_by_slug(**conn, target->slug);
  if (!sys) {
    return std::unexpected(
        error_from_body(domain_error_kind::not_found, std::format("external system '{}' not found", target->slug)));
  }

  // The CLI's defaults, NOT `create_args`' — see link.cppm.
  auto const role_text = flag_string(args, "--role").value_or("reference");
  auto const sync_text = flag_string(args, "--sync").value_or("read-only");

  auto const role = link_ns::link_role_from_text(role_text);
  if (!role) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("invalid --role '{}'", role_text)));
  }
  auto const direction = link_ns::sync_direction_from_text(sync_text);
  if (!direction) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("invalid --sync '{}'", sync_text)));
  }
  // The kind is validated; the ROW is not. `link task:999` succeeds. See
  // link.cppm — an oracle defect, reproduced under D2.
  auto const entity_kind = link_ns::external_entity_kind_from_text(ref->kind);
  if (!entity_kind) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("unsupported entity kind '{}'", ref->kind)));
  }

  // `never`, not `ok`: nothing has been exchanged with the remote. `ext
  // create` writes `ok` because it just created the counterpart.
  auto const stored = link_ns::create(**conn, link_ns::create_args{
                                                  .entity_kind    = *entity_kind,
                                                  .entity_id      = ref->id,
                                                  .system_id      = sys->id,
                                                  .external_id    = target->external_id,
                                                  .role           = *role,
                                                  .direction      = *direction,
                                                  .initial_status = link_ns::sync_status::never,
                                              });
  if (!stored) {
    if (stored.error() == link_ns::link_error::link_exists) {
      return std::unexpected(error_from_body(domain_error_kind::slug_conflict, std::format("link already exists for {}:{} on {}",
                                                                                           ref->kind, ref->id, target->slug)));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "link create: QueryFailed"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    std::string out = std::format(R"({{"ok":true,"link_id":{},"entity_kind":)", stored->id);
    json_text::append_json_string(out, ref->kind);
    out += std::format(R"(,"entity_id":{},"external_id":)", ref->id);
    json_text::append_json_string(out, target->external_id);
    out += std::format(R"(,"system_id":{}}})", sys->id);
    out += "\n";
    ctx.out() << out;
    return {};
  }

  // A U+2192 RIGHTWARDS ARROW, TWO spaces before the parenthesis, and the
  // pair rendered `<sync> <role>` — all three oracle-captured:
  // `linked task:1 → jira-demo:DEMO-1  (link id: 1, read-only reference)`.
  ctx.out() << std::format("linked {}:{} \xe2\x86\x92 {}:{}  (link id: {}, {} {})\n", ref->kind, ref->id, target->slug,
                           target->external_id, stored->id, sync_text, role_text);
  return {};
}

} // namespace planar::cmd::handlers
