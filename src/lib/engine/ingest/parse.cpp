/// @file parse.cpp
/// @brief Implementation of `planar.engine.ingest.parse`.
///
/// Every extraction rule here is a behavior-preserving port (D2) of
/// `zig/src/engine/ingestor/parse.zig`. Where the Zig original threads an
/// allocator and pairs each type with a `deinit`, this implementation returns
/// owning `std::string`/`std::vector` values — the parsing decisions are
/// identical, the memory discipline is structural instead of conventional.

module planar.engine.ingest.parse;

import std;

namespace planar::engine::ingest::parse {
namespace {

constexpr std::string_view whitespace_inline = " \t";
constexpr std::string_view whitespace_block  = " \t\n\r";

/// @brief Trims `chars` from the end of `s`.
[[nodiscard]] auto trim_end(std::string_view s, std::string_view chars) -> std::string_view {
  const auto last = s.find_last_not_of(chars);
  return last == std::string_view::npos ? std::string_view{} : s.substr(0, last + 1);
}

/// @brief Trims `chars` from the start of `s`.
[[nodiscard]] auto trim_begin(std::string_view s, std::string_view chars) -> std::string_view {
  const auto first = s.find_first_not_of(chars);
  return first == std::string_view::npos ? std::string_view{} : s.substr(first);
}

/// @brief Trims `chars` from both ends of `s`.
[[nodiscard]] auto trim(std::string_view s, std::string_view chars) -> std::string_view {
  return trim_end(trim_begin(s, chars), chars);
}

/// @brief Trims trailing inline whitespace, mirroring the Zig `trimRight`.
[[nodiscard]] auto trim_right(std::string_view line) -> std::string_view {
  return trim_end(line, whitespace_inline);
}

/// @brief Splits `s` on `\n`, keeping the trailing segment even when empty.
///
/// Matches the Zig `splitLines` exactly: a body ending in `\n` therefore
/// yields one final empty line, which the section walkers rely on to close an
/// open block.
[[nodiscard]] auto split_lines(std::string_view s) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  std::size_t                   start = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  out.push_back(s.substr(start));
  return out;
}

/// @brief Returns the lines belonging to the H2 section titled `header`.
///
/// The section runs from the line after the heading to the next H2 (a `### `
/// H3 does NOT terminate it — `"### "` does not start with `"## "`). Returns
/// `nullopt` when no such heading exists.
[[nodiscard]] auto slice_section(std::span<const std::string_view> lines, std::string_view header)
    -> std::optional<std::span<const std::string_view>> {
  std::optional<std::size_t> start;
  std::size_t                end = lines.size();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto t = trim_right(lines[i]);
    if (!start.has_value() && t == header) {
      start = i + 1;
      continue;
    }
    if (start.has_value() && t.starts_with("## ")) {
      end = i;
      break;
    }
  }
  if (!start.has_value()) {
    return std::nullopt;
  }
  return lines.subspan(*start, end - *start);
}

/// @brief Joins `lines` with `\n` and trims surrounding whitespace.
[[nodiscard]] auto join_trimmed_body(std::span<const std::string_view> lines) -> std::string {
  std::string joined;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    joined.append(lines[i]);
    if (i + 1 < lines.size()) {
      joined.push_back('\n');
    }
  }
  return std::string{trim(joined, whitespace_block)};
}

/// @brief Whether `line` (after leading-whitespace trim) opens a Markdown
/// bullet.
[[nodiscard]] auto is_bullet(std::string_view line) -> bool {
  const auto t = trim_begin(line, whitespace_inline);
  return t.starts_with("- ") || t.starts_with("* ");
}

/// @brief Extracts a short title from a decision bullet.
///
/// Priority: the first `**…**` bold run, else the first sentence terminated by
/// `.`/`?`/`!`, else the whole bullet text.
[[nodiscard]] auto extract_bullet_decision_title(std::string_view bullet_text) -> std::string {
  if (bullet_text.starts_with("**")) {
    if (const auto close = bullet_text.find("**", 2); close != std::string_view::npos) {
      const auto inner = trim(bullet_text.substr(2, close - 2), whitespace_inline);
      if (!inner.empty()) {
        return std::string{inner};
      }
    }
  }
  for (std::size_t i = 0; i < bullet_text.size(); ++i) {
    const char c = bullet_text[i];
    if (c == '.' || c == '?' || c == '!') {
      const auto sentence = trim(bullet_text.substr(0, i + 1), whitespace_inline);
      if (!sentence.empty()) {
        return std::string{sentence};
      }
    }
  }
  return std::string{trim(bullet_text, whitespace_inline)};
}

