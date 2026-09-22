/// @file unlink.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.unlink`.

module planar.cmd.planar.handlers.unlink;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.external;
import planar.engine.runtime;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace link    = engine::external::link;
namespace session = engine::runtime::session;

namespace {

/// @brief The vendor identity to attribute the audit row to.
///
/// Reads `$PLANAR_VENDOR` through the CONTEXT's env-lookup rather than
/// calling `planar.engine.runtime.session`'s `vendor_from_env()`, which
/// reaches the real process environment via `std::getenv`. The rule this
/// binary holds — `planar.cmd.planar.context`'s header — is that
/// `process_env()` is the only place that touches it, so a handler test
/// can run against a scratch map. The semantics are the Zig original's,
/// which are NOT "unset -> default": unset AND empty both yield `cli`.
/// @param ctx The invocation context.
/// @return The vendor identity.
auto vendor_from(const context& ctx) -> std::string {
  auto const value = ctx.env()("PLANAR_VENDOR");
  if (!value.has_value() || value->empty()) {
    return "cli";
  }
  return *value;
}

/// @brief The vendor's own session id, when it published one.
///
/// Same env-lookup discipline as `vendor_from`, and the same unset/empty
/// equivalence — an empty `$PLANAR_VENDOR_SESSION_ID` is "absent", not a
/// session keyed on the empty string.
/// @param ctx The invocation context.
/// @return The vendor session id, or unset.
auto vendor_session_id_from(const context& ctx) -> std::optional<std::string> {
  auto const value = ctx.env()("PLANAR_VENDOR_SESSION_ID");
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return value;
}

/// @brief Record the unlink in the session timeline, swallowing every
/// failure.
///
/// Best-effort by contract, not by accident: the row it mirrors is written
/// AFTER the delete has already committed, so reporting a failure here
/// would tell the operator the unlink failed when it did not. The Zig
/// original's helper is `catch return` on every step; this is the same
/// shape with the errors named as values.
/// @param ctx The invocation context.
/// @param conn The open connection.
/// @param link_id The link that was removed.
auto append_audit(const context& ctx, db::connection& conn, std::int64_t link_id) -> void {
  auto const sid = session::ensure_active(conn, vendor_from(ctx), vendor_session_id_from(ctx));
  if (!sid) {
    return;
  }
  auto const body = std::format("unlink: removed external link {}", link_id);
  // Discarded deliberately — see this function's doc comment.
  static_cast<void>(session::append_entry(conn, *sid, "action", body));
}

} // namespace

auto unlink(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // The database is opened BEFORE the id is parsed, and the order is
  // observable: `planar unlink abc` exits 2 having already CREATED and
  // migrated `$PLANAR_DB` (oracle-captured against a scratch root). Moving
  // the cheap validation first would be tidier and would diverge.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // `--scope` is read by neither implementation. See this leaf's module
  // header — link verbs are unguarded by design.
  auto const raw     = positional_string(args, "link-id").value_or(std::string{});
  auto const link_id = cliapp::parse_int64_zig(raw);
  if (!link_id.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("invalid link id '{}'", raw)));
  }

  // Resolved first so a missing id yields the precise message BEFORE the
  // delete would otherwise report the same thing — the Zig original's own
  // comment. Both paths produce identical bytes today; the lookup is what
  // makes that true rather than coincidental.
  auto const found = link::show(**conn, *link_id);
  if (!found) {
    if (found.error() == link::link_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("link {} not found", *link_id)));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("unlink: lookup link {}: QueryFailed", *link_id)));
  }

  auto const removed = link::remove(**conn, *link_id);
  if (!removed) {
    if (removed.error() == link::link_error::not_found) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("link {} not found", *link_id)));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("unlink: delete link {}: QueryFailed", *link_id)));
  }

  append_audit(ctx, **conn, *link_id);

  // Two hand-rolled payloads, not an engine renderer: `engine_external`
  // has no render surface (the Zig original emits these from the handler
  // too, via `std.json.Stringify` on a two-field struct and a `print`), so
  // the terminator belongs to this layer on BOTH paths. Both lines are
  // oracle-captured verbatim.
  if (flag_bool(args, "--json")) {
    ctx.out() << std::format("{{\"ok\":true,\"id\":{}}}\n", *link_id);
  } else {
    ctx.out() << std::format("unlinked: external link {} removed\n", *link_id);
  }
  return {};
}

/// @brief Declare the `unlink` leaf.
///
/// MOVED HERE FROM `tree.cpp` at M11.3f; it was hand-written there
/// since task 6106 and separately described by a `node_spec` that
/// `apply_surface` skipped. The two halves agreed field for field.
///
/// This binary declares no `ext` or `sync` group. If you came here
/// looking for one — `unlink` is the nearest surviving neighbor of where
/// they used to live — see `planar.cmd.planar.surface`'s "THE `ext` AND
/// `sync` FAMILIES" section (`src/cmd/planar/surface.cppm`) for where they
/// went and why (task 6666).
///
/// `<link-id>` is a plain STRING positional with no validator,
/// deliberately: the oracle declared `.kind = .string` and its handler
/// converted with Zig's `parseInt`, so `unlink abc` reaches the
/// HANDLER's refusal (exit 1) rather than a parse error (exit 2).
/// Attaching `zig_int_validator` here would move that failure a layer
/// earlier and change both the message and the exit code. The parity
/// suite pins the whole `1_0` / `007` / `+12` / `_10` / `10_` / `0x10`
/// / overflow table through this leaf.
auto declare_unlink(CLI::App& root) -> void {
  CLI::App* unlink = root.add_subcommand(
      "unlink",
      "Remove an external_links row by its link id.\n\n  Associated sync_events rows are detached by setting link_id to null\n  "
      "rather than cascade-deleted; they are no longer reachable through\n  the deleted link's audit trail.");
  add_string(*unlink, "--scope", "Scope for the cross-scope guard (currently informational)");
  add_json(*unlink);
  add_positional_described(*unlink, "link-id", "External-link id (integer)");
}

} // namespace planar::cmd::handlers
