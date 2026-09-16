/// @file materialize.cppm
/// @brief `planar.engine.ingest.materialize` — stable, provenance-bearing task
/// facts derived during spec ingest.
///
/// Behavior-preserving port (D2) of `zig/src/engine/ingestor/materialize.zig`,
/// with ONE deliberate contract improvement, described below.
///
/// The caller owns the transaction. Facts are staged as a complete set for
/// every task below an anchor plan, compared against the authoritative set,
/// and replaced only when the two differ semantically — so a replay writes
/// nothing and stored fact ids stay stable.
///
/// ## The citation diagnostic travels IN the error
///
/// The Zig original builds a precise diagnostic for every unresolvable
/// citation (`buildCitationDiagnostic`) and then hands it back through an
/// OPTIONAL out-parameter sink, separately from the `InvalidCitation` error
/// itself. A caller that returns the error without also draining the sink
/// reports `apply failed: InvalidCitation` — naming no task, no artifact, no
/// locator, and no section. That has cost this project real time twice (see
/// task 6048's addenda 2 and 3): the offender was invisible because the
/// obvious search for it, `task list --plan N`, hides done tasks while the
/// staging query joins tasks regardless of status.
///
/// Here, `materialize_error` CARRIES the diagnostics. It is not possible to
/// obtain `invalid_citation` without also holding the detail, so the
/// unnameable failure cannot be reproduced by a caller that forgets to wire a
/// sink. `citation_diagnostic::describe()` renders the whole story in one
/// line, and additionally names the most common root cause — a locator
/// truncated mid-heading — when it can detect it.
///
/// The locator grammar itself is ported unchanged (a locator still terminates
/// at `,`, `)` or `]`), because changing it would alter which citations
/// resolve. What changes is that a citation truncated by that rule now says so
/// instead of failing anonymously.

module;

export module planar.engine.ingest.materialize;

import std;
import planar.db;

namespace planar::engine::ingest::materialize {

/// @brief Stamped on every fact this materializer writes.
///
/// Part of the stored fact's identity: routing compares it when deciding
/// whether a fact was produced by a materializer it still understands.
export inline constexpr std::string_view materializer_version = "spec-ingest-v1";

/// @brief Materializer version stamped on facts an OPERATOR staged for a
/// single task, rather than ones ingest derived from a roadmap bullet.
///
/// Kept distinct from `materializer_version` so provenance stays legible in
/// the row and in `task packet --json`. `packet`'s freshness rule accepts
/// both versions; the digest comparison is untouched, so an operator-staged
/// fact still goes stale the moment its source text changes (decision 1102).
export inline constexpr std::string_view operator_materializer_version = "operator-v1";

// Semantic-source labels for the aggregate facts `stage_count` produces.
//
// `stage_count` stores `"<counted_kind>:<count>"` as a fact's semantic source
// and the routing packet RECOMPUTES that same string live to decide freshness.
// The two must agree byte for byte: when they drift, every affected fact is
// permanently stale and the task's packet never becomes ready — with no error
// anywhere, because each side is independently well-formed. They DID drift
// once (task 5762), which is why both sides read shared constants now.

/// @brief Counted fact kind behind the `breadth` aggregate.
export inline constexpr std::string_view counted_kind_touch = "touch";
/// @brief Counted fact kind behind the `validation_burden` aggregate.
export inline constexpr std::string_view counted_kind_scenario = "scenario";
/// @brief Counted fact kind behind the `dependency_fanout` aggregate.
///
/// Deliberately still `blocks`: it is a routing FACT KIND, a separate
/// vocabulary from the `entity_links.relationship` value the 00033 rename
/// moved. Changing it would invalidate every stored digest.
export inline constexpr std::string_view counted_kind_dependency = "blocks";

/// @brief Locator recorded on the `breadth` aggregate fact.
export inline constexpr std::string_view locator_touches = "links#touches";
/// @brief Locator recorded on the `validation_burden` aggregate fact.
export inline constexpr std::string_view locator_scenarios = "links#scenarios";
/// @brief Locator recorded on the `dependency_fanout` aggregate fact.
export inline constexpr std::string_view locator_dependency_fanout = "links#blocks-outgoing";

/// @brief One task's citation of a specific roadmap bullet.
///
/// Supplied by the caller (the diff carries the parsed provenance) rather than
/// re-derived here, so the staged `source_text` is exactly the canonical
/// folded bullet the parser produced.
export struct roadmap_citation {
  std::int64_t task_id_     = 0; ///< The citing task.
  std::int64_t artifact_id_ = 0; ///< The cited roadmap artifact.
  std::string  source_locator_;  ///< `roadmap#milestone:N/item:M`, 1-based.
  std::string  source_text_;     ///< The canonical folded bullet.
};

/// @brief Detail about one citation whose section could not be resolved.
///
/// Carries everything an operator needs to fix the citation without bisecting
/// their own edits: which task, which artifact, the locator as it was actually
/// parsed out of the task body, the section name that locator asked for, and
/// the sections the artifact really offers.
export struct citation_diagnostic {
  std::int64_t             task_id_     = 0; ///< The task whose citation failed.
  std::int64_t             artifact_id_ = 0; ///< The artifact it cited.
  std::string              locator_;         ///< The full locator parsed from the task body.
  std::string              wanted_;          ///< The section name the locator asked for.
  std::vector<std::string> available_;       ///< Section names the artifact actually has.
  /// @brief Set when an available section STARTS WITH `wanted_`.
  ///
  /// That is the signature of the truncation trap: the locator scan stops at
  /// the first `,`, `)` or `]`, so a heading containing any of them can never
  /// be cited in full and resolves to a prefix that names no section. Empty
  /// when no such prefix match exists.
  std::string truncated_from_;