/// @brief One `[name: …]` annotation lifted out of a bullet title.
struct annotation {
  bool        found_ = false;
  std::string inner_;    ///< Raw text between the marker and its `]`.
  std::string stripped_; ///< The title with the annotation removed, trimmed.
};

/// @brief Finds the LAST `<open>…]` run in `title`.
///
/// Searching from the right (and tolerating exactly one occurrence) is what
/// the Zig original does per-annotation; keeping it identical matters because
/// a title that legitimately contains `[` earlier would otherwise resolve
/// differently.
[[nodiscard]] auto extract_annotation(std::string_view title, std::string_view open) -> annotation {
  const auto idx = title.rfind(open);
  if (idx == std::string_view::npos) {
    return {};
  }
  const auto close_off = title.substr(idx).find(']');
  if (close_off == std::string_view::npos) {
    return {};
  }
  const auto  inner = title.substr(idx + open.size(), close_off - open.size());
  std::string rebuilt{title.substr(0, idx)};
  rebuilt.append(title.substr(idx + close_off + 1));
  return {.found_ = true, .inner_ = std::string{inner}, .stripped_ = std::string{trim(rebuilt, whitespace_inline)}};
}

/// @brief Splits an annotation's inner text on commas, dropping empty entries.
[[nodiscard]] auto split_csv(std::string_view inner) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::size_t              start = 0;
  while (start <= inner.size()) {
    const auto comma = inner.find(',', start);
    const auto piece = inner.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    if (const auto t = trim(piece, whitespace_inline); !t.empty()) {
      out.emplace_back(t);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

/// @brief Parses one (already folded) roadmap bullet into a work item.
[[nodiscard]] auto parse_bullet(std::string_view line) -> work_item {
  auto      raw = trim_begin(line, whitespace_inline);
  work_item item{};
  item.source_text_ = std::string{trim(raw, whitespace_inline)};
  if (raw.starts_with("- ") || raw.starts_with("* ")) {
    raw = raw.substr(2);
  }

  std::string title{trim(raw, whitespace_inline)};

  // `[touches: a, b]` — repos or paths this bullet's task will edit.
  if (const auto a = extract_annotation(title, "[touches:"); a.found_) {
    item.touches_ = split_csv(trim(a.inner_, whitespace_inline));
    title         = a.stripped_;
  }
  // `[depends: other-slug, …]` — bullets whose tasks must finish first.
  // Resolved to edges only after every task exists, so a bullet may name one
  // defined later in the roadmap.
  if (const auto a = extract_annotation(title, "[depends:"); a.found_) {
    item.depends_ = split_csv(trim(a.inner_, whitespace_inline));
    title         = a.stripped_;
  }
  // `[slug: foo-bar]` — the task's stable slug.
  if (const auto a = extract_annotation(title, "[slug:"); a.found_) {
    item.slug_ = sanitize_slug(a.inner_);
    title      = a.stripped_;
  }

  item.title_ = std::move(title);
  return item;
}

/// @brief Returns the trimmed value after a literal `**Field:**` token.
[[nodiscard]] auto strip_field_line(std::string_view line, std::string_view prefix) -> std::optional<std::string> {
  const auto t = trim(line, whitespace_inline);
  if (!t.starts_with(prefix)) {
    return std::nullopt;
  }
  return std::string{trim(t.substr(prefix.size()), whitespace_inline)};
}

/// @brief Whether every byte of `s` is ASCII alphanumeric, `-`, or `_`.
[[nodiscard]] auto looks_like_slug(std::string_view s) -> bool {
  if (s.empty()) {
    return false;
  }
  return std::ranges::all_of(s, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

/// @brief Splits a comma-separated `**Verifies:**` value into task refs.
///
/// Each surviving entry is a bare positive integer (kind defaults to `task`),
/// `<kind>:<int>`, or `<kind>:<slug>`. Malformed entries are dropped silently,
/// matching the Zig original — the coverage gate reports the resulting
/// shortfall rather than the parser rejecting the document.
[[nodiscard]] auto parse_task_refs(std::string_view value) -> std::vector<task_ref> {
  std::vector<task_ref> out;
  if (value.empty()) {
    return out;
  }
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto comma = value.find(',', start);
    const auto piece = value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    start            = comma == std::string_view::npos ? value.size() + 1 : comma + 1;

    const auto t = trim(piece, whitespace_inline);
    if (t.empty()) {
      continue;
    }
    std::string_view kind_str = "task";
    std::string_view val_str  = t;
    if (const auto colon = t.find(':'); colon != std::string_view::npos) {
      kind_str = trim(t.substr(0, colon), whitespace_inline);
      val_str  = trim(t.substr(colon + 1), whitespace_inline);
    }
    if (kind_str.empty() || val_str.empty()) {
      continue;
    }

    // Numeric form first; a non-positive id is dropped rather than stored.
    std::int64_t id     = 0;
    const auto*  begin  = val_str.data();
    const auto*  end    = val_str.data() + val_str.size();
    const auto   parsed = std::from_chars(begin, end, id);
    if (parsed.ec == std::errc{} && parsed.ptr == end) {
      if (id <= 0) {
        continue;
      }
      out.push_back({.kind_ = std::string{kind_str}, .id_ = id, .slug_ = {}});
      continue;
    }

    if (!looks_like_slug(val_str)) {
      continue;
    }
    out.push_back({.kind_ = std::string{kind_str}, .id_ = 0, .slug_ = std::string{val_str}});
  }
  return out;
}

/// @brief Extracts the `Resolution:` text from a question's body lines.
///
/// The marker is honored only on the first NON-BLANK line and only
/// case-sensitively, exactly as the Zig original: a body that merely mentions
/// resolution further down stays an open question.
[[nodiscard]] auto extract_resolution(std::span<const std::string_view> body_lines) -> std::string {
  std::optional<std::size_t> idx;
  for (std::size_t i = 0; i < body_lines.size(); ++i) {
    if (!trim(body_lines[i], whitespace_inline).empty()) {
      idx = i;
      break;
    }
  }
  if (!idx.has_value()) {
    return {};
  }
  const auto first = trim_right(body_lines[*idx]);
  if (!first.starts_with("Resolution:")) {
    return {};
  }

  std::vector<std::string_view> pieces;
  if (const auto after = trim(first.substr(std::string_view{"Resolution:"}.size()), whitespace_inline); !after.empty()) {
    pieces.push_back(after);
  }
  for (std::size_t i = *idx + 1; i < body_lines.size(); ++i) {
    pieces.push_back(trim_right(body_lines[i]));
  }
  return join_trimmed_body(pieces);
}

/// @brief Mutable state for one open scenario heading (H3 or H4).
struct pending_scenario {
  bool                          open_ = false;
  std::string                   title_;
  std::string                   kind_;
  std::string                   acceptance_;
  std::vector<task_ref>         verifies_;
  std::vector<std::string_view> body_lines_;
  bool                          has_prefix_ = false; ///< Heading carried `Scenario: `.
  bool                          is_h3_      = false; ///< Heading was H3 (else H4).
};

} // namespace

auto sanitize_slug(std::string_view raw) -> std::string {
  const auto t = trim(raw, whitespace_inline);
  if (t.empty()) {
    return {};
  }
  std::string out;
  bool        prev_dash = true; // suppresses leading dashes
  for (const char c : t) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c + ('a' - 'A')));
      prev_dash = false;
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out.push_back(c);
      prev_dash = false;
    } else if (!prev_dash) {
      out.push_back('-');
      prev_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  return out;
}

