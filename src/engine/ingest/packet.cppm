/// @file packet.cppm
/// @brief `planar.engine.ingest.packet` — the canonical, fail-closed routing
/// packet behind `planar task packet <id>` (plan 996, task 6324).
///
/// Port target: the TASK half of `zig/src/engine/routing/packet.zig` (1674
/// lines) plus its handler `zig/src/cmd/planar/handlers/task/packet.zig` (41
/// lines). The oracle compiles FACTS, not routing choices: it reads current
/// Planar rows and links into a typed input, orders unordered evidence,
/// checks source freshness and completeness, and hashes only semantic fields.
///
/// ## WHY THIS LIVES IN `engine/ingest` AND NOT AN `engine/routing` BUCKET
///
/// A layer-2 `engine_*` may not depend on another layer-2 `engine_*`, and the
/// packet shares the `materialize` module's digest and Markdown-section
/// helpers: `source_digest` and `sha256_hex` digest a reference, and
/// `artifact_section` resolves a `#heading` fragment for `--expand`. A
/// separate bucket would FATAL at configure time.
///
/// ## REFERENCES, NOT FACTS
///
/// The packet carries no `routing_task_facts` row and no section text. A
/// citation is a REFERENCE derived live from the task's `cites` entity links:
/// the cited artifact's id, a locator, a digest and a one-line label. The
/// text of a cited section is fetched on demand with `expand_reference`
/// (`planar task packet <id> --expand <ref>`). Nothing reads
/// `routing_task_facts` on master any more; the table is dropped by a later
/// migration.
///
/// ## THE REASON LIST IS CONTRACT, AND ITS ORDER IS SOURCE ORDER
///
/// `task packet <id> --json` is what the orchestrator reads to decide whether
/// a task is dispatchable; `ready: false` is a documented STOP. Packets only
/// compare within one `policy_version`, which is why it rides in the JSON
/// ENVELOPE and not only inside the canonical body — a consumer has to know
/// two packets were built under the same rules BEFORE comparing their digests,
/// and digging the version out of the canonical body would mean parsing the
/// very thing whose format the version describes.
///
/// **THE ORDER IS THE ORDER OF THE `append_reason` CALLS IN `compile_task`,
/// DEDUPLICATED, FIRST OCCURRENCE WINS.** It is NOT sorted and must not be.
/// Measured on a bare task that owns an active plan:
///
///     missing_body, missing_acceptance_section, generic_acceptance,
///     generic_next_action, missing_product_spec, missing_tech_spec,
///     missing_roadmap, missing_test_spec, missing_locked_decision,
///     missing_touch, absent_validation_gates,
///     uncovered_required_scenario
///
/// **NO PART OF THE ORDER COMES FROM HASH-MAP ITERATION.** This was checked
/// explicitly, because task 6274 found exactly that in the routing table's
/// `languages`. The oracle accumulates reasons into a `std::ArrayList` in
/// straight-line source order, and every evidence list it reads is an SQL
/// result with an explicit `order by`. The order is fully reproducible and is
/// pinned as such — there is no divergence to record here.
///
/// **`validate_evidence_classes` RUNS LAST, BUT DEDUPLICATION CAN PULL ITS
/// REASONS FORWARD.** A reason reachable from both the main block and the
/// class walk lands at its EARLY position.
///
/// ## ORACLE FACTS THAT COST A CYCLE IF ASSUMED
///
/// **ROOT TASKS ARE READY WITHOUT A `depends-on` EDGE.** An empty dependency
/// list means the task is a root task, so it does not produce
/// `missing_dependency`. A declared edge whose dependency is unfinished still
/// produces `invalid_dependency`; this exemption affects only the empty list.
///
/// **A CLAIM ROW IS ONLY EVIDENCE WHILE ITS LEASE IS LIVE.** `claim_evidence`
/// selects `status = 'active'` rows and then re-derives the status as
/// `active` / `expired` from `lease_expires_at` against `now`. An active claim
/// whose lease has passed reports `inactive_claim` and blocks the packet; a
/// live one leaves it ready. Both arms measured.
///
/// **THE DIGEST IS COMPUTED OVER THE SEMANTIC CANONICAL FORM, THE `canonical`
/// FIELD OVER THE DISPLAY FORM.** They differ by exactly the `display_label`
/// key. Renaming a plan therefore changes the `canonical` bytes and leaves the
/// digest alone, which is what lets the digest certify "same packet" across a
/// cosmetic edit.
///
/// READ-ONLY: this module only SELECTs. No writes, no migration.
module;

export module planar.engine.ingest.packet;

import std;
import planar.db;

