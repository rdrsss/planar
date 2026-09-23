/// @file src/cmd/planar/handlers/links/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.links`.

module planar.cmd.planar.handlers.links;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.entitylink;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.links.add;
import planar.cmd.planar.handlers.links.list;
import planar.cmd.planar.handlers.links.remove;
import planar.cmd.planar.handlers.links.trail;

namespace planar::cmd::handlers {

namespace el = engine::entitylink;

namespace {

/// @brief The Zig error name for an `entity_link_error`.
///
/// The fallback arms of every one of these handlers die with `exit.die(ctx,
/// e, "<verb>: {s}", .{@errorName(e)})`, so an unmapped failure surfaces
/// Zig's CamelCase error TAG. Transcribed from
/// zig/src/engine/entitylink.zig's error set, whose members line up
/// one-for-one with this port's `entity_link_error`.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(el::entity_link_error err) -> std::string_view {
  switch (err) {
  case el::entity_link_error::not_found:
    return "NotFound";
  case el::entity_link_error::link_exists:
    return "LinkExists";
  case el::entity_link_error::unsupported_scope:
    return "UnsupportedScope";
  case el::entity_link_error::invalid_ref:
    return "InvalidRef";
  case el::entity_link_error::endpoint_not_found:
    return "EndpointNotFound";
  case el::entity_link_error::query_failed:
    return "QueryFailed";
  case el::entity_link_error::audit_write_failed:
    // zig `policy.audit.Error` has the single member `WriteFailed`, which
    // the Zig call sites `try` straight out of the engine module.
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief The generic `<verb>: <ZigTag>` fallback refusal.
///
/// EVERY member lands in the generic bucket (exit 1): none of the tags this
/// family raises has an arm in `zig/src/cmd/planar/exit.zig`'s `codeFor`.
/// In particular `link_exists` is NOT `already_exists` — that bucket is
/// exit 6 and the oracle answers 1. See this module's header.
/// @param err The engine error.
/// @param verb The verb name to lead with.
/// @return The refusal.
auto fallback_error(el::entity_link_error err, std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: {}", verb, zig_error_name(err)));
}

/// @brief The two refusals `entitylink::add` raises that carry oracle
/// wording rather than a Zig tag, plus the fallback.
///
/// `endpoint_not_found` re-queries which SIDE is missing and names that
/// ref. A bare "endpoint not found" would leave the operator re-reading
/// both halves of their command to work out which id was wrong, and a
/// mistyped id is the whole reason the check exists.
/// @param conn An open connection, for the missing-endpoint re-query.
/// @param add_args The arguments the failed `add` was given.
/// @param err The engine error.
/// @param relationship_text The relationship as the OPERATOR spelled it.
/// @param verb The verb name for the fallback message.
/// @param unicode_arrow True only for `plan link`; see this module's header.
/// @return The refusal.
auto map_add_error(db::connection& conn, const el::entity_link_add_args& add_args, el::entity_link_error err,
                   std::string_view relationship_text, std::string_view verb, bool unicode_arrow) -> domain_error {
  switch (err) {
  case el::entity_link_error::link_exists:
    return error_from_body(domain_error_kind::generic_failure,
                           el::render_link_exists_error(add_args.from_kind, add_args.from_id, add_args.to_kind, add_args.to_id,
                                                        relationship_text, unicode_arrow));
  case el::entity_link_error::unsupported_scope:
    return error_from_body(domain_error_kind::generic_failure, "scoped entity links not yet supported (M3)");
  case el::entity_link_error::endpoint_not_found: {
    // `from` is checked before `to`, and an absent answer defaults to
    // `from` — same fallback the oracle takes (`orelse .from`).
    auto const side = el::missing_endpoint_of(conn, add_args).value_or(el::missing_endpoint::from);
    auto const kind = side == el::missing_endpoint::from ? add_args.from_kind : add_args.to_kind;
    auto const id   = side == el::missing_endpoint::from ? add_args.from_id : add_args.to_id;
    return error_from_body(domain_error_kind::generic_failure, std::format("{}:{} not found", el::entity_kind_to_text(kind), id));
  }
  default:
    return fallback_error(err, verb);
  }
}

/// @brief One end of a link, decoded from a `kind:integer-id` positional.
struct decoded_ref {
  el::entity_kind kind; ///< The referenced entity's kind.
  std::int64_t    id;   ///< The referenced entity's id.
};

/// @brief Decode a `kind:integer-id` positional, with per-verb refusal
/// wording.
///
/// The three call-site families spell BOTH refusals differently and the
/// differences were captured by running each, not assumed:
///
///     links list       invalid ref 'x': expected kind:id (id must be an integer)
///                      links list requires a numeric id (got slug 'plan:p')
///     links add        invalid from-ref 'x': expected kind:integer-id
///                      slug refs are not supported; use kind:integer-id (e.g. task:42)
///     <entity> link    invalid ref 'x': expected kind:integer-id
///                      slug refs are not supported; use kind:integer-id (e.g. plan:42)
///
/// so both messages are parameters. Note `links add` even varies the
/// EXAMPLE between its two positionals (`task:42` for from, `plan:42` for
/// to).
/// @param raw The positional's raw text.
/// @param malformed_message The refusal for text that is not `kind:id` at all.
/// @param slug_message The refusal for a well-formed ref whose id is a slug.
/// @return The decoded ref, or `invalid_input` (exit 2).
auto decode_ref(std::string_view raw, std::string_view malformed_message, std::string_view slug_message)
    -> std::expected<decoded_ref, domain_error> {
  auto parsed = el::parse_ref(raw);
  if (!parsed) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::string{malformed_message}));
  }
  auto const* as_id = std::get_if<el::parsed_ref::id_ref>(&parsed->value);
  if (as_id == nullptr) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::string{slug_message}));
  }
  return decoded_ref{.kind = as_id->kind, .id = as_id->id};
}

