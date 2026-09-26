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

auto strip_html(std::string_view line) -> std::string {
  std::string result;
  for (std::size_t cursor = 0; cursor < line.size();) {
    if (line[cursor] == '<') {
      auto close = line.find('>', cursor + 1);
      if (close != std::string_view::npos) {
        cursor = close + 1;
        continue;
      }
    }
    result.push_back(line[cursor++]);
  }
  return result;
}

auto flatten_links(std::string_view line) -> std::string {
  std::string result;
  std::size_t cursor = 0;
  while (cursor < line.size()) {
    auto open = line.find('[', cursor);
    if (open == std::string_view::npos) {
      result.append(line.substr(cursor));
      break;
    }
    auto label_end = line.find(']', open + 1);
    if (label_end == std::string_view::npos || label_end == open + 1 || label_end + 1 >= line.size() ||
        line[label_end + 1] != '(') {
      result.append(line.substr(cursor, open - cursor + 1));
      cursor = open + 1;
      continue;
    }
    auto href_end = line.find(')', label_end + 2);
    auto href = href_end == std::string_view::npos ? std::string_view{} : line.substr(label_end + 2, href_end - label_end - 2);
    if (href.empty() || std::ranges::any_of(href, [](unsigned char c) { return std::isspace(c) != 0 || c == ')'; })) {
      result.append(line.substr(cursor, open - cursor + 1));
      cursor = open + 1;
      continue;
    }
    result.append(line.substr(cursor, open - cursor));
    result.append(line.substr(open + 1, label_end - open - 1));
    cursor = href_end + 1;
  }
  return result;
}

auto classify(std::string_view line) -> std::pair<std::string, std::string> {
  auto heading_marks = std::ranges::find_if(line, [](char c) { return c != '#'; }) - line.begin();
  if (heading_marks >= 1 && heading_marks <= 6 && static_cast<std::size_t>(heading_marks) + 1 < line.size() &&
      line[static_cast<std::size_t>(heading_marks)] == ' ')
    return {"heading", flatten_links(line.substr(static_cast<std::size_t>(heading_marks) + 1))};

  auto first = line.find_first_not_of(" \t");
  auto value = first == std::string_view::npos ? std::string_view{} : line.substr(first);
  if ((value.starts_with("- [ ] ") || value.starts_with("- [x] ") || value.starts_with("- [X] ") || value.starts_with("* [ ] ") ||
       value.starts_with("* [x] ") || value.starts_with("* [X] ") || value.starts_with("+ [ ] ") || value.starts_with("+ [x] ") ||
       value.starts_with("+ [X] ")))
    return {"task_list_item", flatten_links(value.substr(6))};
  if (value.starts_with("- ") || value.starts_with("* ") || value.starts_with("+ "))
    return {"list_item", flatten_links(value.substr(2))};
  auto ordered_end = value.find(". ");
  if (ordered_end != std::string_view::npos && ordered_end > 0 &&
      std::ranges::all_of(value.substr(0, ordered_end), [](unsigned char c) { return std::isdigit(c) != 0; }))
    return {"list_item", flatten_links(value.substr(ordered_end + 2))};
  if (line.starts_with(">"))
    return {"quote", flatten_links(line.substr(line.starts_with("> ") ? 2 : 1))};
  return {"paragraph", flatten_links(line)};
}

auto fenced_open(std::string_view line) -> std::optional<std::string> {
  if (!line.starts_with("```"))
    return std::nullopt;
  auto suffix = line.substr(3);
  if (suffix.contains('`'))
    return std::nullopt;
  return std::string{trim(suffix)};
}

auto fenced_close(std::string_view line) -> bool {
  return line.starts_with("```") && std::ranges::all_of(line.substr(3), [](unsigned char c) { return std::isspace(c) != 0; });
}

auto table_row(std::string_view line) -> bool {
  auto value = trim(line);
  return value.size() >= 2 && value.front() == '|' && value.back() == '|';
}

auto table_cells(std::string_view line) -> std::vector<std::string> {
  auto value = trim(line);
  value.remove_prefix(1);
  value.remove_suffix(1);
  std::vector<std::string> result;
  for (auto part : std::views::split(value, '|'))
    result.emplace_back(trim(std::string_view{part.begin(), part.end()}));
  return result;
}

auto table_delimiter(std::string_view line) -> bool {
  auto value = trim(line);
  if (value.starts_with('|'))
    value.remove_prefix(1);
  if (value.ends_with('|'))
    value.remove_suffix(1);
  std::size_t count = 0;
  for (auto part : std::views::split(value, '|')) {
    auto cell = trim(std::string_view{part.begin(), part.end()});
    if (cell.starts_with(':'))
      cell.remove_prefix(1);
    if (cell.ends_with(':'))
      cell.remove_suffix(1);
    if (cell.size() < 3 || !std::ranges::all_of(cell, [](char c) { return c == '-'; }))
      return false;
    ++count;
  }
  return count >= 2;
}