auto parse_tech_spec_decisions(std::string_view body) -> std::vector<decision> {
  const auto lines   = split_lines(body);
  const auto section = slice_section(lines, "## Decisions");
  if (!section.has_value()) {
    return {};
  }

  // An H3 anywhere in the section takes precedence; bullets are then ignored
  // entirely rather than merged, so a spec that documents a decision in prose
  // under a proper H3 does not also emit every stray bullet as a decision.
  const bool has_h3 = std::ranges::any_of(*section, [](std::string_view line) { return trim_right(line).starts_with("### "); });

  std::vector<decision> out;
  if (has_h3) {
    std::optional<std::string>    current_title;
    std::vector<std::string_view> body_lines;
    for (const auto line : *section) {
      const auto trimmed = trim_right(line);
      if (trimmed.starts_with("### ")) {
        if (current_title.has_value()) {
          out.push_back({.title_ = std::move(*current_title), .body_ = join_trimmed_body(body_lines)});
        }
        current_title = std::string{trim(trimmed.substr(4), whitespace_inline)};
        body_lines.clear();
        continue;
      }
      if (current_title.has_value()) {
        body_lines.push_back(line);
      }
    }
    if (current_title.has_value()) {
      out.push_back({.title_ = std::move(*current_title), .body_ = join_trimmed_body(body_lines)});
    }
    return out;
  }

  // Bullet fallback: each `- ` / `* ` bullet is one decision.
  for (const auto line : *section) {
    const auto trimmed = trim_right(line);
    if (!is_bullet(trimmed)) {
      continue;
    }
    auto raw = trim_begin(trimmed, whitespace_inline);
    if (raw.starts_with("- ") || raw.starts_with("* ")) {
      raw = raw.substr(2);
    }
    raw = trim(raw, whitespace_inline);
    if (raw.empty()) {
      continue;
    }
    out.push_back({.title_ = extract_bullet_decision_title(raw), .body_ = std::string{raw}});
  }
  return out;
}

