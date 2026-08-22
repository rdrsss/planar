/// @file parse.cppm
/// @brief `planar.engine.ingest.parse` — Markdown extraction for tech-spec,
/// roadmap, and test-spec workbench documents.
///
/// Behavior-preserving port (D2) of `zig/src/engine/ingestor/parse.zig`. Pure
/// data transformation: no database handle, no filesystem access, no output.
/// The intermediate representation produced here is what `diff` reconciles
/// against stored rows and what `coverage` measures the strict gate over, so
/// every extraction rule below is load-bearing for the M9 parity gate.
///
/// Conventions (locked, mirrored from the Zig original's header):
///
///   * Tech-spec H2 `## Decisions` → each `### <title>` H3 is one decision.
///     When the section contains NO H3 headings at all, each `- ` / `* `
///     bullet becomes one decision instead (the bolded lead is the title).
///     H3 wins whenever both shapes are present.
///   * Tech-spec H2 `## Open Questions` → each `### <title>` H3 is one
///     question; a body whose first non-blank line begins with the literal
///     `Resolution:` (case-sensitive) populates `question::resolution_`.
///   * Roadmap H2 `## <milestone>` → each bullet becomes a `work_item`.
///     `[touches: a, b]`, `[depends: other-slug]` and `[slug: foo-bar]`
///     annotations are lifted out of the display title and stored separately;
///     `[slug: ...]` is sanitized to lowercase ASCII + digits + `-`.
///     Indented continuation lines are folded onto the bullet BEFORE
///     annotations are extracted, so an annotation an operator wrote on a
///     wrapped line is not silently dropped.
///   * Test-spec H2 `## Scenarios` → `### Scenario: <title>` H3 items, or
///     `#### Scenario: <title>` H4 items nested under a bucket-group H3.
///     Three field lines are lifted out of the body:
///         **Verifies:** task:<slug-or-id>[, ...]
///         **Kind:**      <free-form>
///         **Acceptance:** <one-line>
///     The comma-split / trim / slug-validate steps on `**Verifies:**` are
///     what the coverage gate counts; malformed entries are dropped silently
///     exactly as the Zig original drops them.
///
/// Unlike the Zig original there are no `deinit` companions: every returned
/// type owns its storage through `std::string` / `std::vector`, so lifetime
/// is structural rather than a convention the caller has to honor.

module;

export module planar.engine.ingest.parse;

import std;

