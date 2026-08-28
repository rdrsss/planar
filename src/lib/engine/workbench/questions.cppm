/// @file questions.cppm
/// @brief `planar.engine.workbench.questions` — the read-only Markdown pass
/// behind `planar workbench extract-questions` (plan 996, task 6302).
///
/// Behavior-preserving port (D2) of the pure halves of
/// `zig/src/cmd/planar/handlers/workbench/extract_questions.zig` (402
/// lines). The Zig original keeps all of this in the HANDLER; it lands in
/// the engine here for the same reason every other `workbench` leaf's
/// printing half did — so the exact bytes are testable without spawning a
/// process.
///
/// It shares no code with the sync engine and touches no database. What is
/// here is a directory listing, a section walk, and two renderers.
///
/// ## THE SCAN IS TOP-LEVEL AND SKIPS `README.md`
///
/// `collect_top_level_specs` reads ONLY regular `.md` files directly inside
/// the feature directory, and drops `README.md`. That is not an incidental
/// detail — it is the single most misleading property of this leaf, and it
/// has produced a vacuous green three times. A feature tree that is a
/// README plus subdirectories (`questions/`, `tasks/`, …) yields an EMPTY
/// file list, so every downstream assertion passes against nothing.
///
/// A fixture for this module must therefore contain at least one top-level
/// non-README spec whose front matter actually parses, and a test must
/// assert the extraction is NON-EMPTY before comparing it to anything.
/// `fsutil::collect_markdown` is deliberately NOT reused: it recurses and
/// it keeps `README.md`.
///
/// ## Which branch runs
///
/// The walk finds the first `## Open Questions` H2 (case-insensitive),
/// takes every line until the next H1/H2, and then picks ONE branch for the
/// whole section: if any `### ` line is present it is the H3 branch, else
/// the bullet branch. They are not combined, so a section mixing both
/// yields only the H3 questions.
///
///   - H3 branch: the heading text is the title, and everything up to the
///     next `### ` — blank-trimmed at both ends, joined with `\n` — is the
///     body.
///   - Bullet branch: every `- ` / `* ` line at ANY indent (nested bullets
///     included) is its own question, split at the first `. ` / `? ` / `! `
///     into title and body.
///
/// `source_line` is 1-based within the PARSED BODY, not within the file, so
/// it does not count the front matter.
module;

export module planar.engine.workbench.questions;

import std;

namespace planar::engine::workbench::questions {

/// @brief One extracted question.
export struct question {
  std::string title;           ///< The heading text or the first sentence of the bullet.
  std::string body;            ///< The remainder; empty when there is none.
  std::size_t source_line = 0; ///< 1-based line within the parsed body.

  /// @brief Memberwise equality, for the extraction tests.
  /// @return Whether every field matches.
  auto operator==(const question&) const -> bool = default;
};

/// @brief One scanned file and what it yielded.
export struct file_questions {
  std::int64_t          artifact_id = 0; ///< `entity_id` from the file's front matter.
  std::string           file;            ///< The file's basename.
  std::vector<question> questions;       ///< Possibly empty; the file is still reported.
};

/// @brief Every top-level, non-`README.md`, regular `.md` file in `dir`.
///
/// Returned as BASENAMES, sorted bytewise, mirroring the Zig original's
/// `insertionSortStrings` over `std.mem.order`. An absent or unreadable
/// directory is an empty list, never an error.
///
/// See this module's header: the top-level-only, README-skipping shape is
/// the vacuous-fixture trap.
/// @param dir The feature directory.
/// @return The basenames to scan.
export auto collect_top_level_specs(const std::filesystem::path& dir) -> std::vector<std::string>;

/// @brief Extract every question from one parsed body.
/// @param body The document body, front matter already stripped.
/// @return The questions, empty when there is no `## Open Questions` H2.
export auto extract_questions(std::string_view body) -> std::vector<question>;

/// @brief Render the operator-facing text form.
///
/// One header line per file, then one indented line per question. A body
/// longer than 60 BYTES is truncated at 60 and followed by `…`; the cut is
/// bytewise and can split a UTF-8 sequence, exactly as the oracle's
/// `q.body[0..60]` does.
/// @param results The scanned files.
/// @return The rendered text, newline-terminated per line.
export auto render_text(std::span<const file_questions> results) -> std::string;

/// @brief Render the `--json` form.
///
/// Matches `std.json.Stringify.value` over the oracle's result slice: field
/// order is declaration order (`artifact_id`, `file`, `questions`; then
/// `title`, `body`, `source_line`), no whitespace, and an empty list is
/// `[]`.
/// @param results The scanned files.
/// @return The JSON array, WITHOUT a trailing newline.
export auto render_json(std::span<const file_questions> results) -> std::string;

} // namespace planar::engine::workbench::questions
