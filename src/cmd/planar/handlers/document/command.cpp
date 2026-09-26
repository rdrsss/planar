/// @file src/cmd/planar/handlers/document/command.cpp
/// @brief Implementation of the authoritative document projection boundary.

module planar.cmd.planar.handlers.document;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.document_authority;
import planar.sha256;
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

auto trim(std::string_view input) -> std::string_view {
  while (!input.empty() && (input.front() == ' ' || input.front() == '\t' || input.front() == '\r'))
    input.remove_prefix(1);
  while (!input.empty() && (input.back() == ' ' || input.back() == '\t' || input.back() == '\r'))
    input.remove_suffix(1);
  return input;
}

auto classify(std::string_view line) -> std::pair<std::string, std::string_view> {
  auto value = trim(line);
  if (value.starts_with("#")) {
    while (value.starts_with("#"))
      value.remove_prefix(1);
    return {"heading", trim(value)};
  }
  if (value.starts_with("- ") || value.starts_with("* "))
    return {"list_item", trim(value.substr(2))};
  if (value.starts_with("> "))
    return {"quote", trim(value.substr(2))};
  if (value.starts_with("```"))
    return {"code_fence", value};
  return {"paragraph", value};
}

auto add_markdown(std::vector<passage>& out, std::string_view source_kind, std::string source_id, std::string_view path,
                  std::string_view body) -> void {
  std::map<std::string, std::size_t, std::less<>> occurrences;
  std::size_t                                     line_no = 0;
  for (auto part : std::views::split(body, '\n')) {
    ++line_no;
    std::string_view line{part.begin(), part.end()};
    if (trim(line).empty())
      continue;
    auto [kind, text] = classify(line);
    auto identity   = sha256::hex(std::format("{}\n{}\n{}\n{}", source_kind, source_id, path, std::format("{}\n{}", kind, text)));
    auto occurrence = occurrences[identity]++;
    out.push_back({.key         = std::format("{}:{}:{}:{}", source_kind, source_id, identity, occurrence),
                   .kind        = std::move(kind),
                   .text        = std::string{text},
                   .source_kind = std::string{source_kind},
                   .source_id   = source_id,
                   .source_path = std::string{path},
                   .start_line  = static_cast<std::int64_t>(line_no),
                   .end_line    = static_cast<std::int64_t>(line_no)});
  }
}

auto bind_id(db::connection& conn, std::string_view sql, std::int64_t id) -> std::expected<db::statement, domain_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, id))
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "document query failed"));
  return std::move(*stmt);
}

auto build_document(db::connection& conn, std::string_view kind, std::int64_t id) -> std::expected<document, domain_error> {
  document result{.id = std::format("{}:{}", kind, id)};
  if (kind == "artifact") {
    auto stmt = bind_id(conn, "select title, body from artifacts where id = ?", id);
    if (!stmt)
      return std::unexpected(stmt.error());
    auto step = stmt->step();
    if (!step || *step != db::step_result::row)
      return std::unexpected(error_from_body(domain_error_kind::not_found, "document source not found"));
    add_markdown(result.passages, "artifact", std::to_string(id), "title", std::format("# {}", stmt->column_text(0)));
    if (!stmt->is_null(1))
      add_markdown(result.passages, "artifact", std::to_string(id), "body", stmt->column_text(1));
  } else if (kind == "plan") {
    auto stmt = bind_id(conn, "select title, summary from plans where id = ?", id);
    if (!stmt)
      return std::unexpected(stmt.error());
    auto step = stmt->step();
    if (!step || *step != db::step_result::row)
      return std::unexpected(error_from_body(domain_error_kind::not_found, "document source not found"));
    add_markdown(result.passages, "plan", std::to_string(id), "title", std::format("# {}", stmt->column_text(0)));
    if (!stmt->is_null(1))
      add_markdown(result.passages, "plan", std::to_string(id), "summary", stmt->column_text(1));
    auto artifacts =
        bind_id(conn,
                "select a.id, a.title, a.body from entity_links e join artifacts a on a.id = e.to_id "
                "where e.from_kind = 'plan' and e.from_id = ? and e.to_kind = 'artifact' and e.relationship = 'derives-from' "
                "order by case a.kind when 'product_spec' then 1 when 'tech_spec' then 2 when 'roadmap' then 3 when 'test_spec' "
                "then 4 else 5 end, e.created_at, e.id",
                id);
    if (!artifacts)
      return std::unexpected(artifacts.error());
    while (true) {
      auto row = artifacts->step();
      if (!row)
        return std::unexpected(error_from_body(domain_error_kind::generic_failure, "document query failed"));
      if (*row == db::step_result::done)
        break;
      auto artifact_id = artifacts->column_int64(0);
      add_markdown(result.passages, "artifact", std::to_string(artifact_id), "title",
                   std::format("## {}", artifacts->column_text(1)));
      if (!artifacts->is_null(2))
        add_markdown(result.passages, "artifact", std::to_string(artifact_id), "body", artifacts->column_text(2));
    }
  } else {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "document kind must be plan or artifact"));
  }
  std::string canonical = "block-document-v1\n" + result.id;
  for (auto const& p : result.passages)
    canonical +=
        std::format("\n{}\n{}\n{}\n{}:{}:{}:{}", p.key, p.kind, p.text, p.source_kind, p.source_id, p.source_path, p.start_line);
  result.revision = sha256::hex(canonical);
  return result;
}

auto render(const document& doc) -> std::string {
  std::string out =
      std::format("{{\"contract_version\":\"block-document-v1\",\"document_id\":{},\"content_revision\":{},\"passages\":[",
                  json(doc.id), json(doc.revision));
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
  document result{.id = projected->id, .revision = projected->content_revision};
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

} // namespace

auto document_project(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto src = source(args);
  if (!src)
    return std::unexpected(src.error());
  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  auto doc = authoritative_document(**conn, src->first, src->second);
  if (!doc)
    return std::unexpected(doc.error());
  ctx.out() << render(*doc) << '\n';
  return {};
}

auto document_validate_range(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto src = source(args);
  if (!src)
    return std::unexpected(src.error());
  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  auto doc = authoritative_document(**conn, src->first, src->second);
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
  std::string out = std::format(
      "{{\"contract_version\":\"block-range-validation-v1\",\"document_id\":{},\"content_revision\":{},\"covered_keys\":[",
      json(doc->id), json(doc->revision));
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