  /// @brief Renders the whole failure as one operator-readable line.
  /// @return The rendered diagnostic.
  [[nodiscard]] auto describe() const -> std::string;
};

/// @brief What went wrong in `reconcile`.
export enum class materialize_error_kind : std::uint8_t {
  query_failed,     ///< A SQLite operation failed.
  invalid_citation, ///< At least one task cites a section its artifact does not have.
  task_not_found    ///< `stage_one_task` was given an id no task has.
};

/// @brief Failure surface for `reconcile`.
///
/// Deliberately a struct rather than the bare enum this codebase uses
/// elsewhere: `citations_` is the whole point (see this module's header). It
/// holds EVERY unresolvable citation found in the pass, not just the first, so
/// an operator with three bad citations fixes them in one run rather than
/// discovering them one ingest at a time.
export struct materialize_error {
  materialize_error_kind           kind_ = materialize_error_kind::query_failed; ///< What went wrong.
  std::vector<citation_diagnostic> citations_;                                   ///< Every unresolvable citation from the pass.

  /// @brief Renders the error, including one line per unresolvable citation.
  /// @return The rendered error.
  [[nodiscard]] auto describe() const -> std::string;
};

/// @brief Replaces the complete fact set for every task below `anchor_plan_id`.
///
/// Stages the full set, compares it against the stored set, and rewrites only
/// on a difference. The caller owns the transaction: this function opens none,
/// so a failure leaves whatever the caller's transaction decides.
/// @param conn An open connection to a migrated database.
/// @param anchor_plan_id The root plan whose subtree's facts are rebuilt.
/// @param roadmap_citations Parsed roadmap provenance, one per ingested task.
/// @return Success, or the failure — carrying every citation diagnostic when
/// the kind is `invalid_citation`.
export [[nodiscard]] auto reconcile(db::connection& conn, std::int64_t anchor_plan_id,
                                    std::span<const roadmap_citation> roadmap_citations)
    -> std::expected<void, materialize_error>;

/// @brief Replaces the complete fact set for ONE task, under operator
/// provenance.
///
/// The operator-facing counterpart to `reconcile`: that function rebuilds
/// every task below an anchor plan and is reachable only from `spec ingest
/// --apply`, which is why a hand-filed task could never obtain routing facts
/// and an edited task could never restage them (task 6048, decision 1102).
///
/// Facts are derived through the SAME predicates the plan-wide pass uses, and
/// stamped `operator_materializer_version`. Citation facts are staged only for
/// artifacts the task already cites through an `entity_links` edge AND
/// references explicitly in its body; an artifact it cites without a resolvable
/// locator is reported as a `citation_diagnostic`, never fabricated.
///
/// Unlike `reconcile`, this function OWNS its transaction (immediate), and its
/// delete is scoped to `task_id` — staging one task must never discard a
/// sibling's ingest-materialized facts.
///
/// @param conn An open connection to a migrated database.
/// @param task_id The task whose facts are rebuilt.
/// @return Success, or the failure — `task_not_found` for an unknown id, or
/// `invalid_citation` carrying every unresolvable citation from the pass.
export [[nodiscard]] auto stage_one_task(db::connection& conn, std::int64_t task_id) -> std::expected<void, materialize_error>;

/// @brief Computes a fact's `source_digest`.
///
/// SHA-256 over `"<kind>\0<id>\0<locator>\0<semantic source>"`, lowercase hex.
/// The exact canonical form is a stored contract — every fact already in a
/// database was digested this way, so changing the separator or the field
/// order would invalidate all of them at once and silently mark every fact
/// stale.
/// @param source_kind The source entity's kind.
/// @param source_id The source entity's id.
/// @param locator The locator within that entity.
/// @param semantic_source The semantic content the fact was derived from.
/// @return The 64-character lowercase hex digest.
export [[nodiscard]] auto source_digest(std::string_view source_kind, std::int64_t source_id, std::string_view locator,
                                        std::string_view semantic_source) -> std::string;

/// @brief `sha256(input)` as 64 lowercase hex characters (FIPS 180-4).
///
/// Exported at task 6324 for `packet.cpp`, which digests whole evidence texts
/// and canonical bodies rather than the four-field preimage `source_digest`
/// builds. Both modules live in this bucket, so sharing the implementation
/// costs no new dependency and avoids a second copy.
///
/// NOTE, CORRECTED AT TASK 6407. This comment used to say that
/// `engine/external/sha256.hpp` and `engine/workbench/manifest.cpp` each
/// carried their own, making four. Measured by the FIPS round constants, the
/// real count was FIVE: those two, this one, `engine/planning/annotation.cpp`
/// (which the note missed), and the layer-1 module `planar.sha256`.
///
/// ONE implementation remains in the tree: layer-1 `planar.sha256`, which
/// this is now a thin wrapper over (task 6759, finishing what 6407 began).
/// There were five, counted by their FIPS 180-4 round constants. Each was
/// verified to agree as an ORDERED constant sequence -- a sorted-set
/// comparison cannot see a misordering, which is exactly the bug task 6106
/// fixed -- and each one's tests were checked for vectors the module lacked
/// before it was deleted, so no stored digest changed and no coverage was
/// lost on the way.
/// @param input The bytes to digest.
/// @return The 64-character lowercase hex digest.
export [[nodiscard]] auto sha256_hex(std::string_view input) -> std::string;

/// @brief Strips the synthetic wrapper `planar artifact show` puts around a
/// stored body, returning the authored document.
///
/// A stored artifact begins with a `## Content` heading followed by a YAML
/// frontmatter block. The workbench source parsed at ingestion has neither, so
/// parsing the stored body directly would count `## Content` as a milestone and
/// shift every `milestone:N` locator by one — every roadmap citation would then
/// resolve to the wrong item, or to nothing.
///
/// Only a wrapper at the VERY START is removed: a section legitimately named
/// "Content" further down is authored material and is left alone, and an
/// authored milestone with no work items keeps its index.
/// @param body The stored artifact body.
/// @return A view INTO `body` (which must outlive the result).
export [[nodiscard]] auto unwrap_stored_artifact_body(std::string_view body) -> std::string_view;

/// @brief Resolves a `roadmap#milestone:N/item:M` locator to its canonical
/// folded bullet — the same string ingestion staged as `source_text`.
///
/// Indices are 1-based, matching the locators the differ emits. Returns
/// `nullopt` when the milestone or the item does not exist, which is what makes
/// a citation to a DELETED roadmap item go stale rather than silently resolve
/// to a neighbour and stay fresh forever.
/// @param body The stored artifact body (wrapped or not).
/// @param locator The locator to resolve.
/// @return The bullet text, or `nullopt`.
export [[nodiscard]] auto roadmap_section(std::string_view body, std::string_view locator) -> std::optional<std::string>;

/// @brief Finds the roadmap bullet carrying `[slug: <slug>]`.
/// @param body The roadmap body.
/// @param slug The slug to match, compared after trimming.
/// @return A view INTO `body` (which must outlive the result), or `nullopt`.
export [[nodiscard]] auto roadmap_bullet_for_slug(std::string_view body, std::string_view slug)
    -> std::optional<std::string_view>;

/// @brief Resolves an `<anything>#<section name>` locator against a document.
///
/// Heading matching is case-insensitive and honors nesting: the section runs
/// until the next heading at the SAME OR SHALLOWER level, so a `### Nested`
/// under a cited `## Target` stays part of the section. Headings inside fenced
/// or indented code blocks are ignored entirely, so a decoy heading in an
/// example block neither opens nor closes a section.
/// @param body The document to search.
/// @param locator The locator whose `#` fragment names the section.
/// @return A view INTO `body` (which must outlive the result), or `nullopt`
/// when the document has no such section.
export [[nodiscard]] auto artifact_section(std::string_view body, std::string_view locator) -> std::optional<std::string_view>;

/// @brief Returns the text under an exact `## <heading>` line, up to the next H2.
/// @param body The document to search.
/// @param heading The full heading text to find (e.g. `## Acceptance Criteria`).
/// @return A view INTO `body`, empty when the heading is absent.
export [[nodiscard]] auto section(std::string_view body, std::string_view heading) -> std::string_view;

/// @brief Returns the rest of the line following `marker`.
/// @param body The document to search.
/// @param marker The literal marker (e.g. `**Acceptance:**`).
/// @return A view INTO `body`, or `nullopt` when absent or empty.
export [[nodiscard]] auto field(std::string_view body, std::string_view marker) -> std::optional<std::string_view>;

} // namespace planar::engine::ingest::materialize