/// @brief Parse the `--relationship` value, refusing an unrecognized one.
/// @param raw The flag's raw text.
/// @return The relationship, or `invalid_input` (exit 2).
auto decode_relationship(std::string_view raw) -> std::expected<el::relationship, domain_error> {
  auto const parsed = el::relationship_from_text(raw);
  if (!parsed) {
    // `blocks` reaches here too — it is the pre-migration-00033 spelling
    // and is REFUSED rather than aliased, because `A --blocks--> B` stored
    // "A depends on B" and re-accepting the word would restore exactly the
    // direction inversion migration 00033 exists to fix.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown relationship '{}'", raw)));
  }
  return *parsed;
}

/// @brief Parse a `<link-id>` positional the way `links remove`/`trail` do.
/// @param args The parsed arguments.
/// @return The id, or `invalid_input` (exit 2).
auto link_id_arg(const cliapp::parsed_args& args) -> std::expected<std::int64_t, domain_error> {
  auto const raw = cliapp::positional_string(args, "link-id");
  if (!raw.has_value()) {
    // Unreachable through the CLI11 tree (declared required), but an
    // absent id must never fall through to 0 and act on row 0.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "link id is required"));
  }
  auto const parsed = cliapp::parse_int64_zig(*raw);
  if (!parsed.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("link id must be an integer, got '{}'", *raw)));
  }
  return *parsed;
}

} // namespace

auto links_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before any argument validation — zig's handler opens with `try
  // runtime.ensureDb()`, so even a refused invocation leaves a
  // created-and-migrated database behind.
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // `--relationship` is declared REQUIRED on this leaf (unlike the three
  // `<entity> link` ones), so the parser refuses an absent one before
  // dispatch and this handler always sees a value.
  auto const rel_flag = cliapp::flag_string(args, "--relationship");
  if (!rel_flag.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--relationship is required"));
  }
  auto const relationship = decode_relationship(*rel_flag);
  if (!relationship) {
    return std::unexpected(relationship.error());
  }

  auto const from_raw = cliapp::positional_string(args, "from-ref").value_or("");
  auto const from     = decode_ref(from_raw, std::format("invalid from-ref '{}': expected kind:integer-id", from_raw),
                                   "slug refs are not supported; use kind:integer-id (e.g. task:42)");
  if (!from) {
    return std::unexpected(from.error());
  }
  auto const to_raw = cliapp::positional_string(args, "to-ref").value_or("");
  auto const to     = decode_ref(to_raw, std::format("invalid to-ref '{}': expected kind:integer-id", to_raw),
                                 "slug refs are not supported; use kind:integer-id (e.g. plan:42)");
  if (!to) {
    return std::unexpected(to.error());
  }

  el::entity_link_add_args const add_args{
      .from_kind     = from->kind,
      .from_id       = from->id,
      .to_kind       = to->kind,
      .to_id         = to->id,
      .relationship_ = *relationship,
      .scope         = std::nullopt,
  };
  auto created = el::add(**conn, add_args);
  if (!created) {
    return std::unexpected(map_add_error(**conn, add_args, created.error(), *rel_flag, "links add", false));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << el::render_links_add_json(*created, *rel_flag);
  } else {
    ctx.out() << el::render_links_add_text(*created, *rel_flag);
  }
  return {};
}

