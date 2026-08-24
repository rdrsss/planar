/// @file assoc.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.assoc`.

module planar.cmd.planar.handlers.assoc;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.identity;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace id = engine::identity;

namespace {

/// @brief The Zig error name for an `association_error`.
///
/// zig's handler fails with `exit.die(ctx, e, "association create: {s}",
/// .{@errorName(e)})` — note "association create", NOT the "assoc create"
/// the operator typed; the message spells the ENGINE module's name and
/// the oracle capture confirms it (`error: association create:
/// SlugConflict`). Reproducing the operator's spelling here would look
/// tidier and break any script matching the real bytes.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(id::association_error err) -> std::string_view {
  switch (err) {
  case id::association_error::not_found:
    return "NotFound";
  case id::association_error::slug_conflict:
    return "SlugConflict";
  case id::association_error::unknown_kind:
    return "UnknownKind";
  case id::association_error::already_member:
    return "AlreadyMember";
  case id::association_error::not_a_member:
    return "NotAMember";
  case id::association_error::query_failed:
    return "QueryFailed";
  }
  return "Unknown";
}

/// @brief Map an `association_error` onto this binary's exit-code bucket,
/// per zig/src/cmd/planar/exit.zig's `codeFor`.
///
/// `SlugConflict` maps to 6 (oracle-confirmed: a duplicate slug exits 6);
/// every other member has no arm in `codeFor` and falls to `else => 1`.
/// @param err The engine error.
/// @return The mapped failure.
auto map_association_error(id::association_error err) -> domain_error {
  auto const kind =
      err == id::association_error::slug_conflict ? domain_error_kind::slug_conflict : domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("association create: {}", zig_error_name(err)));
}

} // namespace

auto assoc_create(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before validating `--kind`. zig opens with `const d = try
  // runtime.ensureDb();`, so a refused `--kind nope` still creates and
  // migrates the database. Same ordering, same reason, as `plan create`'s.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto slug = cliapp::positional_string(args, "slug");
  if (!slug) {
    // Unreachable through the declared tree (the positional is required),
    // but the engine's `slug` is a non-optional `std::string` and an
    // empty default would write a blank-slug row. Refuse instead.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "assoc create: slug is required"));
  }

  id::create_args create{.slug = std::move(*slug), .name = cliapp::flag_string(args, "--name")};

  // Parse ONLY when the flag is present — an absent `--kind` leaves the
  // engine's `ad_hoc` default in place and must never reach the
  // unknown-kind refusal. Mirrors zig's `if (args.kind) |k| { ... }`.
  if (auto const kind_raw = cliapp::flag_string(args, "--kind")) {
    auto const kind = id::association_kind_from_text(*kind_raw);
    if (!kind) {
      // Exit 2 here, unlike `plan create`'s `--status` at 1. See this
      // module's header.
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown kind '{}'", *kind_raw)));
    }
    create.kind = *kind;
  }

  auto created = id::create(**conn, create);
  if (!created) {
    return std::unexpected(map_association_error(created.error()));
  }

  // Per-renderer terminator contract — see the two `@return` blocks on
  // `render_text` / `render_json` in `association.cppm`.
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << id::render_json(*created) << '\n';
  } else {
    ctx.out() << id::render_text(*created);
  }
  return {};
}

} // namespace planar::cmd::handlers
