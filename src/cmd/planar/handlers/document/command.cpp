/// @file src/cmd/planar/handlers/document/command.cpp
/// @brief Implementation of the authoritative document projection boundary.

module planar.cmd.planar.handlers.document;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.document_authority;
import planar.cmd.planar.context;
import planar.cmd.planar.declare;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {
namespace {

auto json(std::string_view value) -> std::string {
  std::string out{"\""};
  for (unsigned char c : value) {
    switch (c) {
    case '\"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20U)
        out += std::format("\\u{:04x}", c);
      else
        out.push_back(static_cast<char>(c));
    }
  }
  out += '"';
  return out;
}

auto render(const document_authority::document& doc) -> std::string {
  std::string out = std::format(
      "{{\"contract_version\":\"block-document-v1\",\"source_uuid\":{},\"document_id\":{},\"content_revision\":{},\"passages\":[",
      json(doc.source_uuid), json(doc.id), json(doc.content_revision));
  for (std::size_t i = 0; i < doc.passages.size(); ++i) {
    auto const& p = doc.passages[i];
    if (i != 0)
      out += ',';
    out += std::format("{{\"key\":{},\"kind\":{},\"text\":{},\"source\":{{\"kind\":{},\"id\":{},\"path\":{},\"start_line\":{},"
                       "\"end_line\":{}}}}}",
                       json(p.key), json(p.kind), json(p.text), json(p.source.kind), json(p.source.id), json(p.source.path),
                       p.source.start_line, p.source.end_line);
  }
  out += "]}";
  return out;
}

auto source(const cliapp::parsed_args& args) -> std::expected<std::pair<std::string, std::int64_t>, domain_error> {
  auto kind = cliapp::flag_string(args, "--kind");
  auto id   = cliapp::flag_int(args, "--id");
  if (!kind || !id)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--kind and --id are required"));
  return std::pair{*kind, *id};
}

/// @brief The stable reason string a rejected range reports for `err`.
/// @param err The validator's refusal.
/// @return The reason token, spelled as the library enumerator.
auto rejection_reason(document_authority::error err) -> std::string_view {
  switch (err) {
  case document_authority::error::invalid_kind:
    return "invalid_kind";
  case document_authority::error::not_found:
    return "not_found";
  case document_authority::error::query_failed:
    return "query_failed";
  case document_authority::error::stale_revision:
    return "stale_revision";
  case document_authority::error::missing_boundary:
    return "missing_boundary";
  case document_authority::error::foreign_key:
    return "foreign_key";
  case document_authority::error::reversed_range:
    return "reversed_range";
  case document_authority::error::invalid_utf8_boundary:
    return "invalid_utf8_boundary";
  case document_authority::error::noncontiguous_covered_keys:
    return "noncontiguous_covered_keys";
  case document_authority::error::forged_quote:
    return "forged_quote";
  }
  return "unknown";
}

auto authoritative_document(db::connection& conn, std::string_view kind, std::int64_t id)
    -> std::expected<document_authority::document, domain_error> {
  auto projected = document_authority::project(conn, kind, id);
  if (!projected) {
    if (projected.error() == document_authority::error::invalid_kind)
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "document kind must be plan or artifact"));
    auto error_kind = projected.error() == document_authority::error::not_found ? domain_error_kind::not_found
                                                                                : domain_error_kind::generic_failure;
    return std::unexpected(error_from_body(error_kind, "document projection failed"));
  }
  return std::move(*projected);
}

auto open_authority_source(context& ctx) -> std::expected<db::connection, domain_error> {
  auto opened = db::connection::open_read_only(ctx.db_path().string());
  if (!opened)
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("failed to open document source {} read-only: {}",
                                                                        ctx.db_path().string(), opened.error().message_)));

  auto const compatibility = db::assert_schema_compatible(*opened);
  if (!compatibility)
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("document source schema check failed: {}", compatibility.error().message_)));
  if (compatibility->verdict_ == db::schema_compatibility::behind)
    return std::unexpected(
        error_from_body(domain_error_kind::schema_version_behind,
                        std::format("document source schema version {} is older than this binary requires ({})",
                                    compatibility->live_, compatibility->embedded_max_)));
  if (compatibility->verdict_ == db::schema_compatibility::ahead)
    return std::unexpected(
        error_from_body(domain_error_kind::schema_version_ahead,
                        std::format("document source schema version {} is newer than this binary supports ({})",
                                    compatibility->live_, compatibility->embedded_max_)));
  if (compatibility->verdict_ == db::schema_compatibility::gap)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "document source schema has a missing migration"));
  return std::move(*opened);
}

} // namespace