auto links_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const raw     = cliapp::positional_string(args, "ref").value_or("");
  auto const subject = decode_ref(raw, std::format("invalid ref '{}': expected kind:id (id must be an integer)", raw),
                                  std::format("links list requires a numeric id (got slug '{}')", raw));
  if (!subject) {
    return std::unexpected(subject.error());
  }

  // NO existence check. `links list task:999` succeeds at exit 0 with
  // `no links for task:999`; only the WRITE verbs check endpoints.
  //
  // Two half-queries, because the oracle's listing is an OR over both
  // endpoint columns and `list`'s filter ANDs its fields — one call with
  // both sides set would return only self-links.
  auto from_links = el::list(**conn, el::entity_link_list_filter{
                                         .from_kind = subject->kind,
                                         .from_id   = subject->id,
                                     });
  if (!from_links) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("links list (from-side): {}", zig_error_name(from_links.error()))));
  }
  auto to_links = el::list(**conn, el::entity_link_list_filter{
                                       .to_kind = subject->kind,
                                       .to_id   = subject->id,
                                   });
  if (!to_links) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("links list (to-side): {}", zig_error_name(to_links.error()))));
  }

  auto const rows = el::merge_directed(*from_links, *to_links);
  if (cliapp::flag_bool(args, "--json")) {
    // Written verbatim: this renderer owns its newlines and the empty
    // listing is ZERO BYTES, not `[]` and not a bare newline.
    ctx.out() << el::render_link_list_json(rows);
  } else {
    ctx.out() << el::render_link_list_text(rows, subject->kind, subject->id);
  }
  return {};
}

auto links_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const link_id = link_id_arg(args);
  if (!link_id) {
    return std::unexpected(link_id.error());
  }

  auto removed = el::remove(**conn, *link_id);
  if (!removed) {
    if (removed.error() == el::entity_link_error::not_found) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("entity link {} not found", *link_id)));
    }
    return std::unexpected(fallback_error(removed.error(), "links remove"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << el::render_links_remove_json(*link_id);
  } else {
    ctx.out() << el::render_links_remove_text(*link_id);
  }
  return {};
}

auto links_trail(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const link_id = link_id_arg(args);
  if (!link_id) {
    return std::unexpected(link_id.error());
  }

  auto rows = el::trail(**conn, *link_id);
  if (!rows) {
    if (rows.error() == el::entity_link_error::not_found) {
      // The link, not the trail. An EMPTY trail on an existing link is a
      // success (exit 0) — the two answers are distinct and so are their
      // exit codes.
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("entity link {} not found", *link_id)));
    }
    return std::unexpected(fallback_error(rows.error(), "links trail"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << el::render_trail_json(*rows);
  } else {
    ctx.out() << el::render_trail_text(*rows, *link_id);
  }
  return {};
}

auto entity_link_verb(context& ctx, const cliapp::parsed_args& args, engine::entitylink::entity_kind subject_kind,
                      std::string_view id_positional, std::string_view id_label, std::string_view json_key, std::string_view verb,
                      bool unicode_arrow) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // Ordering is oracle-captured: the subject id parses BEFORE the
  // `--relationship` presence check, so `plan link notanint task:2` with no
  // `--relationship` reports the plan id and not the flag.
  auto const subject_id = entity_id_arg(args, id_positional, id_label);
  if (!subject_id) {
    return std::unexpected(subject_id.error());
  }

  // Declared OPTIONAL on these three leaves but required by the handler,
  // which is why the refusal is this wording at exit 2 and not the
  // parser's `required flag missing` at exit 2 on STDOUT.
  auto const rel_flag = cliapp::flag_string(args, "--relationship");
  if (!rel_flag.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--relationship is required"));
  }
  auto const relationship = decode_relationship(*rel_flag);
  if (!relationship) {
    return std::unexpected(relationship.error());
  }

  auto const raw = cliapp::positional_string(args, "ref").value_or("");
  auto const to  = decode_ref(raw, std::format("invalid ref '{}': expected kind:integer-id", raw),
                              "slug refs are not supported; use kind:integer-id (e.g. plan:42)");
  if (!to) {
    return std::unexpected(to.error());
  }

  // `--scope` is READ BY NOBODY here, deliberately — see this module's
  // header. The oracle accepts any value, including one that resolves to
  // nothing, and links anyway.

  el::entity_link_add_args const add_args{
      .from_kind     = subject_kind,
      .from_id       = *subject_id,
      .to_kind       = to->kind,
      .to_id         = to->id,
      .relationship_ = *relationship,
      .scope         = std::nullopt,
  };
  auto created = el::add(**conn, add_args);
  if (!created) {
    return std::unexpected(map_add_error(**conn, add_args, created.error(), *rel_flag, verb, unicode_arrow));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << el::render_entity_link_json(json_key, *subject_id, *created, *rel_flag);
  } else {
    ctx.out() << el::render_entity_link_text(subject_kind, *subject_id, *created, *rel_flag);
  }
  return {};
}

auto declare_links(CLI::App& root) -> void {
  CLI::App* links = root.add_subcommand(
      "links", "Manage internal cross-cutting entity_links relationships.\n\n  Entity links record typed relationships between "
               "any two Planar\n  entities (e.g. a task cites an artifact, a plan blocks another\n  plan). This domain is "
               "distinct from the top-level link/unlink\n  commands, which operate on external-system ticket linkage.");
  links->require_subcommand(0);

  links_cli::attach_add(links);

  links_cli::attach_list(links);

  links_cli::attach_remove(links);

  links_cli::attach_trail(links);
}

} // namespace planar::cmd::handlers
