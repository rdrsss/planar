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

struct passage {
  std::string  key;
  std::string  kind;
  std::string  text;
  std::string  source_kind;
  std::string  source_id;
  std::string  source_path;
  std::int64_t start_line;
  std::int64_t end_line;
};

struct document {
  std::string          source_uuid;
  std::string          id;
  std::string          revision;
  std::vector<passage> passages;
};

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

auto render(const document& doc) -> std::string {
  std::string out = std::format(
      "{{\"contract_version\":\"block-document-v1\",\"source_uuid\":{},\"document_id\":{},\"content_revision\":{},\"passages\":[",
      json(doc.source_uuid), json(doc.id), json(doc.revision));
  for (std::size_t i = 0; i < doc.passages.size(); ++i) {
    auto const& p = doc.passages[i];
    if (i != 0)
      out += ',';
    out += std::format("{{\"key\":{},\"kind\":{},\"text\":{},\"source\":{{\"kind\":{},\"id\":{},\"path\":{},\"start_line\":{},"
                       "\"end_line\":{}}}}}",
                       json(p.key), json(p.kind), json(p.text), json(p.source_kind), json(p.source_id), json(p.source_path),
                       p.start_line, p.end_line);
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

auto utf8_boundary(std::string_view value, std::size_t offset) -> bool {
  return offset <= value.size() && (offset == value.size() || (static_cast<unsigned char>(value[offset]) & 0xc0U) != 0x80U);
}

auto authoritative_document(db::connection& conn, std::string_view kind, std::int64_t id)
    -> std::expected<document, domain_error> {
  auto projected = document_authority::project(conn, kind, id);
  if (!projected) {
    auto error_kind = projected.error() == document_authority::error::not_found ? domain_error_kind::not_found
                                                                                : domain_error_kind::generic_failure;
    return std::unexpected(error_from_body(error_kind, "document projection failed"));
  }
  document result{.source_uuid = projected->source_uuid, .id = projected->id, .revision = projected->content_revision};
  for (auto const& item : projected->passages)
    result.passages.push_back({.key         = item.key,
                               .kind        = item.kind,
                               .text        = item.text,
                               .source_kind = item.source.kind,
                               .source_id   = item.source.id,
                               .source_path = item.source.path,
                               .start_line  = item.source.start_line,
                               .end_line    = item.source.end_line});
  return result;
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
  auto covered      = cliapp::flag_strings(args, "--covered-key");
  auto quotes       = cliapp::flag_strings(args, "--segment-quote");
  auto reject       = [&](std::string_view code) -> handler_result {
    return std::unexpected(error_from_body(domain_error_kind::sync_conflict, std::format("document range rejected: {}", code)));
  };
  if (!revision || *revision != doc->revision)
    return reject("stale_revision");
  if (!start_key || !end_key || !start_offset || !end_offset)
    return reject("missing_boundary");
  auto find  = [&](std::string_view key) { return std::ranges::find(doc->passages, key, &passage::key); };
  auto first = find(*start_key), last = find(*end_key);
  if (first == doc->passages.end() || last == doc->passages.end())
    return reject("foreign_key");
  if (first > last)
    return reject("reversed_range");
  auto                     begin  = static_cast<std::size_t>(first - doc->passages.begin());
  auto                     finish = static_cast<std::size_t>(last - doc->passages.begin());
  std::vector<std::string> actual_keys;
  std::vector<std::string> actual_quotes;
  for (auto i = begin; i <= finish; ++i) {
    auto const& text = doc->passages[i].text;
    auto        from = i == begin ? static_cast<std::size_t>(*start_offset) : 0U;
    auto        to   = i == finish ? static_cast<std::size_t>(*end_offset) : text.size();
    if (from > to || to > text.size() || !utf8_boundary(text, from) || !utf8_boundary(text, to))
      return reject("invalid_utf8_boundary");
    actual_keys.push_back(doc->passages[i].key);
    actual_quotes.push_back(text.substr(from, to - from));
  }
  if (covered != actual_keys)
    return reject("noncontiguous_covered_keys");
  if (quotes != actual_quotes)
    return reject("forged_quote");
  std::string normalized;
  for (std::size_t i = 0; i < actual_quotes.size(); ++i) {
    if (i != 0)
      normalized += '\n';
    normalized += actual_quotes[i];
  }
  std::string out = std::format("{{\"contract_version\":\"block-range-validation-v1\",\"source_uuid\":{},\"document_id\":{},"
                                "\"content_revision\":{},\"covered_keys\":[",
                                json(doc->source_uuid), json(doc->id), json(doc->revision));
  for (std::size_t i = 0; i < actual_keys.size(); ++i) {
    if (i != 0)
      out += ',';
    out += json(actual_keys[i]);
  }
  out += "],\"segment_quotes\":[";
  for (std::size_t i = 0; i < actual_quotes.size(); ++i) {
    if (i != 0)
      out += ',';
    out += json(actual_quotes[i]);
  }
  out += std::format("],\"normalized_quote\":{}}}", json(normalized));
  ctx.out() << out << '\n';
  return {};
}

auto declare_document(CLI::App& root) -> void {
  auto* document     = root.add_subcommand("document", "Project and validate authoritative block documents.");
  auto  source_flags = [](CLI::App& app) {
    add_string_required(app, "--kind");
    add_int_required(app, "--id");
  };
  auto* project = document->add_subcommand("project", "Emit an authoritative block-document-v1 projection.");
  source_flags(*project);
  add_json(*project);
  auto* validate = document->add_subcommand("validate-range", "Validate an adjacent revision-bound passage range.");
  source_flags(*validate);
  add_string_required(*validate, "--content-revision");
  add_string_required(*validate, "--start-key");
  add_int_required(*validate, "--start-offset");
  add_string_required(*validate, "--end-key");
  add_int_required(*validate, "--end-offset");
  add_string_list(*validate, "--covered-key");
  add_string_list(*validate, "--segment-quote");
  add_json(*validate);
}

} // namespace planar::cmd::handlers