namespace planar::engine::ingest::parse {

/// @brief One parsed decision from a tech-spec `## Decisions` section.
export struct decision {
  std::string title_; ///< H3 heading text, or the bolded lead of a bullet.
  std::string body_;  ///< Block body, trimmed of surrounding blank lines.
};

/// @brief One parsed question from a tech-spec `## Open Questions` section.
///
/// `body_` always retains the full H3 body INCLUDING the `Resolution:` line
/// when one is present; `resolution_` additionally carries just the resolution
/// text (empty when the question is still open).
export struct question {
  std::string title_;      ///< H3 heading text.
  std::string body_;       ///< Full H3 body, trimmed.
  std::string resolution_; ///< Text after the `Resolution:` marker, or empty.
};

/// @brief One roadmap bullet.
///
/// `title_` is the display text with every `[…: …]` annotation removed;
/// `source_text_` is the canonical folded bullet INCLUDING its annotations,
/// and is what ingestion stages as a citation's `source_text` — the digest
/// over it is how a citation to an edited roadmap item goes stale.
export struct work_item {
  std::string              title_;       ///< Display text, annotations removed.
  std::vector<std::string> touches_;     ///< From `[touches: a, b]`.
  std::vector<std::string> depends_;     ///< Slugs from `[depends: …]`.
  std::string              slug_;        ///< Sanitized `[slug: …]`, or empty.
  std::string              source_text_; ///< Canonical folded bullet.
};

/// @brief One H2 milestone in a roadmap.
export struct milestone {
  std::string            name_;       ///< H2 heading text.
  std::string            intent_;     ///< First paragraph below the H2, space-folded.
  std::vector<work_item> work_items_; ///< Bullets, in source order.
};

/// @brief One `**Verifies:**` entry from a test-spec scenario.
///
/// `id_` and `slug_` are mutually exclusive: exactly one is set on every
/// successfully-parsed ref. Slug-form refs feed the coverage gate; numeric
/// refs are ignored for coverage (at preview time a numeric id cannot be tied
/// to a roadmap bullet) but still produce a `verifies` edge at apply time.
export struct task_ref {
  std::string  kind_;   ///< Ref kind; defaults to `task` for a bare integer.
  std::int64_t id_ = 0; ///< Numeric id, or 0 when this is a slug ref.
  std::string  slug_;   ///< Slug, or empty when this is a numeric ref.
};

/// @brief One scenario under a test-spec `## Scenarios` section.
export struct scenario {
  std::string           title_;      ///< Scenario heading text.
  std::string           kind_;       ///< Free-form `**Kind:**` value.
  std::string           acceptance_; ///< One-line `**Acceptance:**` value.
  std::vector<task_ref> verifies_;   ///< Parsed `**Verifies:**` refs.
  std::string           body_;       ///< Prose, with field lines removed.
};

/// @brief Parses the `## Decisions` H2 section of a tech-spec body.
///
/// Prefers `### <title>` H3 blocks. Falls back to `- ` / `* ` bullets only
/// when the section contains no H3 heading at all; in that shape the title is
/// the first `**…**` bold run, else the first sentence, else the whole bullet.
/// @param body The full tech-spec body.
/// @return One entry per decision, in source order; empty when the section is
/// absent.
export [[nodiscard]] auto parse_tech_spec_decisions(std::string_view body) -> std::vector<decision>;

/// @brief Parses the `## Open Questions` H2 section of a tech-spec body.
/// @param body The full tech-spec body.
/// @return One entry per `### <title>` H3, in source order; empty when the
/// section is absent.
export [[nodiscard]] auto parse_tech_spec_open_questions(std::string_view body) -> std::vector<question>;

/// @brief Parses a roadmap body into its flat list of milestones.
///
/// Each H2 (but not H3) starts a new milestone; the first non-blank paragraph
/// below it is the intent; bulleted lines — with their indented continuation
/// lines folded on — are the work items.
/// @param body The full roadmap body.
/// @return One entry per H2, in source order.
export [[nodiscard]] auto parse_roadmap(std::string_view body) -> std::vector<milestone>;

/// @brief Parses the `## Scenarios` H2 section of a test-spec body.
///
/// An H3 is treated as a bucket-group header (and skipped) only when it has H4
/// children AND carries neither the `Scenario: ` prefix nor a `**Verifies:**`
/// line. An H3 with no H4 children is always emitted, which is what keeps
/// every pre-existing flat spec parsing unchanged.
/// @param body The full test-spec body.
/// @return One entry per scenario, in source order; empty when the section is
/// absent.
export [[nodiscard]] auto parse_test_spec(std::string_view body) -> std::vector<scenario>;

/// @brief Whether the named H2 section exists AND holds at least one non-blank
/// line.
///
/// Lets the ingest layer distinguish "no such section" from "section present
/// but its shape produced zero entities", so the latter can be reported
/// loudly instead of silently discarding authored content.
/// @param body The document body to scan.
/// @param header The exact H2 heading line to look for (e.g. `## Scenarios`).
/// @return `true` when the section exists and has content.
export [[nodiscard]] auto section_has_content(std::string_view body, std::string_view header) -> bool;

/// @brief Normalizes a slug annotation value: trim, lowercase ASCII, collapse
/// every run of non-`[a-z0-9]` to a single `-`, strip leading/trailing `-`.
/// @param raw The raw annotation text.
/// @return The sanitized slug; empty when `raw` holds no slug characters.
export [[nodiscard]] auto sanitize_slug(std::string_view raw) -> std::string;

} // namespace planar::engine::ingest::parse
