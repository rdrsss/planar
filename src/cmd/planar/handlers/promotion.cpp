/// @file promotion.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.promotion`. See
/// promotion.cppm for why the pre-read makes most of the engine's own
/// messages unreachable, and why `--from` is parsed and discarded.

module planar.cmd.planar.handlers.promotion;

import std;
import planar.cliapp.args;
import planar.engine.entitylink;
import planar.engine.promotion;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace link_ns = engine::entitylink;
namespace prom_ns = engine::promotion;

namespace {

/// @brief The Zig `@errorName` spelling of a `promote_error`.
///
/// These strings are OBSERVABLE: the oracle's pre-read failure path is
/// `exit.die(ctx, e, "reading entity scope: {s}", .{@errorName(e)})`, so the
/// enumerator name of `zig/src/engine/promotion.zig`'s `Error` set reaches
/// stderr verbatim. `NotFound` is the one an operator actually hits.
/// @param err The engine failure.
/// @return The Zig error name.
auto promote_error_name(prom_ns::promote_error err) -> std::string_view {
  switch (err) {
  case prom_ns::promote_error::not_found:
    return "NotFound";
  case prom_ns::promote_error::invalid_scope:
    return "InvalidScope";
  case prom_ns::promote_error::scope_unchanged:
    return "ScopeUnchanged";
  case prom_ns::promote_error::unsupported_scope:
    return "UnsupportedScope";
  case prom_ns::promote_error::slug_not_found:
    return "SlugNotFound";
  case prom_ns::promote_error::query_failed:
    break;
  }
  return "QueryFailed";
}

/// @brief The exit bucket a pre-read failure lands in.
///
/// Neither `NotFound` nor `InvalidScope` is in the Zig exit map's 2-bucket
/// (which holds only `InvalidEntityRef` / `InvalidInput` and the parse
/// family), so both fall through its `else => 1`. Captured, not inferred.
/// @param err The engine failure.
/// @return The domain-error kind whose bucket matches the oracle's.
auto pre_read_kind(prom_ns::promote_error err) -> domain_error_kind {
  static_cast<void>(err);
  return domain_error_kind::generic_failure;
}

/// @brief The entity ref both leaves take, decoded to the `kind:id` form the
/// promotion engine needs.
struct entity_ref {
  std::string_view kind; ///< The entity kind's text spelling.
  std::int64_t     id;   ///< The entity's row id.
};

/// @brief Decode the positional ref, refusing the two forms the oracle
/// refuses.
///
/// A SLUG ref is a distinct refusal from a MALFORMED one and carries the
/// verb's own name in its wording, which is why `verb` is a parameter rather
/// than a constant: the oracle prints `promote requires a numeric id` and
/// `demote requires a numeric id` from two separate handlers.
/// @param raw The positional as the operator typed it.
/// @param verb The verb name, interpolated into the slug refusal.
/// @return The decoded ref, or the refusal.
auto decode_ref(const std::string& raw, std::string_view verb) -> std::expected<entity_ref, domain_error> {
  auto parsed = link_ns::parse_ref(raw);
  if (!parsed) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("invalid ref '{}': expected kind:id", raw)));
  }
  auto const* id_form = std::get_if<link_ns::parsed_ref::id_ref>(&parsed->value);
  if (id_form == nullptr) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("{} requires a numeric id (got slug '{}')", verb, raw)));
  }
  return entity_ref{.kind = link_ns::entity_kind_to_text(id_form->kind), .id = id_form->id};
}

} // namespace

