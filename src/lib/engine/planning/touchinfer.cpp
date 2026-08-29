/// @file touchinfer.cpp
/// @brief Implementation of `planar.engine.planning.touchinfer` (plan 996,
/// task 6330). See touchinfer.cppm for scope, the oracle-derived semantics,
/// and the one deliberate DIVERGENCE (expansion order).

module planar.engine.planning.touchinfer;

import std;
import planar.db;

namespace planar::engine::planning::touchinfer {

namespace {

/// @brief Characters stripped from either end of a token before resolution.
///
/// Task prose wraps paths in backticks, quotes, parens, and trailing
/// sentence punctuation; none of that is part of the path. Note `:` is in
/// here, so a token ending in a bare colon loses it before
/// `strip_line_suffix` ever looks.
constexpr std::string_view k_trim_chars = " \t\r\n`'\"()[]{}<>,;:";

/// @brief The whitespace the tokenizer splits on. Narrower than
/// `k_trim_chars` on purpose — this is the oracle's `tokenizeAny` set.
constexpr std::string_view k_split_chars = " \t\r\n";

/// @brief What `stat_rel` found at a path.
enum class entry_kind : std::uint8_t { file, directory, missing };

/// @brief Classify `root`/`rel` as file, directory, or absent.
///
/// Follows symlinks, matching the oracle's `openDir`/`openFile` pair. Any
/// filesystem error is `missing`, again matching the oracle, whose `catch`
/// arms collapse every failure into the same answer.
/// @param root The repo checkout root.
/// @param rel The repo-relative token.
/// @return The kind found.
auto stat_rel(const std::filesystem::path& root, std::string_view rel) -> entry_kind {
  std::error_code ec;
  const auto      status = std::filesystem::status(root / std::filesystem::path(rel), ec);
  if (ec) {
    return entry_kind::missing;
  }
  if (status.type() == std::filesystem::file_type::directory) {
    return entry_kind::directory;
  }
  if (std::filesystem::exists(status)) {
    return entry_kind::file;
  }
  return entry_kind::missing;
}

/// @brief Join a repo-relative parent and a child name the way the oracle's
/// `std.fs.path.join` does, with no leading separator when the parent is
/// empty.
/// @param rel The parent, possibly empty.
/// @param name The child entry name.
/// @return The repo-relative child path.
auto join_rel(std::string_view rel, std::string_view name) -> std::string {
  if (rel.empty()) {
    return std::string(name);
  }
  return std::string(rel) + "/" + std::string(name);
}

/// @brief Recursive walk shared by the directory-expansion and
/// bare-basename passes.
///
/// The two differ only in which files they keep, so they are one function
/// with a predicate rather than the oracle's two near-identical copies.
///
/// The over-limit guard is the oracle's, arm for arm, including its
/// off-by-one: the check runs BEFORE each append, so the accumulator can
/// reach `limit + 1` before the walk stops. That matters — `classify_token`
/// tests `size() > limit`, so a directory holding exactly `limit` files is
/// `directory` and one holding `limit + 1` is `too_broad`. Reproducing the
/// guard loosely would move that boundary by one.
///
/// Dot-entries are skipped outright (`.git`, `.zig-cache`, … are never touch
/// targets), and so is anything that is neither a regular file nor a
/// directory. Symlinks fall in that last group: the oracle reads
/// `readdir`'s `d_type`, where a symlink is `.sym_link` and hits the `else
/// => continue` arm, so `symlink_status` is used here rather than `status`.
/// @param root The repo checkout root.
/// @param rel The repo-relative directory to walk, empty for the root.
/// @param limit The expansion cap.
/// @param keep Predicate over the entry's own filename.
/// @param out The accumulator, appended to in filesystem order.
void walk(const std::filesystem::path& root, std::string_view rel, std::size_t limit,
          const std::function<bool(std::string_view)>& keep, std::vector<std::string>& out) {
  if (out.size() > limit) {
    return;
  }
  const std::filesystem::path dir_path = rel.empty() ? root : root / std::filesystem::path(rel);

  std::error_code                     ec;
  std::filesystem::directory_iterator it(dir_path, ec);
  if (ec) {
    return;
  }
  for (const auto& entry : it) {
    const std::string name = entry.path().filename().string();
    if (!name.empty() && name.front() == '.') {
      continue;
    }
    if (out.size() > limit) {
      return;
    }

    std::error_code   kind_ec;
    const auto        kind  = entry.symlink_status(kind_ec);
    const std::string child = join_rel(rel, name);
    if (kind_ec) {
      continue;
    }
    if (kind.type() == std::filesystem::file_type::directory) {
      walk(root, child, limit, keep, out);
    } else if (kind.type() == std::filesystem::file_type::regular) {
      if (keep(name)) {
        out.push_back(child);
      }
    }
  }
}

/// @brief Sort an expansion list lexicographically.
///
/// This is the module's one DIVERGENCE from the oracle, which emits
/// filesystem readdir order. See touchinfer.cppm's DIVERGENCE section for
/// why that order cannot be reproduced and why sorting cannot reach state.
/// @param paths The list to order in place.
void sort_expansion(std::vector<std::string>& paths) {
  std::ranges::sort(paths);
}

} // namespace

auto to_text(evidence value) -> std::string_view {
  switch (value) {
  case evidence::title:
    return "title";
  case evidence::body:
    return "body";
  case evidence::next_action:
    return "next_action";
  case evidence::citation:
    return "citation";
  }
  return "title";
}

auto to_text(classification value) -> std::string_view {
  switch (value) {
  case classification::resolved:
    return "resolved";
  case classification::directory:
    return "directory";
  case classification::basename:
    return "basename";
  case classification::unresolved:
    return "unresolved";
  case classification::too_broad:
    return "too_broad";
  }
  return "unresolved";
}

auto is_writable(classification value, bool wide) -> bool {
  switch (value) {
  case classification::resolved:
    return true;
  case classification::directory:
  case classification::basename:
    return wide;
  case classification::unresolved:
  case classification::too_broad:
    return false;
  }
  return false;
}

auto inference::writable_count(bool wide) const -> std::size_t {
  std::size_t n = 0;
  for (const auto& c : candidates) {
    if (is_writable(c.classification_, wide)) {
      n += c.paths.size();
    }
  }
  return n;
}

auto inference::review_count(bool wide) const -> std::size_t {
  std::size_t n = 0;
  for (const auto& c : candidates) {
    if (!is_writable(c.classification_, wide)) {
      ++n;
    }
  }
  return n;
}

auto strip_line_suffix(std::string_view s) -> std::string_view {
  const auto colon = s.rfind(':');
  if (colon == std::string_view::npos) {
    return s;
  }
  if (colon == 0 || colon + 1 >= s.size()) {
    return s;
  }
  for (const char c : s.substr(colon + 1)) {
    const bool digit = c >= '0' && c <= '9';
    if (!digit && c != '-') {
      return s;
    }
  }
  return s.substr(0, colon);
}

auto is_path_shaped(std::string_view s) -> bool {
  if (s.empty() || s.size() > 512) {
    return false;
  }
  // URLs are never repo paths, and `://` would otherwise read as a
  // separator-bearing token.
  if (s.find("://") != std::string_view::npos) {
    return false;
  }
  // Whitespace inside a token means the tokenizer already split wrong.
  for (const char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      return false;
    }
  }
  // Absolute paths and parent traversal are out of scope: touches are
  // repo-relative by definition.
  if (s.front() == '/') {
    return false;
  }
  if (s.starts_with("../")) {
    return false;
  }