namespace planar::engine::ingest::packet {

/// @brief The rule-set version every packet is stamped with.
///
/// Packets are comparable only within one version. It rides in the JSON
/// envelope as `policy_version` and inside the canonical body as `policy`.
export inline constexpr std::string_view policy_version = "routing-packet-v2";

/// @brief One piece of evidence the packet compiled, with its provenance and
/// freshness.
///
/// Defaults match the oracle's struct defaults exactly: a bare `evidence` is
/// required, covered, `ready` and `current`, so a loader that sets only the
/// identity fields produces the oracle's own shape.
export struct evidence {
  std::string  kind;                         ///< Entity or fact kind, e.g. `product_spec`, `claim`.
  std::int64_t id = 0;                       ///< Source row id.
  std::string  locator;                      ///< `artifact:12#Overview`, `next_action`, a path, …
  std::string  text;                         ///< The semantic payload the digest is taken over.
  std::string  display_label;                ///< Human label; in `canonical`, never in the digest.
  std::string  source_digest;                ///< Digest recorded when the evidence was materialized.
  std::string  current_digest;               ///< Digest recomputed live; empty when unresolvable.
  bool         required   = true;            ///< Whether readiness depends on this row.
  bool         covered    = true;            ///< Scenario/coverage membership.
  std::string  status     = "ready";         ///< Lifecycle status, checked per evidence class.
  std::string  provenance = "planar";        ///< Where the row came from, e.g. `artifact:12`.
  std::string  materializer_version;         ///< Version stored with the fact.
  std::string  current_materializer_version; ///< Version the running binary would write.
  std::string  freshness = "current";        ///< `current` or `stale`.
};

/// @brief One citation of an artifact, derived from a `cites` entity link.
///
/// A reference names a cited artifact and carries no text from it; the text
/// comes from `expand_reference`. Defaults mirror `evidence`.
export struct reference {
  std::string  ref;               ///< `artifact:<id>`, the argument `--expand` takes.
  std::string  kind;              ///< The cited artifact's kind, e.g. `tech_spec`.
  std::int64_t id      = 0;       ///< The cited artifact's id.
  std::string  locator = "body";  ///< `body` for the whole artifact; the entity link carries no finer locator.
  std::string  digest;            ///< `source_digest("artifact", id, locator, body)`, 64 lowercase hex chars.
  std::string  label;             ///< One-line label: the artifact's title. In `canonical`, never in the digest.
  std::string  status = "active"; ///< The artifact's lifecycle status.
};

/// @brief Everything the compiler reads about one task.
///
/// Field order is load-bearing: it is the key order of both the JSON `input`
/// object and the canonical body.
export struct task_input {
  std::int64_t           task_id = 0;         ///< `tasks.id`.
  std::string            status;              ///< `tasks.status`; only `todo`/`doing` are dispatchable.
  std::string            title;               ///< `tasks.title`.
  std::string            body;                ///< `tasks.body`, coalesced to empty.
  std::string            next_action;         ///< `tasks.next_action`, coalesced to empty.
  std::string            acceptance_criteria; ///< The `## Acceptance Criteria` section, empty when absent.
  std::vector<evidence>  owning_plans;        ///< The plan the task belongs to; 0 or 1 row.
  std::vector<evidence>  anchor_plans;        ///< Its parent (or derives-from target, or itself); 0 or 1 row.
  std::vector<reference> references;          ///< The artifacts the task `cites`, ordered by kind then id.
  std::vector<evidence>  decisions;           ///< Decisions the task `cites`.
  std::vector<evidence>  questions;           ///< Questions the task `addresses`.
  std::vector<evidence>  scenarios;           ///< Scenarios that `verifies` the task, with `covered` set separately.
  std::vector<evidence>  dependencies;        ///< Tasks this one `depends-on`; `done` reads as `satisfied`.
  std::vector<evidence>  touches;             ///< Declared paths first, then whole-repo edges.
  std::vector<evidence>  claims;              ///< Active claims, with a lapsed lease reported as `expired`.
  std::vector<evidence>  validation_gates;    ///< The `## Required validation` section; 0 or 1 row.
};

/// @brief Why a packet is not ready.
///
/// Enumerator order is the oracle's declaration order and is NOT the emission
/// order — see this file's header for the emission order, which comes from the
/// sequence of checks in `compile_task`. The fact and evidence reasons
/// (`missing_acceptance_fact`, `missing_next_action_fact`,
/// `invalid_mandatory_fact`, `stale_fact`, `contradictory_mandatory_fact`,
/// `missing_provenance`, `stale_mandatory_evidence`) are no longer emitted:
/// the packet carries no facts.
export enum class readiness_reason : std::uint8_t {
  missing_title,
  missing_body,
  invalid_task_status,
  missing_acceptance_section,
  generic_acceptance,
  generic_next_action,
  missing_acceptance_fact,
  missing_next_action_fact,
  invalid_mandatory_fact,
  missing_owning_plan,
  missing_anchor_plan,
  invalid_owning_plan,
  invalid_anchor_plan,
  missing_product_spec,
  missing_tech_spec,
  missing_roadmap,
  missing_test_spec,
  missing_locked_decision,
  missing_dependency,
  missing_touch,
  absent_validation_gates,
  uncovered_required_scenario,
  stale_fact,
  contradictory_mandatory_fact,
  unresolved_citation,
  invalid_locked_decision,
  unresolved_question,
  invalid_dependency,
  invalid_touch,
  inactive_claim,
  invalid_validation_gate,
  missing_provenance,
  stale_mandatory_evidence,
};

/// @brief The wire spelling of a reason, matching the oracle's `@tagName`.
/// @param reason The reason.
/// @return Its snake_case name, e.g. `"missing_dependency"`.
export [[nodiscard]] auto reason_name(readiness_reason reason) -> std::string_view;

/// @brief A compiled packet: the input it was built from, both canonical
/// renderings' outcome, and the readiness verdict.
export struct task_packet {
  task_input                    input;     ///< The evidence the packet was compiled from.
  std::string                   canonical; ///< Canonical body INCLUDING `display_label`.
  std::string                   digest;    ///< 64 lowercase hex chars over the SEMANTIC body.
  std::vector<readiness_reason> reasons;   ///< Emission-ordered, deduplicated.