auto add_markdown(document& doc, std::string_view source_kind, std::string source_id, std::string_view path,
                  std::string_view body) -> void {
  std::map<std::string, std::size_t, std::less<>> occurrences;
  std::vector<std::string_view>                   lines;
  for (auto part : std::views::split(body, '\n')) {
    std::string_view line{part.begin(), part.end()};
    if (line.ends_with('\r'))
      line.remove_suffix(1);
    lines.push_back(line);
  }
  auto append = [&](std::string kind, std::string text, std::size_t start_line, std::size_t end_line) {
    auto identity   = sha256::hex(std::format("{}\n{}\n{}\n{}\n{}", source_kind, source_id, path, kind, text));
    auto occurrence = occurrences[identity]++;
    doc.passages.push_back({.key    = std::format("{}:{}:{}:{}", source_kind, source_id, identity, occurrence),
                            .kind   = std::move(kind),
                            .text   = std::move(text),
                            .source = {.kind       = std::string{source_kind},
                                       .id         = source_id,
                                       .path       = std::string{path},
                                       .start_line = static_cast<std::int64_t>(start_line),
                                       .end_line   = static_cast<std::int64_t>(end_line)}});
  };

  for (std::size_t index = 0; index < lines.size(); ++index) {
    auto const raw_line = lines[index];
    auto const line     = strip_html(raw_line);
    if (trim(line).empty())
      continue;
    if (auto language = fenced_open(raw_line)) {
      auto const  start_line = index + 1;
      std::string text;
      bool        first_body_line = true;
      while (++index < lines.size() && !fenced_close(lines[index])) {
        if (!first_body_line)
          text += '\n';
        text += lines[index];
        first_body_line = false;
      }
      append("code", std::move(text), start_line, index < lines.size() ? index + 1 : lines.size());
      continue;
    }
    if (table_row(line) && index + 1 < lines.size() && table_delimiter(lines[index + 1])) {
      auto const start_line = index + 1;
      auto       rows       = std::vector<std::vector<std::string>>{table_cells(line)};
      index += 2;
      while (index < lines.size() && table_row(lines[index])) {
        rows.push_back(table_cells(lines[index]));
        ++index;
      }
      --index;
      std::string text;
      bool        first_cell = true;
      for (auto const& row : rows)
        for (auto const& cell : row) {
          if (!first_cell)
            text += ' ';
          text += cell;
          first_cell = false;
        }
      append("table", std::move(text), start_line, index + 1);
      continue;
    }
    auto [kind, text] = classify(line);
    append(std::move(kind), std::move(text), index + 1, index + 1);
  }
}

auto bind_id(db::connection& conn, std::string_view sql, std::int64_t id) -> std::expected<db::statement, error> {
  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, id))
    return std::unexpected(error::query_failed);
  return std::move(*stmt);
}

auto add_rows(document& doc, db::statement& stmt, std::string_view source_kind, std::string_view title_path = "title",
              std::string_view body_path = "body", std::string_view title_prefix = "") -> std::expected<void, error> {
  while (true) {
    auto row = stmt.step();
    if (!row)
      return std::unexpected(error::query_failed);
    if (*row == db::step_result::done)
      return {};
    auto id = std::to_string(stmt.column_int64(0));
    add_markdown(doc, source_kind, id, title_path, std::format("{}{}", title_prefix, stmt.column_text(1)));
    if (!stmt.is_null(2))
      add_markdown(doc, source_kind, id, body_path, stmt.column_text(2));
  }
}

auto append_rows(document& doc, db::connection& conn, std::int64_t plan_id, std::string_view sql, std::string_view source_kind,
                 std::string_view title_path = "title", std::string_view body_path = "body", std::string_view title_prefix = "")
    -> std::expected<void, error> {
  auto stmt = bind_id(conn, sql, plan_id);
  if (!stmt)
    return std::unexpected(stmt.error());
  return add_rows(doc, *stmt, source_kind, title_path, body_path, title_prefix);
}

auto utf8_boundary(std::string_view value, std::size_t offset) -> bool {
  return offset <= value.size() && (offset == value.size() || (static_cast<unsigned char>(value[offset]) & 0xc0U) != 0x80U);
}
} // namespace detail