  // `src/engine/foo.zig` or `docs/` — a separator is strong evidence.
  if (s.find('/') != std::string_view::npos) {
    return true;
  }

  // A bare `strategy.zig` is a candidate; a bare `README` or an English word
  // is not. Requiring an interior extension dot keeps prose out.
  const auto dot = s.rfind('.');
  return dot != std::string_view::npos && dot > 0 && dot + 1 < s.size();
}

auto extract_tokens(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::size_t              pos = 0;
  while (pos < text.size()) {
    const auto begin = text.find_first_not_of(k_split_chars, pos);
    if (begin == std::string_view::npos) {
      break;
    }
    auto end = text.find_first_of(k_split_chars, begin);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    pos = end;

    std::string_view raw = text.substr(begin, end - begin);

    const auto lead = raw.find_first_not_of(k_trim_chars);
    if (lead == std::string_view::npos) {
      continue;
    }
    const auto trail = raw.find_last_not_of(k_trim_chars);
    raw              = raw.substr(lead, trail - lead + 1);

    // A sentence-ending period rides along with a path at the end of a
    // clause ("Rework src/alpha.zig."). Strip it from the RIGHT only — a
    // leading dot is meaningful (`./x`), a trailing one never is, since no
    // path component ends in `.`. Without this the most natural way to write
    // a task body under-declares silently.
    while (!raw.empty() && raw.back() == '.') {
      raw.remove_suffix(1);
    }

    const std::string_view tok = strip_line_suffix(raw);
    if (!is_path_shaped(tok)) {
      continue;
    }
    out.emplace_back(tok);
  }
  return out;
}

