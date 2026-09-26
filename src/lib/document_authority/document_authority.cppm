/// @file document_authority.cppm
/// @brief Transaction-scoped authoritative document projection and range validation.
export module planar.document_authority;

import std;
import planar.db;
import planar.sha256;

export namespace planar::document_authority {

/// A stable source mapping for one projected passage.
struct source_mapping {
  std::string  kind;
  std::string  id;
  std::string  path;
  std::int64_t start_line;
  std::int64_t end_line;
};

/// One ordered passage in an authoritative document revision.
struct passage {
  std::string    key;
  std::string    kind;
  std::string    text;
  source_mapping source;
};

/// A complete, immutable projection built from one database connection.
struct document {
  std::string          id;
  std::string          content_revision;
  std::vector<passage> passages;
};

/// Caller evidence for a complete adjacent range.
struct range_evidence {
  std::string              content_revision;
  std::string              start_key;
  std::size_t              start_offset;
  std::string              end_key;
  std::size_t              end_offset;
  std::vector<std::string> covered_keys;
  std::vector<std::string> segment_quotes;
};

/// A range accepted against the same authoritative snapshot.
struct validated_range {
  std::string              document_id;
  std::string              content_revision;
  std::vector<std::string> covered_keys;
  std::vector<std::string> segment_quotes;
  std::string              normalized_quote;
};

/// Typed projection or validation refusal.
enum class error {
  invalid_kind,
  not_found,
  query_failed,
  stale_revision,
  missing_boundary,
  foreign_key,
  reversed_range,
  invalid_utf8_boundary,
  noncontiguous_covered_keys,
  forged_quote,
};

namespace detail {
auto trim(std::string_view input) -> std::string_view {
  while (!input.empty() && (input.front() == ' ' || input.front() == '\t' || input.front() == '\r'))
    input.remove_prefix(1);
  while (!input.empty() && (input.back() == ' ' || input.back() == '\t' || input.back() == '\r'))
    input.remove_suffix(1);
  return input;
}

auto classify(std::string_view line) -> std::pair<std::string, std::string_view> {
  auto value         = trim(line);
  auto heading_marks = std::ranges::find_if(value, [](char c) { return c != '#'; }) - value.begin();
  if (heading_marks >= 1 && heading_marks <= 6 && static_cast<std::size_t>(heading_marks) < value.size() &&
      value[static_cast<std::size_t>(heading_marks)] == ' ')
    return {"heading", trim(value.substr(static_cast<std::size_t>(heading_marks) + 1))};
  if ((value.starts_with("- [ ] ") || value.starts_with("- [x] ") || value.starts_with("- [X] ") || value.starts_with("* [ ] ") ||
       value.starts_with("* [x] ") || value.starts_with("* [X] ")))
    return {"task_list_item", trim(value.substr(6))};
  if (value.starts_with("- ") || value.starts_with("* ") || value.starts_with("+ "))
    return {"list_item", trim(value.substr(2))};
  auto ordered_end = value.find(". ");
  if (ordered_end != std::string_view::npos && ordered_end > 0 &&
      std::ranges::all_of(value.substr(0, ordered_end), [](unsigned char c) { return std::isdigit(c) != 0; }))
    return {"list_item", trim(value.substr(ordered_end + 2))};
  if (value.starts_with(">"))
    return {"quote", trim(value.substr(1))};
  return {"paragraph", value};
}

auto add_markdown(document& doc, std::string_view source_kind, std::string source_id, std::string_view path,
                  std::string_view body) -> void {
  std::map<std::string, std::size_t, std::less<>> occurrences;
  std::size_t                                     line_no = 0;
  for (auto part : std::views::split(body, '\n')) {
    ++line_no;
    std::string_view line{part.begin(), part.end()};
    if (trim(line).empty())
      continue;
    auto [kind, text] = classify(line);
    auto identity     = sha256::hex(std::format("{}\n{}\n{}\n{}\n{}", source_kind, source_id, path, kind, text));
    auto occurrence   = occurrences[identity]++;
    doc.passages.push_back({.key    = std::format("{}:{}:{}:{}", source_kind, source_id, identity, occurrence),
                            .kind   = std::move(kind),
                            .text   = std::string{text},
                            .source = {.kind       = std::string{source_kind},
                                       .id         = source_id,
                                       .path       = std::string{path},
                                       .start_line = static_cast<std::int64_t>(line_no),
                                       .end_line   = static_cast<std::int64_t>(line_no)}});
  }
}

auto bind_id(db::connection& conn, std::string_view sql, std::int64_t id) -> std::expected<db::statement, error> {
  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, id))
    return std::unexpected(error::query_failed);
  return std::move(*stmt);
}

auto add_rows(document& doc, db::statement& stmt, std::string_view source_kind, std::string_view title_prefix = "")
    -> std::expected<void, error> {
  while (true) {
    auto row = stmt.step();
    if (!row)
      return std::unexpected(error::query_failed);
    if (*row == db::step_result::done)
      return {};
    auto id = std::to_string(stmt.column_int64(0));
    add_markdown(doc, source_kind, id, "title", std::format("{}{}", title_prefix, stmt.column_text(1)));
    if (!stmt.is_null(2))
      add_markdown(doc, source_kind, id, "body", stmt.column_text(2));
  }
}