auto parse_tech_spec_open_questions(std::string_view body) -> std::vector<question> {
  const auto lines   = split_lines(body);
  const auto section = slice_section(lines, "## Open Questions");
  if (!section.has_value()) {
    return {};
  }

  std::vector<question>         out;
  std::optional<std::string>    current_title;
  std::vector<std::string_view> body_lines;

  const auto flush = [&] {
    if (current_title.has_value()) {
      out.push_back({.title_      = std::move(*current_title),
                     .body_       = join_trimmed_body(body_lines),
                     .resolution_ = extract_resolution(body_lines)});
      current_title.reset();
    }
    body_lines.clear();
  };

  for (const auto line : *section) {
    const auto trimmed = trim_right(line);
    if (trimmed.starts_with("### ")) {
      flush();
      current_title = std::string{trim(trimmed.substr(4), whitespace_inline)};
      continue;
    }
    if (current_title.has_value()) {
      body_lines.push_back(line);
    }
  }
  flush();
  return out;
}

auto parse_roadmap(std::string_view body) -> std::vector<milestone> {
  const auto lines = split_lines(body);

  std::vector<milestone>     out;
  std::optional<std::string> name;
  std::string                intent;
  std::vector<work_item>     work_items;
  bool                       in_intent   = false;
  bool                       intent_done = false;

  const auto flush = [&] {
    if (name.has_value()) {
      out.push_back({.name_ = std::move(*name), .intent_ = intent, .work_items_ = std::move(work_items)});
      name.reset();
      work_items.clear();
    }
    intent.clear();
    in_intent   = false;
    intent_done = false;
  };

  for (std::size_t idx = 0; idx < lines.size(); ++idx) {
    const auto trimmed = trim_right(lines[idx]);

    // An H2 (but not an H3) starts a new milestone.
    if (trimmed.starts_with("## ") && !trimmed.starts_with("### ")) {
      flush();
      name        = std::string{trim(trimmed.substr(3), whitespace_inline)};
      in_intent   = true;
      intent_done = false;
      continue;
    }
    if (!name.has_value()) {
      continue;
    }

    if (is_bullet(trimmed)) {
      intent_done = true;
      in_intent   = false;

      // Fold indented continuation lines onto the bullet BEFORE parsing, so a
      // `[slug:]` / `[touches:]` an operator wrote on a wrapped line survives.
      // Without this the annotation is silently dropped and the task is born
      // slugless — invisible until the coverage gate reports a task nothing
      // can cite.
      std::string merged{trimmed};
      while (idx + 1 < lines.size()) {
        const auto next               = trim_right(lines[idx + 1]);
        const auto next_trimmed_start = trim_begin(next, whitespace_inline);
        if (next_trimmed_start.empty()) {
          break;
        }
        if (next_trimmed_start.starts_with("#")) {
          break;
        }
        if (next_trimmed_start.starts_with("- ") || next_trimmed_start.starts_with("* ")) {
          break;
        }
        // A non-indented line ends the bullet even when it has text.
        if (next.empty() || (next.front() != ' ' && next.front() != '\t')) {
          break;
        }
        merged.push_back(' ');
        merged.append(next_trimmed_start);
        ++idx;
      }

      work_items.push_back(parse_bullet(merged));
      continue;
    }

    if (in_intent && !intent_done) {
      const auto t = trim(trimmed, whitespace_inline);
      if (t.empty()) {
        if (!intent.empty()) {
          intent_done = true;
          in_intent   = false;
        }
        continue;
      }
      if (!intent.empty()) {
        intent.push_back(' ');
      }
      intent.append(t);
    }
  }
  flush();
  return out;
}