auto document_project(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto src = source(args);
  if (!src)
    return std::unexpected(src.error());
  auto conn = open_authority_source(ctx);
  if (!conn)
    return std::unexpected(conn.error());
  auto doc = authoritative_document(*conn, src->first, src->second);
  if (!doc)
    return std::unexpected(doc.error());
  ctx.out() << render(*doc) << '\n';
  return {};
}

auto document_validate_range(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto src = source(args);
  if (!src)
    return std::unexpected(src.error());
  auto conn = open_authority_source(ctx);
  if (!conn)
    return std::unexpected(conn.error());
  auto doc = authoritative_document(*conn, src->first, src->second);
  if (!doc)
    return std::unexpected(doc.error());
  auto revision     = cliapp::flag_string(args, "--content-revision");
  auto start_key    = cliapp::flag_string(args, "--start-key");
  auto end_key      = cliapp::flag_string(args, "--end-key");
  auto start_offset = cliapp::flag_int(args, "--start-offset");
  auto end_offset   = cliapp::flag_int(args, "--end-offset");
  // Every rejection is the caller's evidence disagreeing with the snapshot
  // just projected, so it is `invalid_input` (exit 2): the recovery is to
  // re-project and resend, never to retry unchanged. Not `sync_conflict`
  // (exit 3), whose documented recovery is `sync resolve` on the
  // operational plane, and not the generic 1 that a missing row or a
  // database failure reports.
  auto reject = [&](std::string_view code) -> handler_result {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("document range rejected: {}", code)));
  };
  if (!revision || *revision != doc->content_revision)
    return reject(rejection_reason(document_authority::error::stale_revision));
  if (!start_key || !end_key || !start_offset || !end_offset)
    return reject(rejection_reason(document_authority::error::missing_boundary));
  // A negative offset wraps to a value past every passage, which the
  // validator refuses as `invalid_utf8_boundary` -- the same outcome the
  // handler-local copy this replaced produced.
  document_authority::range_evidence const evidence{.content_revision = *revision,
                                                    .start_key        = *start_key,
                                                    .start_offset     = static_cast<std::size_t>(*start_offset),
                                                    .end_key          = *end_key,
                                                    .end_offset       = static_cast<std::size_t>(*end_offset),
                                                    .covered_keys     = cliapp::flag_strings(args, "--covered-key"),
                                                    .segment_quotes   = cliapp::flag_strings(args, "--segment-quote")};
  auto                                     validated = document_authority::validate(*doc, evidence);
  if (!validated)
    return reject(rejection_reason(validated.error()));
  std::string out = std::format("{{\"contract_version\":\"block-range-validation-v1\",\"source_uuid\":{},\"document_id\":{},"
                                "\"content_revision\":{},\"covered_keys\":[",
                                json(doc->source_uuid), json(validated->document_id), json(validated->content_revision));
  for (std::size_t i = 0; i < validated->covered_keys.size(); ++i) {
    if (i != 0)
      out += ',';
    out += json(validated->covered_keys[i]);
  }
  out += "],\"segment_quotes\":[";
  for (std::size_t i = 0; i < validated->segment_quotes.size(); ++i) {
    if (i != 0)
      out += ',';
    out += json(validated->segment_quotes[i]);
  }
  out += std::format("],\"normalized_quote\":{}}}", json(validated->normalized_quote));
  ctx.out() << out << '\n';
  return {};
}

auto declare_document(CLI::App& root) -> void {
  auto* document     = root.add_subcommand("document", "Project and validate authoritative block documents.");
  auto  source_flags = [](CLI::App& app) {
    add_string_required(app, "--kind", "Source entity kind: plan or artifact");
    add_int_required(app, "--id", "Source entity id");
  };
  auto* project = document->add_subcommand("project", "Emit an authoritative block-document-v1 projection.");
  source_flags(*project);
  add_json(*project, "Emit machine-readable JSON instead of text");
  auto* validate = document->add_subcommand("validate-range", "Validate an adjacent revision-bound passage range.");
  source_flags(*validate);
  add_string_required(*validate, "--content-revision", "Revision from the projection being validated against");
  add_string_required(*validate, "--start-key", "Key of the passage where the range starts");
  add_int_required(*validate, "--start-offset", "Byte offset within the start passage");
  add_string_required(*validate, "--end-key", "Key of the passage where the range ends");
  add_int_required(*validate, "--end-offset", "Byte offset within the end passage");
  add_string_list(*validate, "--covered-key", "Covered passage key; repeat for each passage from start to end, in order");
  add_string_list(*validate, "--segment-quote", "Projected text of one covered segment; repeat once per covered key");
  add_json(*validate, "Emit machine-readable JSON instead of text");
}

} // namespace planar::cmd::handlers