  /// @brief Whether the task is dispatchable.
  /// @return True iff there are no reasons.
  [[nodiscard]] auto ready() const -> bool {
    return reasons.empty();
  }
};

/// @brief Why assembly failed.
export enum class packet_error : std::uint8_t {
  task_not_found, ///< No `tasks` row with that id (`assemble_task`).
  query_failed,   ///< SQL failure.
};

/// @brief Compile a packet from a caller-supplied input.
///
/// Pure: no database, no clock. Exported so the readiness rules can be tested
/// on inputs a fixture cannot reach through SQL.
/// @param input The evidence to compile.
/// @return The packet, always — an unready input is a packet with reasons,
/// never an error.
export [[nodiscard]] auto compile_task(const task_input& input) -> task_packet;

/// @brief Reassemble the packet from the current database snapshot.
/// @param conn An open database connection.
/// @param task_id The task to compile.
/// @return The packet, or `task_not_found` / `query_failed`.
export [[nodiscard]] auto assemble_task(db::connection& conn, std::int64_t task_id) -> std::expected<task_packet, packet_error>;

/// @brief Why `expand_reference` failed.
export enum class expand_error : std::uint8_t {
  task_not_found,    ///< No `tasks` row with that id.
  unknown_reference, ///< The reference is malformed or is not among the task's `cites` links.
  section_not_found, ///< The artifact is cited but has no section with the requested heading.
  query_failed,      ///< SQL failure.
};

/// @brief The text a reference resolves to.
export struct expansion {
  std::string ref;    ///< The reference as the caller wrote it.
  std::string digest; ///< `source_digest("artifact", id, locator, text)`; equals the reference's digest for a whole artifact.
  std::string text;   ///< The whole artifact body, or the section under the requested heading.
};

/// @brief Resolve one of the task's references to its text.
///
/// `ref` is `artifact:<id>` for the whole body, or `artifact:<id>#<heading>`
/// for the section under that heading (matched case-insensitively, nested
/// subsections included). The artifact must be one of the task's `cites`
/// links; anything else is `unknown_reference`, so a caller cannot read an
/// arbitrary artifact through a task it does not cite.
/// @param conn An open database connection.
/// @param task_id The task whose references are searched.
/// @param ref The reference to resolve.
/// @return The expansion, or the failure.
export [[nodiscard]] auto expand_reference(db::connection& conn, std::int64_t task_id, std::string_view ref)
    -> std::expected<expansion, expand_error>;

/// @brief Render an expansion as text.
/// @param value A resolved expansion.
/// @return The text, with a trailing newline added when it lacks one.
export [[nodiscard]] auto render_expansion_text(const expansion& value) -> std::string;

/// @brief Render an expansion as `{"ref":...,"digest":...,"text":...}`.
/// @param value A resolved expansion.
/// @return A COMPLETE stdout payload including its trailing newline.
export [[nodiscard]] auto render_expansion_json(const expansion& value) -> std::string;

/// @brief Render the operator-facing text form.
/// @param packet A compiled packet.
/// @return A COMPLETE stdout payload including its trailing newline.
export [[nodiscard]] auto render_text(const task_packet& packet) -> std::string;

/// @brief Render the machine-readable envelope.
///
/// The envelope carries `policy_version` alongside `ready`, `input`, `digest`
/// and `reasons`. It carries no `canonical` body: `input` is the only
/// serialized copy of the evidence, so the packet is not emitted twice.
/// `input.references` lists `{ref, kind, id, locator, digest, label, status}`
/// per cited artifact and carries no text.
/// @param packet A compiled packet.
/// @return A COMPLETE stdout payload including its trailing newline.
export [[nodiscard]] auto render_json(const task_packet& packet) -> std::string;

} // namespace planar::engine::ingest::packet