/// Project a plan or artifact inside one deferred read snapshot.
[[nodiscard]] auto project(db::connection& conn, std::string_view kind, std::int64_t id) -> std::expected<document, error> {
  auto snapshot = conn.begin_transaction(db::lock_mode::deferred);
  if (!snapshot)
    return std::unexpected(error::query_failed);

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

    // Primary content is emitted in the block-document-v1 canonical order.
    // Every row below has a durable source identity. Source-less absent/partial
    // placeholders and reference-only links remain presentation metadata: they
    // cannot be selected and therefore deliberately do not enter range authority.
    if (auto value =
            detail::append_rows(result, conn, id,
                                "select distinct a.id, a.title, a.body from entity_links e join artifacts a on "
                                "((e.from_kind='artifact' and e.from_id=a.id) or (e.to_kind='artifact' and e.to_id=a.id)) "
                                "where e.relationship='derives-from' and ((e.from_kind='plan' and e.from_id=?1) or "
                                "(e.to_kind='plan' and e.to_id=?1)) "
                                "order by case a.kind when 'product_spec' then 1 when 'tech_spec' then 2 when 'roadmap' then 3 "
                                "when 'test_spec' then 4 else 5 end, a.id",
                                "artifact");
        !value)
      return std::unexpected(value.error());

    if (auto value =
            detail::append_rows(result, conn, id,
                                "select distinct d.id, d.title, d.body from entity_links e join decisions d on "
                                "((e.from_kind='decision' and e.from_id=d.id) or (e.to_kind='decision' and e.to_id=d.id)) "
                                "where (e.from_kind='plan' and e.from_id=?1) or (e.to_kind='plan' and e.to_id=?1) order by d.id",
                                "decision");
        !value)
      return std::unexpected(value.error());

    if (auto value =
            detail::append_rows(result, conn, id,
                                "select distinct q.id, q.title, q.body from entity_links e join questions q on "
                                "((e.from_kind='question' and e.from_id=q.id) or (e.to_kind='question' and e.to_id=q.id)) "
                                "where (e.from_kind='plan' and e.from_id=?1) or (e.to_kind='plan' and e.to_id=?1) order by q.id",
                                "question");
        !value)
      return std::unexpected(value.error());

    if (auto value =
            detail::append_rows(result, conn, id,
                                "select p.id, 'Parent plan', p.title from plans root join plans p on p.id=root.parent_plan_id "
                                "where root.id=? order by p.id",
                                "plan", "relationship", "title");
        !value)
      return std::unexpected(value.error());
    if (auto value = detail::append_rows(
            result, conn, id, "select p.id, 'Child plan', p.title from plans p where p.parent_plan_id=? order by p.title, p.id",
            "plan", "relationship", "title");
        !value)
      return std::unexpected(value.error());

    if (auto value = detail::append_rows(
            result, conn, id,
            "select s.id, 'Milestone ' || s.ordinal, s.body from plan_steps s where s.plan_id=? order by s.ordinal, s.id",
            "plan_step", "ordinal", "body");
        !value)
      return std::unexpected(value.error());

    if (auto value = detail::append_rows(result, conn, id,
                                         "select t.id, t.title, coalesce(t.body, '') from tasks t where t.plan_id=? "
                                         "order by t.priority, t.id",
                                         "task");
        !value)
      return std::unexpected(value.error());

    if (auto value = detail::append_rows(
            result, conn, id,
            "select e.id, e.relationship, e.from_kind || ':' || e.from_id || ' → ' || e.to_kind || ':' || e.to_id "
            "from entity_links e where e.relationship='depends-on' and "
            "((e.from_kind='plan' and e.from_id=?1) or (e.to_kind='plan' and e.to_id=?1)) order by e.id",
            "entity_link", "relationship", "endpoints");
        !value)
      return std::unexpected(value.error());

    if (auto value = detail::append_rows(
            result, conn, id,
            "select e.id, e.relationship, e.from_kind || ':' || e.from_id || ' → ' || e.to_kind || ':' || e.to_id "
            "from entity_links e where e.relationship not in ('derives-from','depends-on') and "
            "((e.from_kind='plan' and e.from_id=?1) or (e.to_kind='plan' and e.to_id=?1)) and "
            "e.from_kind not in ('decision','question') and e.to_kind not in ('decision','question') "
            "order by e.relationship, e.id",
            "entity_link", "relationship", "endpoints");
        !value)
      return std::unexpected(value.error());
  } else
    return std::unexpected(error::invalid_kind);

  std::string canonical = "block-document-v1\n" + result.id;
  for (auto const& p : result.passages)
    canonical += std::format("\n{}\n{}\n{}\n{}:{}:{}:{}:{}", p.key, p.kind, p.text, p.source.kind, p.source.id, p.source.path,
                             p.source.start_line, p.source.end_line);
  result.content_revision = sha256::hex(canonical);
  if (!snapshot->commit())
    return std::unexpected(error::query_failed);
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
