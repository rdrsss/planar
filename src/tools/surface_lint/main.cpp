/// @file main.cpp
/// @brief `surface_lint` — deterministic semantic validator for authored
/// agent, skill, and doc Markdown. It began as a C++ port of
/// `zig/tools/surface_lint.zig` (plan 996, task 6402) and has since grown
/// checks of its own.
///
/// Scope: repository-relative links, retired references, artifact-set and
/// capability drift, command shapes, path existence, the skill
/// feedback/recovery headings, and (plan 1080, task hq-rule-lint) the
/// host-queue rule: `surface-queue-command` flags a build or test command in
/// `agents/` or `skills/src/` that is not given to `planar-agent queue run`,
/// and `surface-queue-marker-invalid` flags an unbalanced
/// `queue-lint-ignore-begin`/`-end` region. `surface-lint-ignore` suppresses
/// the first set of codes; the queue codes have their own `queue-lint-ignore`
/// line and region markers (see the checkQueueCommands comment below).
///
/// Plan 1089 adds `surface-retired-reference`: the names of the removed agent
/// database may not appear in `docs/`, `agents/`, `skills/src/`, `copilot/` or
/// the root guides, except in `docs/changelog.md` and in the marked upgrade
/// note of `INSTALL.md`. `surface-retired-ref-marker-invalid` flags an unclosed
/// or misplaced marker (see the checkRetiredReferences comment below).
///
/// The ported checks mirror their zig namesakes, including the pinned
/// `command_classes` inventory and the hand-rolled JSON string escaper, which
/// is its OWN table, distinct from `planar::json_text`'s (that one special-
/// cases 0x08/0x0C as `\b`/`\f`; this tool's original never did, so this
/// port does not either).
///
/// Usage:
///   surface_lint <repo-root> [--json]
///   surface_lint --command-inventory-json
///
/// Exit codes: 0 = clean, 1 = findings present, 2 = usage / internal error.

#include <cstdio>

import std;

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Finding codes.
// ---------------------------------------------------------------------------

namespace code {
constexpr std::string_view link                = "surface-link-missing";
constexpr std::string_view legacy              = "surface-legacy-reference";
constexpr std::string_view artifacts           = "surface-artifact-set-drift";
constexpr std::string_view capability          = "surface-capability-drift";
constexpr std::string_view command             = "surface-command-drift";
constexpr std::string_view contract            = "surface-contract-missing";
constexpr std::string_view path                = "surface-path-missing";
constexpr std::string_view suppression_invalid = "surface-suppression-invalid";
constexpr std::string_view suppression_unused  = "surface-suppression-unused";
constexpr std::string_view queue_command       = "surface-queue-command";
constexpr std::string_view queue_marker        = "surface-queue-marker-invalid";
constexpr std::string_view retired             = "surface-retired-reference";
constexpr std::string_view retired_marker      = "surface-retired-ref-marker-invalid";
} // namespace code

constexpr std::array<std::string_view, 7> k_suppressible_codes{code::link,    code::legacy,   code::artifacts, code::capability,
                                                               code::command, code::contract, code::path};
constexpr std::array<std::string_view, 3> k_scan_dirs{"agents", "skills/src", "docs"};

struct finding_t {
  std::string_view code;
  std::string      file;
  std::size_t      line = 0;
  std::string      message;
};
struct suppression_t {
  std::string code;
  std::size_t declaration_line = 0;
  std::size_t target_line      = 0;
  bool        used             = false;
};
struct result_t {
  std::vector<finding_t> findings;
  std::size_t            files_scanned = 0;
};

// ---------------------------------------------------------------------------
// Fence tracking — shared shape between the main scan loop, the
// suppression pre-pass, and the feedback-contract pass.
// ---------------------------------------------------------------------------

struct fence_t {
  char        marker  = 0;
  std::size_t run_len = 0;
};

/// @brief Update Markdown fenced-code state. An open fence closes only when
/// the marker character matches and the closing run is at least as long as
/// the opener. A different marker or a shorter run is content inside the
/// fence.
auto advance_fence(std::string_view trimmed, std::optional<fence_t>& state) -> bool {
  if (trimmed.size() < 3 || (trimmed[0] != '`' && trimmed[0] != '~'))
    return false;
  char const  marker  = trimmed[0];
  std::size_t run_len = 1;
  while (run_len < trimmed.size() && trimmed[run_len] == marker)
    ++run_len;
  if (run_len < 3)
    return false;

  if (state.has_value()) {
    if (marker != state->marker || run_len < state->run_len)
      return false;
    std::string_view rest = trimmed.substr(run_len);
    std::size_t      a    = 0;
    std::size_t      b    = rest.size();
    while (a < b && (rest[a] == ' ' || rest[a] == '\t' || rest[a] == '\r'))
      ++a;
    while (b > a && (rest[b - 1] == ' ' || rest[b - 1] == '\t' || rest[b - 1] == '\r'))
      --b;
    if (b - a != 0)
      return false;
    state.reset();
    return true;
  }
  state = fence_t{.marker = marker, .run_len = run_len};
  return true;
}

// ---------------------------------------------------------------------------
// Small string helpers.
// ---------------------------------------------------------------------------

