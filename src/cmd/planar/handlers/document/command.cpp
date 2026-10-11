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

auto declare_document(CLI::App& root) -> void {
  auto* document     = root.add_subcommand("document", "Project authoritative block documents.");
  auto  source_flags = [](CLI::App& app) {
    add_string_required(app, "--kind", "Source entity kind: plan or artifact");
    add_int_required(app, "--id", "Source entity id");
  };
  auto* project = document->add_subcommand("project", "Emit an authoritative block-document-v1 projection.");
  source_flags(*project);
  add_json(*project, "Emit machine-readable JSON instead of text");
}

} // namespace planar::cmd::handlers