auto promote(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const raw = cliapp::positional_string(args, "ref").value_or(std::string{});
  auto const ref = decode_ref(raw, "promote");
  if (!ref) {
    return std::unexpected(ref.error());
  }
  // `--to` is `required` in the surface, so CLI11 refuses a missing one
  // before the handler runs; the fallback keeps the read total.
  auto const to_scope = cliapp::flag_string(args, "--to").value_or(std::string{});

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // The pre-read runs BEFORE the engine call and is what an operator's
  // failure message actually comes from for every bad entity. See the
  // module header.
  auto const previous = prom_ns::read_entity_scope(**conn, ref->kind, ref->id);
  if (!previous) {
    return std::unexpected(error_from_body(pre_read_kind(previous.error()),
                                           std::format("reading entity scope: {}", promote_error_name(previous.error()))));
  }

  auto const moved = prom_ns::promote(**conn, {.kind = ref->kind, .id = ref->id, .to_scope = to_scope});
  if (!moved) {
    switch (moved.error()) {
    case prom_ns::promote_error::not_found:
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no {} with id {}", ref->kind, ref->id)));
    case prom_ns::promote_error::slug_not_found:
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, std::format("no association with slug '{}'", to_scope)));
    case prom_ns::promote_error::scope_unchanged:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                             std::format("{}:{} is already at scope '{}'", ref->kind, ref->id, to_scope)));
    case prom_ns::promote_error::unsupported_scope:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "repo: scopes are not supported"));
    case prom_ns::promote_error::invalid_scope:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("unsupported entity kind '{}'", ref->kind)));
    case prom_ns::promote_error::query_failed:
      break;
    }
    // The oracle's catch-all is `exit.die(ctx, e, "promote: {s}",
    // .{@errorName(e)})`, so the TAG is interpolated rather than fixed. Only
    // `QueryFailed` reaches here — every other arm is named above — but the
    // name is taken from the error rather than spelled as a literal, so a
    // future enumerator does not silently start reporting the wrong tag.
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("promote: {}", promote_error_name(moved.error()))));
  }

  // Read the NEW scope. The oracle re-reads rather than synthesising it, so
  // a `--json` envelope reports what the row now holds rather than what the
  // caller asked for.
  auto const current = prom_ns::read_entity_scope(**conn, ref->kind, ref->id);
  if (!current) {
    return std::unexpected(error_from_body(pre_read_kind(current.error()), std::format("reading entity scope after promote: {}",
                                                                                       promote_error_name(current.error()))));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? prom_ns::render_scope_change_json(ref->kind, ref->id, *current, *previous)
                                                  : prom_ns::render_promote_text(ref->kind, ref->id, to_scope, *previous));
  return {};
}

auto demote(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const raw = cliapp::positional_string(args, "ref").value_or(std::string{});
  auto const ref = decode_ref(raw, "demote");
  if (!ref) {
    return std::unexpected(ref.error());
  }
  // Read and DISCARDED — see the module header. The engine demotes
  // unconditionally, so a bogus `--from` is not a refusal.
  static_cast<void>(cliapp::flag_string(args, "--from"));

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const previous = prom_ns::read_entity_scope(**conn, ref->kind, ref->id);
  if (!previous) {
    return std::unexpected(error_from_body(pre_read_kind(previous.error()),
                                           std::format("reading entity scope: {}", promote_error_name(previous.error()))));
  }

  auto const moved = prom_ns::demote(**conn, ref->kind, ref->id);
  if (!moved) {
    switch (moved.error()) {
    case prom_ns::promote_error::not_found:
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("no {} with id {}", ref->kind, ref->id)));
    case prom_ns::promote_error::scope_unchanged:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                             std::format("{}:{} is already at global scope", ref->kind, ref->id)));
    case prom_ns::promote_error::invalid_scope:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("unsupported entity kind '{}'", ref->kind)));
    case prom_ns::promote_error::slug_not_found:
    case prom_ns::promote_error::unsupported_scope:
    case prom_ns::promote_error::query_failed:
      break;
    }
    // Same shape as `promote`'s catch-all above, and here the interpolation
    // is doing more work: `demote` resolves no slug and reads no target
    // scope, so `SlugNotFound` and `UnsupportedScope` are unreachable
    // through it — but if either ever did surface, the oracle would print
    // its own tag and so does this.
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("demote: {}", promote_error_name(moved.error()))));
  }

  auto const current = prom_ns::read_entity_scope(**conn, ref->kind, ref->id);
  if (!current) {
    return std::unexpected(error_from_body(pre_read_kind(current.error()), std::format("reading entity scope after demote: {}",
                                                                                       promote_error_name(current.error()))));
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? prom_ns::render_scope_change_json(ref->kind, ref->id, *current, *previous)
                                                  : prom_ns::render_demote_text(ref->kind, ref->id, *previous));
  return {};
}

} // namespace planar::cmd::handlers