auto trim(std::string_view s, std::string_view chars) -> std::string_view {
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && chars.find(s[a]) != std::string_view::npos)
    ++a;
  while (b > a && chars.find(s[b - 1]) != std::string_view::npos)
    --b;
  return s.substr(a, b - a);
}
auto trim_start(std::string_view s, std::string_view chars) -> std::string_view {
  std::size_t a = 0;
  while (a < s.size() && chars.find(s[a]) != std::string_view::npos)
    ++a;
  return s.substr(a);
}
auto split_lines(std::string_view content) -> std::vector<std::string_view> {
  std::vector<std::string_view> lines;
  std::size_t                   pos = 0;
  while (true) {
    std::size_t const nl = content.find('\n', pos);
    if (nl == std::string_view::npos) {
      lines.push_back(content.substr(pos));
      break;
    }
    lines.push_back(content.substr(pos, nl - pos));
    pos = nl + 1;
  }
  return lines;
}
auto is_ws(char c) -> bool {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

auto emit(std::vector<finding_t>& findings, std::vector<suppression_t>& suppressions, std::string_view code_,
          std::string const& file, std::size_t line, std::string message) -> void {
  // Deliberately NOT `!s.used`: one suppression comment covers every
  // finding of its code on its target line, not just the first. A single
  // line can legitimately carry more than one distinct finding of the same
  // code — e.g. `checkPathCitations` (task 6930) on a line narrating two
  // different deleted files in one sentence — and a `used`-once match would
  // silently let the second one through as an unsuppressed finding despite
  // a suppression visibly sitting right above it. The suppression is still
  // required to match at least once to avoid `surface-suppression-unused`.
  for (auto& s : suppressions) {
    if (s.target_line == line && s.code == code_) {
      s.used = true;
      return;
    }
  }
  findings.push_back({.code = code_, .file = file, .line = line, .message = std::move(message)});
}

auto is_suppressible(std::string_view code_) -> bool {
  return std::ranges::find(k_suppressible_codes, code_) != k_suppressible_codes.end();
}

auto invalid_suppression(std::vector<finding_t>& findings, std::string const& file, std::size_t line, std::string message)
    -> void {
  findings.push_back({.code = code::suppression_invalid, .file = file, .line = line, .message = std::move(message)});
}

// ---------------------------------------------------------------------------
// Suppression pre-pass.
// ---------------------------------------------------------------------------

auto parse_suppressions(std::string const& file, std::vector<std::string_view> const& lines, std::vector<finding_t>& findings,
                        std::vector<suppression_t>& suppressions) -> void {
  constexpr std::string_view prefix = "<!-- surface-lint-ignore ";
  std::optional<fence_t>     fence;
  for (std::size_t idx = 0; idx < lines.size(); ++idx) {
    std::string_view const line = trim(lines[idx], " \t\r");
    if (advance_fence(line, fence))
      continue;
    if (fence.has_value())
      continue;
    if (line.find("surface-lint-ignore-file") != std::string_view::npos) {
      invalid_suppression(findings, file, idx + 1, "file-wide suppressions are not allowed");
      continue;
    }
    if (line.find("surface-lint-ignore") == std::string_view::npos)
      continue;
    if (!line.starts_with(prefix) || !line.ends_with("-->")) {
      invalid_suppression(findings, file, idx + 1, "expected `<!-- surface-lint-ignore <code>: <rationale> -->`");
      continue;
    }
    std::string_view const body  = trim(line.substr(prefix.size(), line.size() - prefix.size() - 3), " \t");
    auto const             colon = body.find(':');
    if (colon == std::string_view::npos) {
      invalid_suppression(findings, file, idx + 1, "suppression rationale is required after `:`");
      continue;
    }
    std::string_view const code_str  = trim(body.substr(0, colon), " \t");
    std::string_view const rationale = trim(body.substr(colon + 1), " \t");
    if (!is_suppressible(code_str)) {
      invalid_suppression(findings, file, idx + 1, "suppression names an unknown finding code");
      continue;
    }
    if (rationale.empty()) {
      invalid_suppression(findings, file, idx + 1, "suppression rationale must be non-empty");
      continue;
    }
    std::size_t target = idx + 1;
    while (target < lines.size() && trim(lines[target], " \t\r").empty())
      ++target;
    if (target == lines.size()) {
      invalid_suppression(findings, file, idx + 1, "suppression has no following non-blank line");
      continue;
    }
    suppressions.push_back({.code = std::string{code_str}, .declaration_line = idx + 1, .target_line = target + 1});
  }
}

// ---------------------------------------------------------------------------
// checkLinks.
// ---------------------------------------------------------------------------

struct projection_link_t {
  std::string_view file;
  std::string_view target;
};
constexpr std::array<projection_link_t, 2> k_projection_links{
    projection_link_t{"docs/cli-reference.md", "../commands/claude/pl-synthesize.md"},
    projection_link_t{"docs/workflows.md", "../commands/claude/pl-workspace-scan.md"},
};
auto is_projection_only_link(std::string const& file, std::string_view target) -> bool {
  for (auto const& link : k_projection_links)
    if (file == link.file && target == link.target)
      return true;
  return false;
}

auto check_links(fs::path const& root, std::string const& file, fs::path const& abs_file, std::size_t line_no,
                 std::string_view line, std::vector<finding_t>& findings, std::vector<suppression_t>& suppressions) -> void {
  std::size_t cursor = 0;
  while (true) {
    auto const open = line.find("](", cursor);
    if (open == std::string_view::npos)
      break;
    std::size_t const start = open + 2;
    auto const        close = line.find(')', start);
    if (close == std::string_view::npos)
      break;
    cursor                  = close + 1;
    std::string_view target = trim(line.substr(start, close - start), " \t");
    if (target.empty() || target[0] == '#' || target.find("://") != std::string_view::npos || target.starts_with("mailto:") ||
        target.find_first_of("{}") != std::string_view::npos)
      continue;
    if (auto space = target.find_first_of(" \t"); space != std::string_view::npos)
      target = target.substr(0, space);
    if (auto hash = target.find('#'); hash != std::string_view::npos)
      target = target.substr(0, hash);
    if (auto query = target.find('?'); query != std::string_view::npos)
      target = target.substr(0, query);
    if (target.empty())
      continue;
    if (is_projection_only_link(file, target))
      continue;
    fs::path resolved;
    if (target.starts_with('/')) {
      resolved = root / fs::path{std::string{target.substr(1)}};
    } else {
      fs::path const base = abs_file.has_parent_path() ? abs_file.parent_path() : root;
      resolved            = base / fs::path{std::string{target}};
    }
    std::error_code ec;
    if (!fs::exists(resolved, ec)) {
      emit(findings, suppressions, code::link, file, line_no,
           std::format("repository-relative link target does not exist: {}", target));
    }
  }
}

// ---------------------------------------------------------------------------
// checkLegacy.
// ---------------------------------------------------------------------------

constexpr std::array<std::string_view, 5> k_retired_patterns{"src/internal/", "harness Agent/Task tool", "harness Agent tool",
                                                             "Go side", "Phase 5.5"};
auto check_legacy(std::string const& file, std::size_t line_no, std::string_view line, std::vector<finding_t>& findings,
                  std::vector<suppression_t>& suppressions) -> void {
  for (auto pattern : k_retired_patterns) {
    if (line.find(pattern) != std::string_view::npos &&
        (pattern != "src/internal/" || line.find(".go") != std::string_view::npos)) {
      emit(findings, suppressions, code::legacy, file, line_no, std::format("retired authored-surface reference: {}", pattern));
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// checkRetiredReferences (plan 1089, task qp-retired-ref-lint; tech spec 656
// § Retired-reference lint).
//
// The host queue moved into `planar.db` and the separate agent database was
// removed, so the names that belonged to it must not come back in authored
// surfaces. Unlike `k_retired_patterns` above, which fires on every scanned
// file, these patterns carry a PATH SCOPE and an EXEMPTION LIST:
//
//   - SCOPE: `docs/`, `agents/`, `skills/src/`, `copilot/`, and the root
//     files `README.md`, `CLAUDE.md`, `AGENTS.md`, `INSTALL.md`. A path outside
//     the scope (`install.sh`, `scripts/`, `src/`, `migrations/README.md`) is
//     never read for these patterns: the installer legitimately names the
//     store it retires.
//   - EXEMPT FILE: `docs/changelog.md` records the removal and may name it.
//   - EXEMPT REGION: in `INSTALL.md` only, the lines between two marker lines
//     (each exactly `<!-- retired-ref: agent.db upgrade note -->`) are the
//     upgrade note. The marker is honoured in no other file, so a stray copy
//     cannot silence a hit elsewhere; an unclosed region, or a marker in a
//     file that does not honour it, is itself a finding.
//
// Unlike `check_legacy`, a fenced code block is read too: a path in an
// installed-layout tree or a shell example is still a stale reference. The
// findings are NOT suppressible with `surface-lint-ignore`: the scope and the
// marked region are the whole exemption mechanism, so there is no per-line
// escape hatch to leave behind.
// ---------------------------------------------------------------------------

/// One retired name and where it may still appear.
struct scoped_retired_pattern_t {
  std::string_view text; ///< The retired name, matched as a substring.
};

constexpr std::array<scoped_retired_pattern_t, 4> k_scoped_retired_patterns{
    {{"agent.db"}, {"PLANAR_AGENT_DB"}, {"migrations-agent"}, {"limit_columns_select"}}};

constexpr std::array<std::string_view, 4> k_retired_scope_dirs{"docs/", "agents/", "skills/src/", "copilot/"};
constexpr std::array<std::string_view, 4> k_retired_scope_files{"README.md", "CLAUDE.md", "AGENTS.md", "INSTALL.md"};
constexpr std::array<std::string_view, 1> k_retired_exempt_files{"docs/changelog.md"};
constexpr std::string_view                k_retired_region_marker = "<!-- retired-ref: agent.db upgrade note -->";
constexpr std::string_view                k_retired_region_file   = "INSTALL.md";

auto in_retired_scope(std::string_view rel_file) -> bool {
  if (std::ranges::find(k_retired_exempt_files, rel_file) != k_retired_exempt_files.end())
    return false;
  if (std::ranges::find(k_retired_scope_files, rel_file) != k_retired_scope_files.end())
    return true;
  return std::ranges::any_of(k_retired_scope_dirs, [&](std::string_view dir) { return rel_file.starts_with(dir); });
}

auto check_retired_references(std::string const& rel_file, std::vector<std::string_view> const& lines,
                              std::vector<finding_t>& findings) -> void {
  if (!in_retired_scope(rel_file))
    return;
  bool const                 honours_region = rel_file == k_retired_region_file;
  std::optional<std::size_t> region_line;
  for (std::size_t idx = 0; idx < lines.size(); ++idx) {
    std::size_t const      line_no = idx + 1;
    std::string_view const line    = lines[idx];
    if (trim(line, " \t\r") == k_retired_region_marker) {
      if (!honours_region) {
        findings.push_back({.code    = code::retired_marker,
                            .file    = rel_file,
                            .line    = line_no,
                            .message = std::format("a retired-ref marker is honoured only in {}", k_retired_region_file)});
      } else if (region_line.has_value()) {
        region_line.reset();
      } else {
        region_line = line_no;
      }
      continue;
    }
    if (region_line.has_value())
      continue;
    for (auto const& pattern : k_scoped_retired_patterns) {
      if (line.find(pattern.text) != std::string_view::npos)
        findings.push_back({.code    = code::retired,
                            .file    = rel_file,
                            .line    = line_no,
                            .message = std::format("retired reference outside an exempt region: {}", pattern.text)});
    }
  }
  if (region_line.has_value())
    findings.push_back({.code    = code::retired_marker,
                        .file    = rel_file,
                        .line    = *region_line,
                        .message = "the retired-ref region opened here is never closed"});
}

// ---------------------------------------------------------------------------
// checkArtifactSet.
// ---------------------------------------------------------------------------

auto check_artifact_set(std::string const& file, std::size_t line_no, std::string_view line, std::vector<finding_t>& findings,
                        std::vector<suppression_t>& suppressions) -> void {
  if (line.find(".md") == std::string_view::npos)
    return;
  bool const contract_statement =
      line.find("Create ") != std::string_view::npos || line.find("Creates ") != std::string_view::npos ||
      line.find("creates ") != std::string_view::npos || line.find("registers ") != std::string_view::npos ||
      line.find("produces ") != std::string_view::npos || line.find("For each of ") != std::string_view::npos ||
      line.find("Repeat for each ") != std::string_view::npos;
  if (!contract_statement)
    return;
  constexpr std::array<std::string_view, 4> names{"product-spec", "tech-spec", "roadmap", "test-spec"};
  std::size_t                               present = 0;
  for (auto name : names)
    if (line.find(name) != std::string_view::npos)
      ++present;
  if (present >= 3 && present != names.size())
    emit(findings, suppressions, code::artifacts, file, line_no,
         "planning artifact set must contain product-spec, tech-spec, roadmap, and test-spec");
}

auto marker_run_length(std::string_view text, std::size_t start, char marker) -> std::size_t {
  std::size_t end = start;
  while (end < text.size() && text[end] == marker)
    ++end;
  return end - start;
}

// ---------------------------------------------------------------------------
// checkPathCitations (task 6930) — resolve inline-code-span source-path
// citations (`` `src/lib/engine/planning/strategy.cpp` ``) against the
// working tree. `checkLinks` above validates repository-relative Markdown
// LINK targets; this validates repository-relative PATH CITATIONS inside
// inline code spans, which is a distinct surface — a stale citation reads
// as live prose ("Add the event type to `src/engine/...`") with nothing
// resolving it, which is exactly how 116 citations of files deleted at the
// M10 cutover sat unnoticed for months (task 6926 / PR #190), and how the
// hand-fix for them introduced 13 more against the wrong checkout.
//
// A candidate is a substring of an inline (single-backtick) span that
// starts, at a path-char boundary, with one of `k_path_topdirs` followed by
// `/`, and runs through path characters (alnum, `_`, `-`, `.`, `/`) to a
// known source-ish extension in `k_path_extensions`. Trailing `.` characters
// (Markdown sentence punctuation, e.g. "...touch `src/foo.cpp`.") are
// trimmed before the extension check. Fenced code blocks need no special
// handling here: their content carries no single-backtick characters, so a
// ```json example embedding a fake `"path":"src/foo.cpp"` is never scanned
// as an inline span in the first place — only genuinely inline citations
// are.
//
// Three escape hatches, deliberately narrow (see this file's module doc and
// task 6930's brief), and deliberately NOT including removing the code-span
// backticks around a citation to dodge the check — that was tried and
// reverted (see `k_path_exemptions`'s doc comment for why it is actively
// harmful, not merely inelegant):
//   - `k_path_historical_files` / `k_path_historical_dirs` — a handful of
//     documents that are entirely retrospective (`docs/adrs.md`,
//     `docs/research/*`) and legitimately narrate deleted paths throughout.
//     Skipped at the FILE level, matching the task's explicit carve-out for
//     "the whole document is history" — distinct from, and not a
//     workaround for, the inline suppression mechanism below (which
//     explicitly refuses a file-wide `surface-lint-ignore-file`).
//   - the existing `<!-- surface-lint-ignore surface-path-missing: <why>
//     -->` inline comment, for a single historical or illustrative line
//     inside an otherwise-live document (e.g. `CLAUDE.md`,
//     `docs/architecture.md` outside the cockpit section, or a syntax
//     example like `[touches: src/lib/engine/x.cpp]`), placed on the line
//     immediately before — this is the SAME mechanism `checkLinks`/
//     `checkLegacy`/etc. already use, no new suppression syntax, and it is
//     ONLY safe when the citation's line starts its own paragraph/table/
//     list block; a comment line inserted mid-table-row or mid-wrapped-
//     paragraph changes the rendered document.
//   - `k_path_exemptions` — a keyed `(file, exact path, reason)` allowlist
//     for the citations where the inline comment is NOT structurally safe
//     (a table row, or an interior line of a hard-wrapped paragraph/list
//     item). See its own doc comment for the full rationale.
//
// Multiple distinct missing paths cited on one line are deduplicated to
// their unique matched strings before emitting, so one suppression comment
// covers a line that repeats the same illustrative citation twice (as both
// `docs/workflows.md`'s `[touches: ...]` example and `docs/cli-reference.md`'s
// `Rework src/alpha.cpp.` / `src/alpha.cpp` pairing do). Two GENUINELY
// different missing paths on one line would still need the line split, or
// two suppressions — matching `checkLinks`' identical pre-existing
// limitation for two distinct broken links on one line.
//
// SCOPE, PINNED: only citations inside an inline (single-backtick) code
// span are checked. A path-shaped token in plain, unformatted prose (no
// backticks at all) is never scanned and never flagged — this is the same
// boundary `checkLinks` draws for Markdown link targets, and it is
// deliberate, not an oversight: formatting a path as code is this
// codebase's own signal that the string is meant to be resolved literally,
// and a plain-prose mention (e.g. a sentence just naming a tool by its old
// name) makes no such claim. `fixtures/path_citation_bare/` pins this
// decision with a bare, non-resolving path that must NOT produce a finding
// — if that boundary ever needs to move, the fixture is the place future
// authors will find the decision recorded, not just this comment.

constexpr std::array<std::string_view, 12> k_path_topdirs{"src",    "migrations", "cmake",     "agents",
                                                          "skills", "docs",       "templates", "scripts",
                                                          "vendor", "external",   "tools",     "integration_tests"};
constexpr std::array<std::string_view, 12> k_path_extensions{"cpp", "cppm", "hpp", "h",     "cc",  "cxx",
                                                             "zig", "py",   "sql", "cmake", "lua", "sh"};

// Entire documents that are deliberately retrospective and may narrate
// deleted paths throughout (task 6930 brief: "a file-level exemption for
// genuinely historical documents is acceptable where the whole document is
// history"). Exact relative-path match.
constexpr std::array<std::string_view, 1> k_path_historical_files{"docs/adrs.md"};
// Same, by directory prefix.
constexpr std::array<std::string_view, 1> k_path_historical_dirs{"docs/research/"};

auto is_path_historical_file(std::string const& file) -> bool {
  if (std::ranges::find(k_path_historical_files, file) != k_path_historical_files.end())
    return true;
  for (auto dir : k_path_historical_dirs)
    if (file.starts_with(dir))
      return true;
  return false;
}

// A THIRD, narrower escape hatch: an exact (file, path) pair, with a
// mandatory reason. This exists for exactly one situation — a citation that
// is legitimately historical or illustrative but sits inside a Markdown
// construct where the inline `surface-lint-ignore` comment cannot be placed
// without changing the rendered document: a TABLE ROW (an HTML-comment line
// between table rows breaks GFM table continuation) or the MIDDLE of a
// hard-wrapped paragraph/list item (a comment line there ends the block
// early and visibly splits the sentence). `checkFeedbackContract`'s sibling
// checks never hit this because their targets are always link/legacy/
// command SHAPES on one line, never a citation embedded mid-sentence inside
// a wrapped physical line.
//
// This is deliberately NOT a substitute for de-backticking (removing the
// code-span formatting around the path) to silence the finding: stripping
// the backticks was tried and reverted, because it (a) formats a file path
// as plain prose, which reads worse and is factually wrong once the doc's
// own convention is "paths are code", (b) leaves no record that a human
// decided the miss is legitimate — the next reader cannot distinguish an
// intentional exemption from prose nobody ever formatted, and (c) teaches
// future authors that the cheap, invisible way to clear a `surface-path-
// missing` finding is to delete two characters, which silently narrows this
// tool's coverage over time with no diff that reads as a policy change. A
// keyed exemption is exactly as narrow (it names one literal path in one
// file, never a directory or a whole file) but it is visible, grep-able,
// and requires a reason.
struct path_exemption_t {
  std::string_view file;
  std::string_view path;
  std::string_view reason;
};
constexpr std::array<path_exemption_t, 11> k_path_exemptions{
    // CLAUDE.md's "Configure-time codegen" table row narrates the
    // Zig-era build-time codegen tools superseded by cmake/generate_*.cmake.
    path_exemption_t{"CLAUDE.md", "tools/gen_migrations.zig",
                     "table row (a preceding comment would break GFM table continuation); "
                     "deleted with zig/ at the M10 cutover"},
    path_exemption_t{"CLAUDE.md", "tools/gen_templates.zig", "table row; deleted with zig/ at the M10 cutover"},
    // CLAUDE.md's "Cross-implementation differential lanes" bullet is a
    // hard-wrapped paragraph; both retired-lane paths sit mid-sentence on
    // interior physical lines, not at the paragraph's start. Each of these
    // two also appears once more, in CLAUDE.md's "Source Layout" opening
    // paragraph — that occurrence is (file, path)-keyed the same way, not
    // given its own inline comment, so there is exactly one place recording
    // why each string is exempt, not two mechanisms disagreeing.
    path_exemption_t{"CLAUDE.md", "scripts/oracle-retirement-gate.sh",
                     "mid-wrapped bullet paragraph (a preceding comment would split the "
                     "sentence); also cited plainly in the Source Layout opening paragraph; "
                     "deleted with zig/ at the M10 cutover"},
    path_exemption_t{"CLAUDE.md", "src/cmd/parity_strict.hpp",
                     "mid-wrapped bullet paragraph; also cited plainly in the Source Layout "
                     "opening paragraph; deleted with zig/ at the M10 cutover"},
    // CLAUDE.md's "Authored-surface lint gate" bullet, same shape.
    path_exemption_t{"CLAUDE.md", "tools/cli_usage_lint.zig",
                     "mid-wrapped bullet paragraph naming the Zig source this tool was ported "
                     "from; deleted with zig/ at the M10 cutover"},
    path_exemption_t{"CLAUDE.md", "tools/surface_lint.zig", "mid-wrapped bullet paragraph; deleted with zig/ at the M10 cutover"},
    // docs/architecture.md's "Cross-implementation differential lanes"
    // bullet — same content as CLAUDE.md's, same reason.
    path_exemption_t{"docs/architecture.md", "scripts/oracle-retirement-gate.sh",
                     "bullet-list paragraph; deleted with zig/ at the M10 cutover"},
    path_exemption_t{"docs/architecture.md", "src/cmd/parity_strict.hpp",
                     "bullet-list paragraph; deleted with zig/ at the M10 cutover"},
    // docs/concepts.md's "Removed" callout for the never-implemented ANSI
    // color feature; citation sits mid-wrapped-paragraph.
    path_exemption_t{"docs/concepts.md", "src/cmd/planar/output.zig",
                     "mid-wrapped paragraph inside a > **Removed** callout naming the "
                     "never-C++-ported Zig entry point; deleted with zig/ at the M10 cutover"},
    // docs/lifecycles.md's own drift-tracking table (§7) catalogs a CLAIM
    // made elsewhere that has since been fixed; the citation is the claim
    // being cataloged, in a table cell.
    path_exemption_t{"docs/lifecycles.md", "src/engine/workbench/terminal.zig",
                     "table row in the §7 drift-tracking table, cataloging a since-fixed claim; "
                     "the cited path is the historical claim, not live documentation"},
    // docs/operations.md's `spec ingest --apply` touches-resolution section:
    // one of the five illustrative placeholders named in task 6930's brief.
    // `typo:src/x.cpp` demonstrates the unrelated-repo-slug rejection path;
    // `src/x.cpp` (the part this tool's matcher extracts) was never meant to
    // resolve. Mid-wrapped paragraph, so a preceding comment isn't safe.
    path_exemption_t{"docs/operations.md", "src/x.cpp",
                     "illustrative placeholder inside a mid-wrapped paragraph: `typo:src/x.cpp` "
                     "demonstrates the unresolved-unknown-repo path in `spec ingest --apply`'s "
                     "touches resolution, not a real file"},
};
auto is_path_exempt(std::string const& file, std::string_view path) -> bool {
  for (auto const& e : k_path_exemptions)
    if (file == e.file && path == e.path)
      return true;
  return false;
}

auto is_path_char(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
         c == '/';
}

/// @brief True when `span[pos]` starts a path-like token: `pos` is at a
/// non-path-char boundary (or the start of the span) and `span` continues
/// from `pos` with one of `k_path_topdirs` followed by `/`.
auto starts_path_candidate(std::string_view span, std::size_t pos) -> bool {
  if (pos != 0 && is_path_char(span[pos - 1]))
    return false;
  for (auto topdir : k_path_topdirs) {
    if (span.substr(pos).starts_with(topdir) && span.size() > pos + topdir.size() && span[pos + topdir.size()] == '/')
      return true;
  }
  return false;
}

/// @brief From a confirmed candidate start at `pos`, consume the maximal
/// run of path characters, trim trailing `.` (sentence punctuation), and
/// return the trimmed candidate when it ends with a known source-ish
/// extension; `std::nullopt` otherwise (e.g. the run has no recognized
/// extension, as with `handlers/drafting.cppm`'s bare-`handlers/` mentions,
/// which this deliberately does not chase without a `k_path_topdirs`
/// prefix).
auto extract_path_candidate(std::string_view span, std::size_t pos) -> std::optional<std::string_view> {
  std::size_t end = pos;
  while (end < span.size() && is_path_char(span[end]))
    ++end;
  std::string_view candidate = span.substr(pos, end - pos);
  while (!candidate.empty() && candidate.back() == '.')
    candidate.remove_suffix(1);
  for (auto ext : k_path_extensions) {
    if (candidate.size() > ext.size() + 1 && candidate.ends_with(ext) && candidate[candidate.size() - ext.size() - 1] == '.')
      return candidate;
  }
  return std::nullopt;
}

/// @brief Collect every distinct path-citation candidate inside one inline
/// (single-backtick) span into `out`, deduplicating exact repeats.
auto collect_path_candidates(std::string_view span, std::vector<std::string>& out) -> void {
  std::size_t pos = 0;
  while (pos < span.size()) {
    if (starts_path_candidate(span, pos)) {
      if (auto candidate = extract_path_candidate(span, pos); candidate.has_value()) {
        if (std::ranges::find(out, *candidate) == out.end())
          out.push_back(std::string{*candidate});
        pos += candidate->size();
        continue;
      }
    }
    ++pos;
  }
}

auto check_path_citations(fs::path const& root, std::string const& file, std::size_t line_no, std::string_view line,
                          std::vector<finding_t>& findings, std::vector<suppression_t>& suppressions) -> void {
  if (is_path_historical_file(file))
    return;
  std::vector<std::string> candidates;
  std::size_t              cursor = 0;
  while (true) {
    auto const open = line.find('`', cursor);
    if (open == std::string_view::npos)
      break;
    std::size_t const          run_len = marker_run_length(line, open, '`');
    std::size_t                search  = open + run_len;
    std::optional<std::size_t> close;
    while (true) {
      auto const candidate_close = line.find('`', search);
      if (candidate_close == std::string_view::npos)
        break;
      std::size_t const candidate_len = marker_run_length(line, candidate_close, '`');
      if (candidate_len == run_len) {
        close = candidate_close;
        break;
      }
      search = candidate_close + candidate_len;
    }
    if (!close.has_value())
      break;
    if (run_len == 1)
      collect_path_candidates(line.substr(open + run_len, *close - (open + run_len)), candidates);
    cursor = *close + run_len;
  }
  for (auto const& candidate : candidates) {
    if (is_path_exempt(file, candidate))
      continue;
    std::error_code ec;
    if (!fs::exists(root / fs::path{candidate}, ec))
      emit(findings, suppressions, code::path, file, line_no,
           std::format("authored surface cites a repository path that does not resolve against the working tree: {}", candidate));
  }
}

// ---------------------------------------------------------------------------
// command_classes — exhaustive leaf classification from the four schema
// catalogs (transcribed verbatim from zig/tools/surface_lint.zig).
// ---------------------------------------------------------------------------

enum class access_t : std::uint8_t { read, mutate };
struct command_class_t {
  std::string_view shape;
  access_t         access;
};
constexpr command_class_t r_(std::string_view s) {
  return {s, access_t::read};
}
constexpr command_class_t m_(std::string_view s) {
  return {s, access_t::mutate};
}

constexpr std::array<command_class_t, 260> k_command_classes{
    m_("planar init"),
    r_("planar scope show"),
    r_("planar scope suggest"),
    r_("planar scope use"),
    r_("planar scope pop"),
    r_("planar scope clear"),
    r_("planar assoc list"),
    m_("planar assoc create"),
    m_("planar assoc add"),
    m_("planar assoc remove"),
    r_("planar assoc members"),
    m_("planar assoc detect"),
    m_("planar plan create"),
    r_("planar plan show"),
    r_("planar plan list"),
    m_("planar plan update"),
    m_("planar plan edit"),
    r_("planar plan view"),
    r_("planar plan diff"),
    r_("planar plan review"),
    m_("planar plan link"),
    r_("planar plan next"),
    r_("planar plan recommend-strategy"),
    r_("planar plan divergence"),
    m_("planar plan recompute-status"),
    m_("planar plan closeout"),
    m_("planar plan step add"),
    r_("planar plan step list"),
    m_("planar plan step done"),
    m_("planar plan step skip"),
    m_("planar plan step link"),
    r_("planar plan descendants"),
    m_("planar task add"),
    r_("planar task show"),
    r_("planar task packet"),
    r_("planar task list"),
    m_("planar task update"),
    m_("planar task edit"),
    r_("planar task view"),
    r_("planar task diff"),
    r_("planar task review"),
    m_("planar task done"),
    m_("planar task cancel"),
    m_("planar task block"),
    m_("planar task link"),
    m_("planar task reopen"),
    m_("planar task touches add"),
    r_("planar task touches list"),
    m_("planar task touches remove"),
    m_("planar task touches infer"),
    m_("planar question add"),
    m_("planar question edit"),
    r_("planar question view"),
    r_("planar question diff"),
    r_("planar question review"),
    m_("planar question answer"),
    m_("planar question wontfix"),
    r_("planar question list"),
    r_("planar question show"),
    m_("planar question link"),
    m_("planar scenario add"),
    m_("planar scenario edit"),
    r_("planar scenario view"),
    r_("planar scenario diff"),
    r_("planar scenario review"),
    m_("planar scenario verify"),
    m_("planar scenario retire"),
    r_("planar scenario list"),
    r_("planar scenario show"),
    m_("planar scenario link"),
    m_("planar decision add"),
    r_("planar decision show"),
    r_("planar decision list"),
    m_("planar decision accept"),
    m_("planar decision supersede"),
    m_("planar decision withdraw"),
    m_("planar decision edit"),
    r_("planar decision view"),
    r_("planar decision diff"),
    r_("planar decision review"),
    m_("planar decision link"),
    m_("planar artifact add"),
    r_("planar artifact show"),
    r_("planar artifact list"),
    m_("planar artifact update"),
    m_("planar artifact edit"),
    r_("planar artifact view"),
    r_("planar artifact diff"),
    r_("planar artifact review"),
    m_("planar artifact link"),
    m_("planar annotate add"),
    r_("planar annotate show"),
    r_("planar annotate list"),
    m_("planar annotate update"),
    m_("planar annotate remove"),
    m_("planar annotate tag"),
    m_("planar annotate resolve"),
    m_("planar annotate dismiss"),
    m_("planar annotate archive"),
    m_("planar annotate bulk-resolve"),
    m_("planar annotate bulk-dismiss"),
    m_("planar annotate bulk-archive"),
    r_("planar annotate verify"),
    m_("planar annotate sweep"),
    m_("planar promote"),
    m_("planar demote"),
    r_("planar workbench lint"),
    m_("planar workbench pull"),
    m_("planar workbench push"),
    r_("planar workbench status"),
    m_("planar workbench resolve"),
    m_("planar workbench sync"),
    m_("planar workbench archive"),
    m_("planar workbench restore"),
    m_("planar workbench gc"),
    r_("planar workbench list"),
    m_("planar workbench publish"),
    r_("planar workbench extract-questions"),
    m_("planar workbench edit"),
    m_("planar workspace init"),
    m_("planar workspace doctor"),
    m_("planar workspace routing build"),
    r_("planar workspace routing show"),
    m_("planar workspace regenerate"),
    m_("planar ext register jira"),
    m_("planar ext register github"),
    r_("planar ext list"),
    r_("planar ext test"),
    m_("planar ext create"),
    m_("planar ext propagate-one"),
    m_("planar ext propagate"),
    m_("planar link"),
    m_("planar unlink"),
    m_("planar links add"),
    r_("planar links list"),
    m_("planar links remove"),
    r_("planar links trail"),
    m_("planar sync pull"),
    m_("planar sync push"),
    r_("planar sync status"),
    m_("planar sync resolve"),
    r_("planar resume validate"),
    m_("planar handoff create"),
    m_("planar handoff validate"),
    m_("planar handoff consume"),
    m_("planar handoff abandon"),
    r_("planar handoff list"),
    r_("planar handoff show"),
    m_("planar capture session"),
    m_("planar capture commits"),
    m_("planar capture end"),
    m_("planar capture note"),
    m_("planar capture command"),
    m_("planar capture file"),
    m_("planar capture snapshot"),
    r_("planar audit trail"),
    r_("planar audit commits"),
    r_("planar audit session"),
    m_("planar audit publish-decision"),
    r_("planar audit handoff-readiness"),
    r_("planar health hygiene"),
    r_("planar models evals"),
    r_("planar models registry list"),
    m_("planar models registry add"),
    m_("planar models registry update"),
    m_("planar models registry remove"),
    m_("planar models registry bind"),
    m_("planar models registry unbind"),
    m_("planar models registry observe"),
    r_("planar models registry eligibility"),
    r_("planar models registry export"),
    r_("planar models registry verify-identity"),
    r_("planar dashboard"),
    m_("planar spec ingest"),
    r_("planar test-spec status"),
    r_("planar config show"),
    m_("planar config edit"),
    r_("planar config validate"),
    m_("planar config init"),
    r_("planar config path"),
    r_("planar templates list"),
    r_("planar templates show"),
    r_("planar templates render"),
    r_("planar templates validate"),
    m_("planar templates init"),
    r_("planar templates path"),
    r_("planar tree"),
    r_("planar search"),
    r_("planar local list"),
    m_("planar local link"),
    m_("planar local unlink"),
    m_("planar local import"),
    m_("planar local migrate"),
    r_("planar skills"),
    m_("planar import"),
    m_("planar synthesize"),
    r_("planar version"),
    r_("planar completion"),
    r_("planar schema"),
    r_("planar report"),
    m_("planar bench start"),
    m_("planar bench event"),
    m_("planar bench touch"),
    m_("planar bench harvest"),
    m_("planar bench finish"),
    r_("planar bench show"),
    m_("planar closure compute"),
    r_("planar closure show"),
    m_("planar run start"),
    m_("planar run event"),
    m_("planar run finish"),
    r_("planar run show"),
    r_("planar groups recommend"),
    m_("planar explore"),
    r_("planar workflow list"),
    r_("planar workflow show"),
    m_("planar workflow run"),
    r_("planar feedback triage list"),
    r_("planar feedback triage show"),
    m_("planar feedback triage set"),
    r_("planar-agent version"),
    m_("planar-agent pull"),
    r_("planar-agent peek"),
    m_("planar-agent complete"),
    m_("planar-agent fail"),
    m_("planar-agent release"),
    m_("planar-agent block"),
    m_("planar-agent claim"),
    m_("planar-agent heartbeat"),
    m_("planar-agent claim-associate"),
    m_("planar-agent action start"),
    m_("planar-agent action end"),
    m_("planar-agent ingest"),
    m_("planar-agent reconcile"),
    m_("planar-agent abort"),
    r_("planar-agent schema"),
    m_("planar-agent run start"),
    m_("planar-agent run end"),
    m_("planar-agent context add"),
    m_("planar-agent context capsule"),
    r_("planar-agent context list"),
    r_("planar models experiments"),
    r_("planar models outcomes"),
    r_("planar models resolve"),
    m_("planar-agent context resolve"),
    m_("planar-agent dispatch preview"),
    m_("planar-agent dispatch confirm"),
    r_("planar-watch feed"),
    r_("planar-watch ps"),
    r_("planar-watch claims"),
    r_("planar-watch actions"),
    r_("planar-watch plans"),
    r_("planar-watch log"),
    r_("planar-watch tree"),
    r_("planar-watch run list"),
    r_("planar-watch run show"),
    r_("planar-watch sync-events"),
    r_("planar-watch version"),
    r_("planar-watch completion"),
    r_("planar-watch schema"),
};

// ---------------------------------------------------------------------------
// checkCapability.
// ---------------------------------------------------------------------------

struct capability_exemption_t {
  std::string_view file;
  std::string_view role;
  std::string_view shape;
};
constexpr std::array<capability_exemption_t, 7> k_capability_exemptions{
    capability_exemption_t{"agents/introspector.md", "introspector", "planar plan create"},
    capability_exemption_t{"agents/introspector.md", "introspector", "planar task add"},
    capability_exemption_t{"agents/introspector.md", "introspector", "planar question add"},
    capability_exemption_t{"agents/reviewer.md", "reviewer", "planar-agent pull"},
    capability_exemption_t{"agents/reviewer.md", "reviewer", "planar-agent claim"},
    capability_exemption_t{"agents/reviewer.md", "reviewer", "planar-agent heartbeat"},
    capability_exemption_t{"agents/reviewer.md", "reviewer", "planar skills render"},
};
auto is_capability_exemption(std::string const& file, std::string_view role, std::string_view shape) -> bool {
  for (auto const& e : k_capability_exemptions)
    if (file == e.file && role == e.role && shape == e.shape)
      return true;
  return false;
}

auto contains_command_shape(std::string_view line, std::string_view shape) -> bool {
  constexpr std::string_view before_set = "$(`/;|&{";
  constexpr std::string_view after_set  = "<[{(\"'`;|&)$}";
  std::size_t                cursor     = 0;
  while (true) {
    auto const at = line.find(shape, cursor);
    if (at == std::string_view::npos)
      break;
    bool const        before_ok = at == 0 || is_ws(line[at - 1]) || before_set.find(line[at - 1]) != std::string_view::npos;
    std::size_t const end       = at + shape.size();
    bool const        after_ok  = end == line.size() || is_ws(line[end]) || after_set.find(line[end]) != std::string_view::npos;
    if (before_ok && after_ok)
      return true;
    cursor = end;
  }
  return false;
}

auto check_executable_capability(std::string const& file, std::string_view role, std::size_t line_no, std::string_view executable,
                                 std::vector<finding_t>& findings, std::vector<suppression_t>& suppressions) -> void {
  for (auto const& c : k_command_classes) {
    if (c.access == access_t::mutate && contains_command_shape(executable, c.shape)) {
      if (is_capability_exemption(file, role, c.shape))
        return;
      emit(findings, suppressions, code::capability, file, line_no,
           std::format("read-only role contains coordination or entity write: {}", c.shape));
      return;
    }
  }
}

auto check_executable_capability_impl(std::string const& file, std::string_view role, std::size_t line_no, std::string_view line,
                                      std::vector<finding_t>& findings, std::vector<suppression_t>& suppressions) -> void {
  std::size_t cursor = 0;
  while (true) {
    auto const open = line.find('`', cursor);
    if (open == std::string_view::npos)
      break;
    std::size_t const          run_len = marker_run_length(line, open, '`');
    std::size_t                search  = open + run_len;
    std::optional<std::size_t> close;
    while (true) {
      auto const candidate = line.find('`', search);
      if (candidate == std::string_view::npos)
        break;
      std::size_t const candidate_len = marker_run_length(line, candidate, '`');
      if (candidate_len == run_len) {
        close = candidate;
        break;
      }
      search = candidate + candidate_len;
    }
    if (!close.has_value())
      break;
    check_executable_capability(file, role, line_no, line.substr(open + run_len, *close - (open + run_len)), findings,
                                suppressions);
    cursor = *close + run_len;
  }
}

auto check_capability(std::string const& file, std::string_view role, std::size_t line_no, std::string_view line, bool in_fence,
                      std::vector<finding_t>& findings, std::vector<suppression_t>& suppressions) -> void {
  if (in_fence) {
    check_executable_capability(file, role, line_no, line, findings, suppressions);
    return;
  }
  check_executable_capability_impl(file, role, line_no, line, findings, suppressions);
}

// ---------------------------------------------------------------------------
// checkCommand (audit trail selector) and checkDeferredCommand.
// ---------------------------------------------------------------------------

auto contains_command_at(std::string_view line, std::size_t at, std::size_t shape_len) -> bool {
  constexpr std::string_view before_set = "$(`/;|&{";
  constexpr std::string_view after_set  = "<[{(\"'`;|&)$}";
  bool const        before_ok = at == 0 || is_ws(line[at - 1]) || before_set.find(line[at - 1]) != std::string_view::npos;
  std::size_t const end       = at + shape_len;
  bool const        after_ok  = end == line.size() || is_ws(line[end]) || after_set.find(line[end]) != std::string_view::npos;
  return before_ok && after_ok;
}

auto next_command_token(std::string_view args, std::size_t& cursor) -> std::optional<std::string_view> {
  constexpr std::string_view stop_start = "`;) |&";
  constexpr std::string_view stop_body  = "`,;)|&";
  while (cursor < args.size() && is_ws(args[cursor]))
    ++cursor;
  if (cursor >= args.size() || stop_start.find(args[cursor]) != std::string_view::npos)
    return std::nullopt;
  std::size_t const start = cursor;
  while (cursor < args.size() && !is_ws(args[cursor]) && stop_body.find(args[cursor]) == std::string_view::npos)
    ++cursor;
  return args.substr(start, cursor - start);
}

auto is_valid_audit_kind(std::string_view token) -> bool {
  if (token == "<kind>")
    return true;
  constexpr std::array<std::string_view, 6> kinds{"plan", "task", "question", "scenario", "decision", "artifact"};
  return std::ranges::find(kinds, token) != kinds.end();
}

auto valid_audit_selector_args(std::string_view args) -> bool {
  std::size_t cursor   = 0;
  auto const  selector = next_command_token(args, cursor);
  if (!selector.has_value())
    return false;
  auto const value = next_command_token(args, cursor);
  if (!value.has_value())
    return false;
  if (*selector == "--link")
    return !value->empty() && (*value)[0] != '-';
  if (*selector != "--kind" || !is_valid_audit_kind(*value))
    return false;
  auto const entity_id = next_command_token(args, cursor);
  if (!entity_id.has_value())
    return false;
  return !entity_id->empty() && (*entity_id)[0] != '-';
}

auto is_invalid_audit_selector(std::string_view token) -> bool {
  if (token.empty())
    return false;
  constexpr std::array<std::string_view, 3> kinds{"plan", "task", "question"};
  for (auto kind : kinds) {
    if (token == kind || (token.starts_with(kind) && token.size() > kind.size() && token[kind.size()] == ':'))
      return true;
    if ((token[0] == '<' || token[0] == '{') && token.find(kind) != std::string_view::npos)
      return true;
  }
  return (token[0] == '<' || token[0] == '{') && token.find("kind:") != std::string_view::npos;
}

auto check_command(std::string const& file, std::size_t line_no, std::string_view line, std::vector<finding_t>& findings,
                   std::vector<suppression_t>& suppressions) -> void {
  constexpr std::string_view shape  = "planar audit trail";
  std::size_t                cursor = 0;
  while (true) {
    auto const at = line.find(shape, cursor);
    if (at == std::string_view::npos)
      break;
    std::size_t const end = at + shape.size();
    cursor                = end;
    if (!contains_command_at(line, at, shape.size()))
      continue;
    std::string_view const args = trim_start(line.substr(end), " \t");
    if (args.empty() || args[0] == '`' || args[0] == '.' || args[0] == ',' || args[0] == ')' || args.starts_with("[--kind"))
      continue;
    if (args.starts_with("--kind") || args.starts_with("--link")) {
      if (valid_audit_selector_args(args))
        continue;
      emit(findings, suppressions, code::command, file, line_no,
           "audit trail selector requires `--kind <kind> <entity-id>` or `--link <link-id>`");
      return;
    }
    auto const             token_end = args.find_first_of(" \t`.,)");
    std::string_view const token     = token_end == std::string_view::npos ? args : args.substr(0, token_end);
    if (!is_invalid_audit_selector(token))
      continue;
    emit(findings, suppressions, code::command, file, line_no, "audit trail entity kind must use `--kind <kind> <entity-id>`");
    return;
  }
}

auto check_deferred_executable(std::string const& file, std::size_t line_no, std::string_view executable,
                               std::vector<finding_t>& findings, std::vector<suppression_t>& suppressions) -> void {
  if (!contains_command_shape(executable, "planar links update"))
    return;
  emit(findings, suppressions, code::command, file, line_no,
       "deferred `planar links update` cannot be presented as an executable current workflow; use unlink/link recovery");
}

auto check_deferred_command(std::string const& file, std::size_t line_no, std::string_view line, bool in_fence,
                            std::vector<finding_t>& findings, std::vector<suppression_t>& suppressions) -> void {
  if (!file.starts_with("agents/") && !file.starts_with("skills/src/"))
    return;
  if (in_fence) {
    check_deferred_executable(file, line_no, line, findings, suppressions);
    return;
  }
  std::size_t cursor = 0;
  while (true) {
    auto const open = line.find('`', cursor);
    if (open == std::string_view::npos)
      break;
    std::size_t const          run_len = marker_run_length(line, open, '`');
    std::size_t                search  = open + run_len;
    std::optional<std::size_t> close;
    while (true) {
      auto const candidate = line.find('`', search);
      if (candidate == std::string_view::npos)
        break;
      std::size_t const candidate_len = marker_run_length(line, candidate, '`');
      if (candidate_len == run_len) {
        close = candidate;
        break;
      }
      search = candidate + candidate_len;
    }
    if (!close.has_value())
      break;
    check_deferred_executable(file, line_no, line.substr(open + run_len, *close - (open + run_len)), findings, suppressions);
    cursor = *close + run_len;
  }
}

// ---------------------------------------------------------------------------
// Frontmatter.
// ---------------------------------------------------------------------------

auto frontmatter_value(std::string_view content, std::string_view key) -> std::optional<std::string_view> {
  if (!content.starts_with("---\n"))
    return std::nullopt;
  for (auto line : split_lines(content.substr(4))) {
    if (trim(line, " \t\r") == "---")
      break;
    auto const colon = line.find(':');
    if (colon == std::string_view::npos)
      continue;
    if (trim(line.substr(0, colon), " \t") == key)
      return trim(line.substr(colon + 1), " \t\r\"");
  }
  return std::nullopt;
}
auto frontmatter_literal_true(std::string_view content, std::string_view key) -> bool {
  if (!content.starts_with("---\n"))
    return false;
  for (auto line : split_lines(content.substr(4))) {
    if (trim(line, " \t\r") == "---")
      break;
    auto const colon = line.find(':');
    if (colon == std::string_view::npos)
      continue;
    if (trim(line.substr(0, colon), " \t") == key)
      return trim(line.substr(colon + 1), " \t\r") == "true";
  }
  return false;
}

// ---------------------------------------------------------------------------
// checkFeedbackContract.
// ---------------------------------------------------------------------------

auto heading_label(std::string_view line) -> std::optional<std::string_view> {
  if (line.size() < 3 || line[0] != '#')
    return std::nullopt;
  std::size_t hash_count = 1;
  while (hash_count < line.size() && line[hash_count] == '#')
    ++hash_count;
  if (hash_count == line.size() || line[hash_count] != ' ')
    return std::nullopt;
  return trim(line.substr(hash_count + 1), " \t\r");
}

auto check_feedback_contract(std::string const& file, std::string_view content, std::vector<finding_t>& findings,
                             std::vector<suppression_t>& suppressions) -> void {
  constexpr std::array<std::string_view, 7> required{"Context",  "Intent",       "Actions", "Result",
                                                     "Warnings", "Next actions", "Recovery"};
  std::array<std::size_t, 7>                counts{};
  std::array<std::optional<std::size_t>, 7> malformed_lines{};
  std::optional<fence_t>                    fence;
  std::size_t                               line_no = 0;
  for (auto raw : split_lines(content)) {
    ++line_no;
    std::string_view const line = trim(raw, " \t\r");
    if (advance_fence(line, fence) || fence.has_value())
      continue;
    for (std::size_t idx = 0; idx < required.size(); ++idx) {
      std::string const literal = std::format("## {}", required[idx]);
      if (line == literal) {
        ++counts[idx];
        if (counts[idx] > 1)
          emit(findings, suppressions, code::contract, file, line_no,
               std::format("duplicate required feedback heading `{}`; keep exactly one literal H2 section", literal));
      } else if (!malformed_lines[idx].has_value()) {
        if (auto candidate = heading_label(line); candidate.has_value() && *candidate == required[idx])
          malformed_lines[idx] = line_no;
      }
    }
  }
  for (std::size_t idx = 0; idx < required.size(); ++idx) {
    if (counts[idx] != 0)
      continue;
    std::string const literal = std::format("## {}", required[idx]);
    if (malformed_lines[idx].has_value()) {
      emit(findings, suppressions, code::contract, file, *malformed_lines[idx],
           std::format("required feedback section must be the literal H2 heading `{}`", literal));
    } else {
      emit(findings, suppressions, code::contract, file, 1,
           std::format(
               "user-invocable skill is missing required feedback heading `{}`; add that literal H2 section or declare a genuine "
               "helper as `internal_only: true` in frontmatter",
               literal));
    }
  }
}

// ---------------------------------------------------------------------------
// checkQueueCommands (plan 1080, task hq-rule-lint) — the queue rule for
// authored agent and skill sources.
//
// Every build and test run on a host goes through `planar-agent queue run`
// (agents/methodology.md § Builds and tests go through the host queue). The
// rule only works when the text agents read never tells them to run such a
// command directly, so this pass flags an inline code span, or a line of a
// fenced code block, whose FIRST word is a build or test program and which
// is therefore not given to the queue: a command given to the queue begins
// `planar-agent queue run`, and its program follows a bare `--`.
//
// The program list is the product spec's "What counts as a build or test
// command", exactly: make, cmake --build, ninja, ctest, cargo build/test, go
// build/test, npm test, pytest, tox, gradle build. "Linters that build first"
// have no program name to match and are the reader's judgement. `cmake
// --preset` (a configure) and `npm install` are not builds and are not
// listed. A program word is matched whole: `make:` and `makefile` are not
// `make`.
//
// SCOPE: agents/ and skills/src/ only. docs/ describes the tools and is not
// an instruction to an agent. Prose outside a span or block is never read.
//
// Escape hatches, deliberately separate from `surface-lint-ignore` (which
// needs a code and a rationale comment on the line above, and cannot sit
// inside a table row or a verbatim section):
//   - a line carrying `queue-lint-ignore` is exempt. Use it on a line that
//     must keep the command, such as a failure-signature table row, and say
//     why beside it.
//   - a region between a `<!-- queue-lint-ignore-begin ... -->` line and a
//     `<!-- queue-lint-ignore-end -->` line is exempt. It exists for text
//     that is pinned byte for byte elsewhere, where a per-line marker would
//     change the bytes: the queue rule's own section in agents/methodology.md.
//     The marker lines sit outside the pinned text. A marker that is
//     unbalanced is itself a finding, so a typo cannot exempt the rest of a
//     file.
// ---------------------------------------------------------------------------

constexpr std::string_view k_queue_ignore       = "queue-lint-ignore";
constexpr std::string_view k_queue_region_begin = "<!-- queue-lint-ignore-begin";
constexpr std::string_view k_queue_region_end   = "<!-- queue-lint-ignore-end";

/// A build or test program: its first word, and the second word that must
/// follow it (empty when the program alone is enough).
struct build_program_t {
  std::string_view first;
  std::string_view second;
};

constexpr std::array<build_program_t, 12> k_build_programs{{{"make", ""},
                                                            {"ninja", ""},
                                                            {"ctest", ""},
                                                            {"pytest", ""},
                                                            {"tox", ""},
                                                            {"cmake", "--build"},
                                                            {"cargo", "build"},
                                                            {"cargo", "test"},
                                                            {"go", "build"},
                                                            {"go", "test"},
                                                            {"npm", "test"},
                                                            {"gradle", "build"}}};

/// @brief The next run of non-blank characters of `text` from `cursor`;
/// advances `cursor` past it.
auto next_word(std::string_view text, std::size_t& cursor) -> std::string_view {
  while (cursor < text.size() && is_ws(text[cursor]))
    ++cursor;
  std::size_t const start = cursor;
  while (cursor < text.size() && !is_ws(text[cursor]))
    ++cursor;
  return text.substr(start, cursor - start);
}

/// @brief Is `text`, taken as one command, a build or test command: does it
/// begin (after an optional `$ ` prompt) with a listed program?
auto begins_with_build_program(std::string_view text) -> bool {
  std::size_t cursor = 0;
  auto        first  = next_word(text, cursor);
  if (first == "$")
    first = next_word(text, cursor);
  if (first.empty())
    return false;
  auto const second = next_word(text, cursor);
  for (auto const& program : k_build_programs) {
    if (first == program.first && (program.second.empty() || second == program.second))
      return true;
  }
  return false;
}

auto queue_command_finding(std::vector<finding_t>& findings, std::string const& file, std::size_t line_no, std::string_view text)
    -> void {
  findings.push_back({.code    = code::queue_command,
                      .file    = file,
                      .line    = line_no,
                      .message = std::format("build or test command is not given to `planar-agent queue run`: `{}`", text)});
}

auto check_queue_commands(std::string const& file, std::vector<std::string_view> const& lines, std::vector<finding_t>& findings)
    -> void {
  std::optional<fence_t>     fence;
  std::optional<std::size_t> region_line;
  for (std::size_t idx = 0; idx < lines.size(); ++idx) {
    std::size_t const      line_no = idx + 1;
    std::string_view const line    = lines[idx];
    std::string_view const trimmed = trim(line, " \t\r");
    if (advance_fence(trimmed, fence))
      continue;
    if (!fence.has_value()) {
      if (trimmed.starts_with(k_queue_region_begin) && trimmed.ends_with("-->")) {
        if (region_line.has_value())
          findings.push_back(
              {.code    = code::queue_marker,
               .file    = file,
               .line    = line_no,
               .message = std::format("`queue-lint-ignore-begin` inside the region opened at line {}", *region_line)});
        else
          region_line = line_no;
        continue;
      }
      if (trimmed.starts_with(k_queue_region_end) && trimmed.ends_with("-->")) {
        if (region_line.has_value())
          region_line.reset();
        else
          findings.push_back({.code    = code::queue_marker,
                              .file    = file,
                              .line    = line_no,
                              .message = "`queue-lint-ignore-end` without a matching `queue-lint-ignore-begin`"});
        continue;
      }
    }
    if (region_line.has_value() || line.find(k_queue_ignore) != std::string_view::npos)
      continue;
    if (fence.has_value()) {
      if (begins_with_build_program(trimmed))
        queue_command_finding(findings, file, line_no, trimmed);
      continue;
    }
    std::size_t cursor = 0;
    while (true) {
      auto const open = line.find('`', cursor);
      if (open == std::string_view::npos)
        break;
      std::size_t const          run_len = marker_run_length(line, open, '`');
      std::size_t                search  = open + run_len;
      std::optional<std::size_t> close;
      while (true) {
        auto const candidate_close = line.find('`', search);
        if (candidate_close == std::string_view::npos)
          break;
        std::size_t const candidate_len = marker_run_length(line, candidate_close, '`');
        if (candidate_len == run_len) {
          close = candidate_close;
          break;
        }
        search = candidate_close + candidate_len;
      }
      if (!close.has_value())
        break;
      std::string_view const span = trim(line.substr(open + run_len, *close - (open + run_len)), " \t");
      if (begins_with_build_program(span))
        queue_command_finding(findings, file, line_no, span);
      cursor = *close + run_len;
    }
  }
  if (region_line.has_value())
    findings.push_back(
        {.code = code::queue_marker, .file = file, .line = *region_line, .message = "`queue-lint-ignore-begin` is never closed"});
}

// ---------------------------------------------------------------------------
// scanFile / scanRepository.
// ---------------------------------------------------------------------------

auto is_authored_surface(std::string_view path) -> bool {
  if (!path.starts_with("agents/"))
    return true;
  return !path.starts_with("agents/claude/") && !path.starts_with("agents/codex/") && !path.starts_with("agents/copilot/");
}

auto relative_path(std::string const& path, std::string const& root) -> std::string {
  if (path.starts_with(root))
    return std::string{trim_start(std::string_view{path}.substr(root.size()), "/")};
  return path;
}

auto scan_file_impl(fs::path const& root, std::string const& rel_file, fs::path const& abs_file, std::string const& content,
                    result_t& result) -> void {
  auto const                 lines = split_lines(content);
  std::vector<suppression_t> suppressions;
  parse_suppressions(rel_file, lines, result.findings, suppressions);
  bool const             read_only = frontmatter_value(content, "capability").value_or("") == "read-only";
  std::string const      role      = std::string{frontmatter_value(content, "role").value_or("")};
  std::optional<fence_t> fence;
  for (std::size_t idx = 0; idx < lines.size(); ++idx) {
    std::size_t const      line_no = idx + 1;
    std::string_view const line    = lines[idx];
    std::string_view const trimmed = trim(line, " \t\r");
    if (advance_fence(trimmed, fence))
      continue;
    check_links(root, rel_file, abs_file, line_no, line, result.findings, suppressions);
    check_legacy(rel_file, line_no, line, result.findings, suppressions);
    check_artifact_set(rel_file, line_no, line, result.findings, suppressions);
    check_path_citations(root, rel_file, line_no, line, result.findings, suppressions);
    if (read_only)
      check_capability(rel_file, role, line_no, line, fence.has_value(), result.findings, suppressions);
    check_command(rel_file, line_no, line, result.findings, suppressions);
    check_deferred_command(rel_file, line_no, line, fence.has_value(), result.findings, suppressions);
  }
  check_retired_references(rel_file, lines, result.findings);
  if (rel_file.starts_with("agents/") || rel_file.starts_with("skills/src/"))
    check_queue_commands(rel_file, lines, result.findings);
  if (rel_file.starts_with("skills/src/") && !frontmatter_literal_true(content, "internal_only"))
    check_feedback_contract(rel_file, content, result.findings, suppressions);
  for (auto const& s : suppressions) {
    if (!s.used)
      result.findings.push_back(
          {.code    = code::suppression_unused,
           .file    = rel_file,
           .line    = s.declaration_line,
           .message = std::format("suppression for {} does not match a finding on the next non-blank line", s.code)});
  }
}

auto collect_markdown(fs::path const& dir, std::vector<fs::path>& paths) -> void {
  std::error_code ec;
  if (!fs::exists(dir, ec))
    return;
  for (auto const& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    if (entry.is_directory()) {
      collect_markdown(entry.path(), paths);
    } else if (entry.is_regular_file() && entry.path().extension() == ".md") {
      paths.push_back(entry.path());
    }
  }
}

/// Reads the files the retired-reference scope names that the general scan does
/// not (`README.md`, `INSTALL.md`, a regular-file `AGENTS.md`, `copilot/`) and
/// runs ONLY `check_retired_references` over them: the other checks were
/// written for `agents/`, `skills/src/` and `docs/` and are not asked of a
/// root guide. `AGENTS.md` is skipped when it is a symlink, because the
/// general scan already read its target as `CLAUDE.md`.
auto scan_retired_only_files(fs::path const& root, result_t& result) -> void {
  std::vector<fs::path> paths;
  for (auto name : {"README.md", "INSTALL.md", "AGENTS.md"}) {
    std::error_code ec;
    auto const      path = root / name;
    if (fs::is_regular_file(path, ec) && !fs::is_symlink(path, ec))
      paths.push_back(path);
  }
  collect_markdown(root / "copilot", paths);
  std::ranges::sort(paths);
  for (auto const& path : paths) {
    std::string const rel_path = relative_path(path.string(), root.string());
    std::ifstream     in(path, std::ios::binary);
    std::string       content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check_retired_references(rel_path, split_lines(content), result.findings);
    ++result.files_scanned;
  }
}

auto scan_repository(fs::path const& root) -> result_t {
  result_t              result;
  std::vector<fs::path> paths;
  for (auto rel : k_scan_dirs)
    collect_markdown(root / rel, paths);
  // CLAUDE.md (task 6930): a single top-level file, not a directory under
  // `k_scan_dirs`, so it needs its own entry point rather than a `collect_
  // markdown` walk. `AGENTS.md` is a symlink to `CLAUDE.md` (see this
  // repo's own CLAUDE.md Â§ Operating Rules) and is deliberately NOT scanned
  // separately: `fs::directory_iterator` would otherwise open it as a
  // second regular file with identical content, and every finding in it
  // would be reported twice under two different names.
  if (std::error_code ec; fs::exists(root / "CLAUDE.md", ec) && fs::is_regular_file(root / "CLAUDE.md", ec))
    paths.push_back(root / "CLAUDE.md");
  std::ranges::sort(paths);
  for (auto const& path : paths) {
    std::string const rel_path = relative_path(path.string(), root.string());
    if (!is_authored_surface(rel_path))
      continue;
    std::ifstream in(path, std::ios::binary);
    std::string   content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    scan_file_impl(root, rel_path, path, content, result);
    ++result.files_scanned;
  }
  scan_retired_only_files(root, result);
  return result;
}

auto finding_less_than(finding_t const& a, finding_t const& b) -> bool {
  if (a.file != b.file)
    return a.file < b.file;
  if (a.line != b.line)
    return a.line < b.line;
  if (a.code != b.code)
    return a.code < b.code;
  return a.message < b.message;
}

// ---------------------------------------------------------------------------
// Output — this tool's OWN JSON string escaper (see this file's header:
// distinct from planar::json_text's, no \b/\f short forms).
// ---------------------------------------------------------------------------

auto append_json_string(std::string& out, std::string_view value) -> void {
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
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
      if (c < 0x20) {
        out += std::format("\\u00{:02x}", c);
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  out.push_back('"');
}

auto write_text(result_t const& result) -> std::string {
  std::string out;
  for (auto const& f : result.findings)
    out += std::format("{}:{}: {}: {}\n", f.file, f.line, f.code, f.message);
  if (result.findings.empty()) {
    out += std::format("surface-lint: clean ({} files)\n", result.files_scanned);
  } else {
    out += std::format("surface-lint: {} finding(s) across {} files\n", result.findings.size(), result.files_scanned);
  }
  return out;
}

auto write_json(result_t const& result) -> std::string {
  std::string out = std::format("{{\"version\":1,\"ok\":{},\"files_scanned\":{},\"findings\":[",
                                result.findings.empty() ? "true" : "false", result.files_scanned);
  for (std::size_t idx = 0; idx < result.findings.size(); ++idx) {
    if (idx != 0)
      out.push_back(',');
    auto const& f = result.findings[idx];
    out += "{\"code\":";
    append_json_string(out, f.code);
    out += ",\"file\":";
    append_json_string(out, f.file);
    out += std::format(",\"line\":{},\"message\":", f.line);
    append_json_string(out, f.message);
    out.push_back('}');
  }
  out += "]}\n";
  return out;
}

auto write_command_inventory() -> std::string {
  std::string out = "{\"version\":1,\"commands\":[";
  for (std::size_t idx = 0; idx < k_command_classes.size(); ++idx) {
    if (idx != 0)
      out.push_back(',');
    auto const& c = k_command_classes[idx];
    out += "{\"command\":";
    append_json_string(out, c.shape);
    out += ",\"access\":";
    append_json_string(out, c.access == access_t::mutate ? "mutate" : "read");
    out.push_back('}');
  }
  out += "]}\n";
  return out;
}

[[noreturn]] auto usage() -> void {
  std::println(stderr, "usage: surface_lint <repo-root> [--json]\n       surface_lint --command-inventory-json");
  std::exit(2);
}

} // namespace

/// @brief Entry point: semantic-validate authored Markdown under
/// `<repo-root>/{agents,skills/src,docs}`, or dump the pinned
/// `command_classes` inventory as JSON.
/// @param argc Argument count.
/// @param argv Argument vector: either `<repo-root> [--json]`, or the single
/// flag `--command-inventory-json` (mutually exclusive with everything else).
/// @return 0 when no finding is present; 1 when at least one finding is
/// present; 2 on a usage error or an internal scan failure.
auto main(int argc, char** argv) -> int {
  std::vector<std::string> args(argv, argv + argc);
  if (args.size() == 2 && args[1] == "--command-inventory-json") {
    std::print("{}", write_command_inventory());
    return 0;
  }
  if (args.size() < 2)
    usage();
  std::optional<std::string> root;
  bool                       json = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    std::string const& arg = args[i];
    if (arg == "--json") {
      json = true;
    } else if (arg.starts_with('-') || root.has_value()) {
      usage();
    } else {
      root = arg;
    }
  }
  if (!root.has_value())
    usage();

  result_t result;
  try {
    result = scan_repository(fs::path{*root});
  } catch (std::exception const& e) {
    std::println(stderr, "surface-lint: internal error: {}", e.what());
    return 2;
  }
  std::ranges::sort(result.findings, finding_less_than);
  std::print("{}", json ? write_json(result) : write_text(result));
  return result.findings.empty() ? 0 : 1;
}