auto classify_token(const std::filesystem::path& root, std::string_view token)
    -> std::pair<classification, std::vector<std::string>> {
  // Normalize a trailing slash: `docs/` and `docs` name the same directory.
  std::string_view norm = token;
  if (norm.size() > 1 && norm.back() == '/') {
    norm.remove_suffix(1);
  }

  switch (stat_rel(root, norm)) {
  case entry_kind::file:
    return {classification::resolved, {std::string(norm)}};
  case entry_kind::directory: {
    std::vector<std::string> acc;
    walk(root, norm, k_max_directory_expansion, [](std::string_view) { return true; }, acc);
    if (acc.size() > k_max_directory_expansion) {
      return {classification::too_broad, {}};
    }
    // An existing directory holding no non-hidden file is `unresolved`, not
    // an empty `directory`. Captured from the oracle: `emptydir/` reports
    // `unresolved` with zero paths.
    if (acc.empty()) {
      return {classification::unresolved, {}};
    }
    sort_expansion(acc);
    return {classification::directory, std::move(acc)};
  }
  case entry_kind::missing:
    break;
  }

  // Not a literal path. A separator-free token may still be a basename
  // occurring somewhere in the tree — propose EVERY match.
  //
  // The separator guard is an OPTIMISATION, not a rule: it is behaviourally
  // unobservable in the oracle and here alike, because the search compares a
  // candidate against a directory entry's own filename, which can never
  // contain `/`. A token carrying a separator therefore matches nothing with
  // the guard removed, and falls to `unresolved` either way. A break-probe
  // that replaced the condition with `true` SURVIVED, which is what
  // established this rather than a reading of the code; the note is here so
  // the next reader does not mistake the survivor for a missing test.
  //
  // What IS observable is that the match runs against the entry's FILENAME
  // rather than its repo-relative path: mutating `keep(name)` to
  // `keep(child)` kills `the basename fallback matches a filename exactly`.
  if (norm.find('/') == std::string_view::npos) {
    std::vector<std::string> acc;
    walk(root, "", k_max_directory_expansion, [norm](std::string_view name) { return name == norm; }, acc);
    if (!acc.empty() && acc.size() <= k_max_directory_expansion) {
      sort_expansion(acc);
      return {classification::basename, std::move(acc)};
    }
    if (acc.size() > k_max_directory_expansion) {
      return {classification::too_broad, {}};
    }
  }

  return {classification::unresolved, {}};
}

auto infer_from_text(const task_text& text, std::int64_t repo_id, const std::filesystem::path& root) -> std::vector<candidate> {
  std::vector<candidate>             out;
  std::set<std::string, std::less<>> seen;

  const std::array<std::pair<evidence, std::string_view>, 3> fields{{
      {evidence::title, text.title},
      {evidence::body, text.body},
      {evidence::next_action, text.next_action},
  }};

  for (const auto& [ev, s] : fields) {
    if (s.empty()) {
      continue;
    }
    for (auto& tok : extract_tokens(s)) {
      // A token repeated across fields is ONE candidate, attributed to the
      // first field that produced it.
      if (seen.contains(tok)) {
        continue;
      }
      auto [cls, paths] = classify_token(root, tok);
      seen.insert(tok);
      out.push_back(candidate{
          .token = std::move(tok), .evidence_ = ev, .classification_ = cls, .paths = std::move(paths), .repo_id = repo_id});
    }
  }

  return out;
}

auto infer(db::connection& conn, std::int64_t task_id, std::int64_t repo_id, const std::filesystem::path& root)
    -> std::expected<inference, infer_error> {
  auto stmt = conn.prepare("select title, coalesce(body, ''), coalesce(next_action, '') from tasks where id = ?");
  if (!stmt) {
    return std::unexpected(infer_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, task_id); !bound) {
    return std::unexpected(infer_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(infer_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(infer_error::not_found);
  }

  const task_text text{.title = stmt->column_text(0), .body = stmt->column_text(1), .next_action = stmt->column_text(2)};

  return inference{.task_id = task_id, .candidates = infer_from_text(text, repo_id, root)};
}

} // namespace planar::engine::planning::touchinfer