auto utf8_boundary(std::string_view value, std::size_t offset) -> bool {
  return offset <= value.size() && (offset == value.size() || (static_cast<unsigned char>(value[offset]) & 0xc0U) != 0x80U);
}
} // namespace detail

/// Project a plan or artifact using the caller's current transaction/snapshot.
[[nodiscard]] auto project(db::connection& conn, std::string_view kind, std::int64_t id) -> std::expected<document, error> {
  document result{.id = std::format("{}:{}", kind, id)};
  if (kind == "artifact") {
    auto stmt = detail::bind_id(conn, "select title, body from artifacts where id = ?", id);
    if (!stmt)
      return std::unexpected(stmt.error());
    auto row = stmt->step();
    if (!row)
      return std::unexpected(error::query_failed);
    if (*row == db::step_result::done)
      return std::unexpected(error::not_found);
    detail::add_markdown(result, "artifact", std::to_string(id), "title", std::format("# {}", stmt->column_text(0)));
    if (!stmt->is_null(1))
      detail::add_markdown(result, "artifact", std::to_string(id), "body", stmt->column_text(1));
  } else if (kind == "plan") {
    auto plan = detail::bind_id(conn, "select title, summary from plans where id = ?", id);
    if (!plan)
      return std::unexpected(plan.error());
    auto row = plan->step();
    if (!row)
      return std::unexpected(error::query_failed);
    if (*row == db::step_result::done)
      return std::unexpected(error::not_found);
    detail::add_markdown(result, "plan", std::to_string(id), "title", std::format("# {}", plan->column_text(0)));
    if (!plan->is_null(1))
      detail::add_markdown(result, "plan", std::to_string(id), "summary", plan->column_text(1));

    auto append = [&](std::string_view sql, std::string_view source_kind) -> std::expected<void, error> {
      auto stmt = detail::bind_id(conn, sql, id);
      if (!stmt)
        return std::unexpected(stmt.error());
      return detail::add_rows(result, *stmt, source_kind);
    };
    if (auto value = append("select a.id, a.title, a.body from entity_links e join artifacts a on a.id=e.to_id "
                            "where e.from_kind='plan' and e.from_id=? and e.to_kind='artifact' and e.relationship='derives-from' "
                            "order by case a.kind when 'product_spec' then 1 when 'tech_spec' then 2 when 'roadmap' then 3 "
                            "when 'test_spec' then 4 else 5 end, a.id",
                            "artifact");
        !value)
      return std::unexpected(value.error());
  } else
    return std::unexpected(error::invalid_kind);

  std::string canonical = "block-document-v1\n" + result.id;
  for (auto const& p : result.passages)
    canonical += std::format("\n{}\n{}\n{}\n{}:{}:{}:{}:{}", p.key, p.kind, p.text, p.source.kind, p.source.id, p.source.path,
                             p.source.start_line, p.source.end_line);
  result.content_revision = sha256::hex(canonical);
  return result;
}

/// Validate complete range evidence against a freshly projected snapshot.
[[nodiscard]] auto validate(const document& doc, const range_evidence& evidence) -> std::expected<validated_range, error> {
  if (evidence.content_revision != doc.content_revision)
    return std::unexpected(error::stale_revision);
  auto find  = [&](std::string_view key) { return std::ranges::find(doc.passages, key, &passage::key); };
  auto first = find(evidence.start_key), last = find(evidence.end_key);
  if (first == doc.passages.end() || last == doc.passages.end())
    return std::unexpected(error::foreign_key);
  if (first > last)
    return std::unexpected(error::reversed_range);
  auto            begin  = static_cast<std::size_t>(first - doc.passages.begin());
  auto            finish = static_cast<std::size_t>(last - doc.passages.begin());
  validated_range result{.document_id = doc.id, .content_revision = doc.content_revision};
  for (auto i = begin; i <= finish; ++i) {
    auto const& text = doc.passages[i].text;
    auto        from = i == begin ? evidence.start_offset : 0U;
    auto        to   = i == finish ? evidence.end_offset : text.size();
    if (from > to || to > text.size() || !detail::utf8_boundary(text, from) || !detail::utf8_boundary(text, to))
      return std::unexpected(error::invalid_utf8_boundary);
    result.covered_keys.push_back(doc.passages[i].key);
    result.segment_quotes.push_back(text.substr(from, to - from));
  }
  if (result.covered_keys != evidence.covered_keys)
    return std::unexpected(error::noncontiguous_covered_keys);
  if (result.segment_quotes != evidence.segment_quotes)
    return std::unexpected(error::forged_quote);
  for (auto const& quote : result.segment_quotes) {
    if (!result.normalized_quote.empty())
      result.normalized_quote += '\n';
    result.normalized_quote += quote;
  }
  return result;
}

} // namespace planar::document_authority
