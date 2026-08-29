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
/// The Zig original sits in `engine/routing/`, so a mechanical port would
/// create a matching `engine_routing` bucket. That bucket CANNOT BE BUILT.
/// D15/D18 forbid a layer-2 `engine_*` from depending on another layer-2
/// `engine_*`, and the packet's freshness computation is defined in terms of
/// `materialize`'s digests: `citation_evidence` and `fact_semantic_source`
/// both call `artifact_section`, `roadmap_section`, `section`, `field` and
/// `source_digest`, and compare against `materializer_version`. A
/// `DEPENDS engine_ingest` line on an `engine_routing` bucket would FATAL at
/// configure time, not silently succeed.
///
/// D19's remedy — extract the shared primitive to layer 1 — was measured and
/// rejected: `roadmap_section` calls `parse::parse_roadmap`, so extracting the
/// helpers drags `planar.engine.ingest.parse` (the whole markdown-parsing half
/// of ingest) down a layer to serve one caller. That is the layering inverted,
/// which is the same argument `engine/ingest/CMakeLists.txt` already makes
/// about `apply.zig`.
///
/// Co-location is also the honest description of the coupling. `materialize`
/// WRITES the `routing_task_facts` rows; this module READS them back and
/// RECOMPUTES the same digest live to decide whether the fact is still
/// current. The oracle says so itself at `zig/src/engine/ingestor/
/// materialize.zig:489` — the two halves must agree byte for byte or every
/// fact is permanently stale. Splitting them across buckets would put a
/// configure-time wall between two functions that are one contract.
///
/// The PLANNING half of `packet.zig` (`assemble_planning`, `compile_planning`,
/// `resolve_invocation`, `coder_brief`) is deliberately NOT ported here. It
/// serves `models resolve`, which `engine/models/CMakeLists.txt` already
/// defers, and it depends on `test_spec_status` — an `engine_planning` module,
/// so it faces this same wall from the other side. Whoever takes `models
/// resolve` inherits that decision; nothing here prejudges it.
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
///     missing_dependency, missing_touch, absent_validation_gates,
///     missing_acceptance_fact, missing_next_action_fact,
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
/// REASONS FORWARD.** `unresolved_citation` and `uncovered_required_scenario`
/// are each reachable from both the main block and the class walk. Measured on
/// a READY task whose product-spec artifact body was then edited:
/// `[missing_product_spec, unresolved_citation, stale_fact,
/// stale_mandatory_evidence]` — `unresolved_citation` lands at its EARLY
/// position (the citations loop), while `stale_mandatory_evidence`, reachable
/// only from the class walk, lands at the end.
///
/// ## ORACLE FACTS THAT COST A CYCLE IF ASSUMED
///
/// **`missing_dependency` FIRES ON A ROOT TASK.** A task with no `depends-on`
/// edge is `not_ready` with `missing_dependency` among its reasons, at exit 0,
/// with no exemption and no error. Planar task 6048 is an OPEN QUESTION
/// arguing that this is wrong. This port reproduces the MEASURED behaviour and
/// leaves 6048 to be decided on its own terms; do not "fix" it here.
///
/// **`contradictory_mandatory_fact` IS UNREACHABLE FROM `assemble_task`.** The
/// check looks for two required facts sharing `kind`, `id` AND `locator` with
/// differing `text`, but `fact_evidence` sets `id` from
/// `routing_task_facts.id`, which is a primary key. Two rows can never share
/// it. The check is live for a hand-built `task_input` and dead for every
/// packet the CLI can produce. Reproduced rather than removed — it is the
/// oracle's code and a future assembler could reach it.
///
/// **THE `blocks` / `blocked_by` FACT KINDS RESOLVE AGAINST THE
/// `depends-on` RELATIONSHIP.** Migration 00033 renamed the entity_links
/// relationship; the routing FACT KIND vocabulary did not move. So
/// `fact_semantic_source` matches `fact_kind == "blocks"` and queries
/// `relationship = 'depends-on'`, and `materialize::counted_kind_dependency`
/// stays the string `"blocks"` while the live count queries `depends-on`.
/// Both halves are the oracle's and both are reproduced verbatim.
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
export inline constexpr std::string_view policy_version = "routing-packet-v1";

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

/// @brief Everything the compiler reads about one task.
///
/// Field order is load-bearing: it is the key order of both the JSON `input`
/// object and the canonical body.
export struct task_input {
  std::int64_t          task_id = 0;         ///< `tasks.id`.
  std::string           status;              ///< `tasks.status`; only `todo`/`doing` are dispatchable.
  std::string           title;               ///< `tasks.title`.
  std::string           body;                ///< `tasks.body`, coalesced to empty.
  std::string           next_action;         ///< `tasks.next_action`, coalesced to empty.
  std::string           acceptance_criteria; ///< The `## Acceptance Criteria` section, empty when absent.
  std::vector<evidence> owning_plans;        ///< The plan the task belongs to; 0 or 1 row.
  std::vector<evidence> anchor_plans;        ///< Its parent (or derives-from target, or itself); 0 or 1 row.
  std::vector<evidence> citations;           ///< Cited artifact SECTIONS, joined through the routing facts.
  std::vector<evidence> decisions;           ///< Decisions the task `cites`.
  std::vector<evidence> questions;           ///< Questions the task `addresses`.
  std::vector<evidence> scenarios;           ///< Scenarios that `verifies` the task, with `covered` set separately.
  std::vector<evidence> dependencies;        ///< Tasks this one `depends-on`; `done` reads as `satisfied`.
  std::vector<evidence> touches;             ///< Declared paths first, then whole-repo edges.
  std::vector<evidence> claims;              ///< Active claims, with a lapsed lease reported as `expired`.
  std::vector<evidence> validation_gates;    ///< The `## Required validation` section; 0 or 1 row.
  std::vector<evidence> facts;               ///< Every `routing_task_facts` row, with freshness recomputed live.
};

/// @brief Why a packet is not ready.
///
/// Enumerator order is the oracle's declaration order and is NOT the emission
/// order — see this file's header for the emission order, which comes from the
/// sequence of checks in `compile_task`.
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
  task_not_found, ///< No `tasks` row with that id.
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

/// @brief Render the operator-facing text form.
/// @param packet A compiled packet.
/// @return A COMPLETE stdout payload including its trailing newline.
export [[nodiscard]] auto render_text(const task_packet& packet) -> std::string;

/// @brief Render the machine-readable envelope.
///
/// The envelope carries `policy_version` alongside `ready`, `input`,
/// `canonical`, `digest` and `reasons` — see this file's header for why the
/// version is duplicated out of the canonical body.
/// @param packet A compiled packet.
/// @return A COMPLETE stdout payload including its trailing newline.
export [[nodiscard]] auto render_json(const task_packet& packet) -> std::string;

} // namespace planar::engine::ingest::packet