auto parse_test_spec(std::string_view body) -> std::vector<scenario> {
  const auto lines   = split_lines(body);
  const auto section = slice_section(lines, "## Scenarios");
  if (!section.has_value()) {
    return {};
  }

  std::vector<scenario> out;
  pending_scenario      pending;
  // True while we are inside the body of an H3 that was classified as a
  // bucket-group header, so its H4 children know they are the real scenarios.
  bool in_bucket_h3 = false;

  // Emits the pending slot when it qualifies, discards it otherwise.
  //
  // An H3 is emitted unless it is a bucket (no `Scenario: ` prefix, no
  // `**Verifies:**`, and an H4 child triggered this flush). An H4 is emitted
  // only when it carries the prefix or a `**Verifies:**` line — a bare H4 with
  // neither signal is an unrecognized sub-item, not a scenario.
  const auto flush = [&](bool triggered_by_h4) {
    if (!pending.open_) {
      pending = {};
      return;
    }
    const bool emit = pending.is_h3_ ? !(triggered_by_h4 && !pending.has_prefix_ && pending.verifies_.empty())
                                     : (pending.has_prefix_ || !pending.verifies_.empty());
    if (emit) {
      out.push_back({.title_      = std::move(pending.title_),
                     .kind_       = std::move(pending.kind_),
                     .acceptance_ = std::move(pending.acceptance_),
                     .verifies_   = std::move(pending.verifies_),
                     .body_       = join_trimmed_body(pending.body_lines_)});
      in_bucket_h3 = false;
    } else if (pending.is_h3_ && triggered_by_h4) {
      in_bucket_h3 = true;
    }
    pending = {};
  };

  const auto open_heading = [&](std::string_view raw_title, bool is_h3) {
    auto title      = trim(raw_title, whitespace_inline);
    bool has_prefix = false;
    if (title.starts_with("Scenario: ")) {
      title      = trim(title.substr(std::string_view{"Scenario: "}.size()), whitespace_inline);
      has_prefix = true;
    }
    pending             = {};
    pending.open_       = true;
    pending.title_      = std::string{title};
    pending.has_prefix_ = has_prefix;
    pending.is_h3_      = is_h3;
  };

  for (const auto line : *section) {
    const auto trimmed = trim_right(line);

    if (trimmed.starts_with("#### ")) {
      // An open H3 flushes with the H4 trigger so the bucket check fires; an
      // open H4 is a plain peer handoff.
      flush(pending.is_h3_);
      open_heading(trimmed.substr(5), /*is_h3=*/false);
      continue;
    }
    if (trimmed.starts_with("### ")) {
      flush(/*triggered_by_h4=*/false);
      in_bucket_h3 = false;
      open_heading(trimmed.substr(4), /*is_h3=*/true);
      continue;
    }

    if (!pending.open_) {
      continue; // Outside any heading, or inside a bucket with no H4 open yet.
    }

    // Field lines are consumed and do not reach the scenario body.
    if (auto val = strip_field_line(trimmed, "**Verifies:**"); val.has_value()) {
      pending.verifies_ = parse_task_refs(*val);
      continue;
    }
    if (auto val = strip_field_line(trimmed, "**Kind:**"); val.has_value()) {
      pending.kind_ = std::move(*val);
      continue;
    }
    if (auto val = strip_field_line(trimmed, "**Acceptance:**"); val.has_value()) {
      pending.acceptance_ = std::move(*val);
      continue;
    }
    pending.body_lines_.push_back(line);
  }
  flush(/*triggered_by_h4=*/false);
  return out;
}

auto section_has_content(std::string_view body, std::string_view header) -> bool {
  if (body.find(header) == std::string_view::npos) {
    return false;
  }
  bool        in_section = false;
  std::size_t start      = 0;
  for (std::size_t i = 0; i <= body.size(); ++i) {
    const bool at_newline = (i == body.size() || body[i] == '\n');
    if (!at_newline) {
      continue;
    }
    const auto t = trim_end(body.substr(start, i - start), whitespace_inline);
    start        = i + 1;

    if (!in_section) {
      if (t == header) {
        in_section = true;
      }
      continue;
    }
    // Another H2 (but not an H3) closes the section.
    if (t.starts_with("## ") && !t.starts_with("### ")) {
      return false;
    }
    if (!t.empty()) {
      return true;
    }
  }
  return false;
}

} // namespace planar::engine::ingest::parse
