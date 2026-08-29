/// @file profile.cppm
/// @brief `planar.engine.models.profile` — project a READY task packet into an
/// explainable routing profile (plan 996, task 6111).
///
/// Behavior-preserving port (D2) of `zig/src/engine/routing/profile.zig`
/// (726 lines, of which 507 are implementation and 219 are colocated Zig
/// `test` blocks that this port replaces with its own Catch2 fixtures).
///
/// The projection is deterministic and citation-bearing: every classification
/// names the rule that produced it, the rule's version, and every fact it
/// matched, so a preview can explain itself and a stored dispatch can be
/// replayed against the same inputs.
///
/// Three outcomes, and the difference between the last two is load-bearing:
///
///   - `profile`          — the packet classified.
///   - `not_ready`        — the TASK lacks required facts, or its facts
///                          contradict each other. Never falls through to
///                          `feature`: silently defaulting a task whose
///                          evidence is broken is how a misclassification
///                          becomes invisible.
///   - `policy_not_ready` — the POLICY lacks a threshold this packet needs.
///                          The task is fine; the operator has not configured
///                          the boundary. Distinguishing this from `not_ready`
///                          tells the operator which of the two to fix.
///
/// ## WHY THIS LIVES IN `engine/models` AND NOT AN `engine/routing` BUCKET
///
/// The Zig original sits in `engine/routing/` beside `packet.zig`, so the
/// mechanical port would put it next to the packet — which in this tree lives
/// in `engine/ingest` (see `engine/ingest/packet.cppm`'s header for why it
/// landed there). That placement CANNOT BE BUILT here.
///
/// This module's whole vocabulary is the routing taxonomy: `tier` (from
/// `registry.cppm`), `work_type` and `complexity` (from `ranking.cppm`). All
/// three already live in `engine_models`. Putting the classifier in
/// `engine_ingest` instead would need a `DEPENDS engine_models` line, and
/// D15/D18 forbid a layer-2 `engine_*` from depending on another layer-2
/// `engine_*` — that FATALs at configure time, it does not silently succeed.
///
/// D19's remedy — extract the shared taxonomy to a new layer-1 module — was
/// considered and REJECTED as the more expensive of two legal options. It
/// would move three enums plus their text conversions out from under
/// `engine_models`' existing tests, rewire every current consumer, and add a
/// library, all to serve a dependency that the alternative removes outright.
///
/// The alternative, taken here: **this module names its own input type.**
/// `compile` consumes a `std::span<const fact>` — a six-field view over the
/// packet's evidence — rather than `engine_ingest`'s `evidence` struct. The
/// six fields are exactly what the Zig original reads (`find_fact` reads
/// `kind` and `text`; `to_matched` additionally reads `locator`, `provenance`,
/// `id`, and `current_digest`), so this is the classifier's true input surface
/// rather than a convenience narrowing. The layer-3 caller
/// (`src/cmd/planar/handlers/models.cpp`) adapts one to the other, which is
/// where the two buckets already meet anyway: the handler is the only place
/// that holds both a packet and a routing decision.
///
/// Consequence worth stating plainly: **this module has NO dependency on the
/// packet at all** and is testable without a database. That is a property of
/// the seam, not an accident of it.
///
/// ## PRECEDENCE IS THE TABLE'S ORDER, NOT THE ENUM'S
///
/// `work_type_rules` is evaluated top to bottom and the FIRST match wins:
/// schema, architectural, engine, cli, mechanical, then `feature` as the
/// fall-through. `enum class work_type`'s declaration order is DIFFERENT
/// (schema, engine, architectural, cli, feature, mechanical) and must not be
/// used to derive precedence. A task touching a migration AND a CLI flag is
/// `schema`, because the cohort must reflect the riskiest dimension of the
/// work rather than the most numerous.
///
/// ## `acceptance_complete` IS CHECKED TWICE, DIFFERENTLY
///
/// Step 2 requires the fact to be PRESENT (`find_fact != nullptr`) and
/// refuses with `missing_required_fact` when it is absent. Step 5 asks
/// whether it is TRUE (`truthy_fact`). A fact recorded as `"false"` therefore
/// passes step 2 and fails step 5 — the packet is classifiable, the work is
/// simply not complete. Collapsing the two into one check changes the tier
/// floor of every task carrying a false `acceptance_complete`.
module;

export module planar.engine.models.profile;

import std;
import planar.engine.models.registry;
import planar.engine.models.ranking;

namespace planar::engine::models::profile {

using ranking::complexity;
using ranking::work_type;
using registry::tier;

/// @brief The version stamped on every profile this module compiles.
///
/// Adding a rule input means adding it to `fact_kinds` AND bumping this.
export inline constexpr std::string_view rule_version = "routing-profile-v1";

/// @brief Named fact kinds the rules consume.
///
/// These are the contract between the ingest materializer (which writes
/// `routing_task_facts`) and this classifier. A fact kind absent from this
/// list is ignored by every rule.
export namespace fact_kinds {
// --- schema: any of these classifies the work as `schema` ------------------
/// A migration file is touched.
inline constexpr std::string_view migration_touched = "schema.migration_touched";
/// The `schema_migrations` version contract changes.
inline constexpr std::string_view schema_version_contract = "schema.version_contract_change";
/// Constraints or indexes are redesigned.
inline constexpr std::string_view constraint_or_index_redesign = "schema.constraint_or_index_redesign";

// --- architectural ---------------------------------------------------------
/// A new subsystem is introduced.
inline constexpr std::string_view new_subsystem = "architectural.new_subsystem";
/// A new binary is introduced.
inline constexpr std::string_view new_binary = "architectural.new_binary";
/// Declared by the oracle and consumed by NO rule.
///
/// Retained so this list stays comparable with the Zig original rather than
/// silently diverging from it. A task carrying only this fact classifies as
/// `feature`; `models.profile: module_breadth is declared but consumed by no
/// rule` pins that.
inline constexpr std::string_view module_breadth = "architectural.module_breadth";

// --- engine ----------------------------------------------------------------
/// Transaction boundaries change.
inline constexpr std::string_view transaction_change = "engine.transaction_change";
/// Concurrency behavior changes.
inline constexpr std::string_view concurrency_change = "engine.concurrency_change";
/// Ownership or lifetime of a resource changes.
inline constexpr std::string_view ownership_change = "engine.ownership_change";
/// Security-relevant behavior changes.
inline constexpr std::string_view security_change = "engine.security_change";
/// Resource acquisition or release changes.
inline constexpr std::string_view resource_lifecycle_change = "engine.resource_lifecycle_change";
/// A status transition rule changes.
inline constexpr std::string_view status_transition_change = "engine.status_transition_change";
/// Scope resolution changes.
inline constexpr std::string_view scope_resolution_change = "engine.scope_resolution_change";
/// A binary's capability boundary changes.
inline constexpr std::string_view capability_boundary_change = "engine.capability_boundary_change";

// --- cli -------------------------------------------------------------------
/// The command surface changes.
inline constexpr std::string_view cli_surface_change = "cli.surface_change";

// --- mechanical ------------------------------------------------------------
/// Documentation only.
inline constexpr std::string_view docs_only = "mechanical.docs_only";
/// Renames or formatting only.
inline constexpr std::string_view rename_or_format_only = "mechanical.rename_or_format_only";

// --- capacity and judgement ------------------------------------------------
// Complexity and tier inputs. NOT work-type inputs: no rule in
// `work_type_rules` names any of these.
/// How many units the work touches; read as an integer.
inline constexpr std::string_view touched_unit_count = "capacity.touched_unit_count";
/// How many validation gates the work must satisfy; read as an integer.
inline constexpr std::string_view validation_gate_count = "capacity.validation_gate_count";
/// The operator declared the work risky; short-circuits every capacity metric.
inline constexpr std::string_view explicit_risk = "risk.explicit";
/// Whether the acceptance criteria are complete. Its PRESENCE is required to
/// classify at all; its TRUTH separately governs the tier floor.
inline constexpr std::string_view acceptance_complete = "acceptance_complete";
/// Correctness is observable from outside the change.
inline constexpr std::string_view observable_correctness = "acceptance.observable_correctness";
/// Acceptance requires high judgement to evaluate.
inline constexpr std::string_view high_judgment_acceptance = "acceptance.high_judgment";
} // namespace fact_kinds

/// @brief This module's input view over one piece of packet evidence.
///
/// Deliberately NOT `engine_ingest`'s `evidence` — see this module's header
/// for why the classifier names its own input type. The layer-3 caller
/// adapts. Field names match the oracle's `packet.Evidence` so the mapping is
/// one-to-one and greppable.
export struct fact {
  std::string_view kind;           ///< The fact kind; matched against `fact_kinds`.
  std::string_view locator;        ///< Where the fact came from, for citations.
  std::string_view text;           ///< The asserted value; `"false"`/`"0"` mean NOT asserted.
  std::string_view provenance;     ///< Source entity kind, carried into `matched_fact`.
  std::string_view current_digest; ///< Live digest, carried into `matched_fact`.
  std::int64_t     id = 0;         ///< Source entity id, carried into `matched_fact`.
};

/// @brief A fact as matched by a rule, carrying the lineage needed to justify it.
export struct matched_fact {
  std::string_view kind;                 ///< The matched fact's kind.
  std::string_view locator;              ///< Where the fact came from.
  std::string_view source_entity_kind;   ///< From the fact's `provenance`.
  std::int64_t     source_entity_id = 0; ///< From the fact's `id`.
  std::string_view source_digest;        ///< From the fact's `current_digest`.
  std::string_view text;                 ///< The asserted value.
};

/// @brief Where a classification came from, for the explanation surface.
export struct citation {
  std::string_view rule_id;   ///< The rule that produced the classification.
  std::string_view fact_kind; ///< The fact kind that matched it.
  std::string_view locator;   ///< That fact's locator.
};

/// @brief One work-type rule.
///
/// `any_of` matches when ANY listed fact kind is present AND asserts true.
/// Rules are evaluated in `work_type_rules` order and the FIRST match wins.
export struct work_type_rule {
  std::string_view                  id;     ///< The rule's versioned id.
  work_type                         type;   ///< What it classifies the work as.
  std::span<const std::string_view> any_of; ///< Fact kinds that trigger it.
};

/// @brief The fall-through rule id when no `work_type_rule` matches.
///
/// Reached only for a packet whose required facts are present and consistent —
/// a task with broken evidence returns `not_ready` instead.
export inline constexpr std::string_view default_rule_id = "work-type.feature.v1";

/// @brief The work-type rules in PRECEDENCE order. First match wins.
/// @return The rule table.
export [[nodiscard]] auto work_type_rules() -> std::span<const work_type_rule>;

/// @brief Fact-kind pairs that are mutually exclusive.
///
/// Asserting both is a contradiction in the task's own evidence, not a
/// precedence question.
/// @return The contradiction table.
export [[nodiscard]] auto contradictions() -> std::span<const std::array<std::string_view, 2>>;

/// @brief A named, versioned complexity boundary.
///
/// Absence is meaningful: a missing `bounded_max` prevents the `bounded`
/// classification (the packet cannot be shown to be small enough), and a
/// missing `large_min` for a metric the packet actually populates yields
/// `policy_not_ready` (the packet has a capacity reading the policy cannot
/// judge).
export struct threshold {
  std::string_view            id;          ///< The threshold's versioned id.
  std::string_view            metric;      ///< The capacity fact kind it covers.
  std::optional<std::int64_t> bounded_max; ///< At or under this, `bounded` stays possible.
  std::optional<std::int64_t> large_min;   ///< At or above this, the work is `high_risk`.
};

/// @brief The configured complexity thresholds.
/// @return The threshold table.
export [[nodiscard]] auto thresholds() -> std::span<const threshold>;

/// @brief Look up the threshold covering a capacity metric.
/// @param metric The fact kind to look up.
/// @return The threshold, or `std::nullopt` when no row covers the metric.
export [[nodiscard]] auto threshold_for(std::string_view metric) -> std::optional<threshold>;

/// @brief Why a packet could not be classified at all.
export enum class not_ready_reason : std::uint8_t {
  contradictory_facts,  ///< Two mutually exclusive facts both assert true.
  missing_required_fact ///< `acceptance_complete` is absent entirely.
};

/// @brief Render a `not_ready_reason` as the oracle's own text.
/// @param reason The reason.
/// @return The wire spelling.
export [[nodiscard]] auto not_ready_reason_name(not_ready_reason reason) -> std::string_view;

/// @brief A capacity metric the policy cannot judge.
export struct policy_gap {
  std::string_view metric; ///< The capacity metric the policy cannot judge.
  /// `std::nullopt` when no threshold row exists for the metric at all;
  /// otherwise the row that exists but lacks a `large_min`.
  std::optional<std::string_view> threshold_id;
};

/// @brief The compiled routing profile for a ready packet.
export struct profile {
  work_type        type = work_type::feature;   ///< The derived work type.
  std::string_view work_type_rule_id;           ///< The rule that derived it.
  std::string_view rule_version_;               ///< Always `rule_version`.
  complexity       risk = complexity::standard; ///< The derived complexity band.
  /// Threshold that decided complexity; EMPTY when complexity followed from
  /// explicit risk rather than a capacity metric.
  std::string_view complexity_threshold_id;
  /// The derived tier FLOOR. An operator may raise it, never lower it.
  tier                      tier_floor = tier::medium;
  std::string_view          tier_floor_rule_id; ///< The rule that set the floor.
  std::vector<matched_fact> matched_facts;      ///< Every fact a rule matched.
  std::vector<citation>     citations;          ///< Rule/fact pairs justifying the above.
};

/// @brief An operator may raise the tier above the floor; they may not lower it.
///
/// The floor is derived from the task's own evidence, so a request below it is
/// a request to ignore that evidence. A refusal, not a value the caller can
/// quietly read past.
/// @param value The compiled profile.
/// @param requested The operator's requested tier.
/// @return The requested tier, or `std::nullopt` when it is below the floor.
export [[nodiscard]] auto accept_tier(const profile& value, tier requested) -> std::optional<tier>;

// `Profile.cohort` (zig/src/engine/routing/profile.zig:268) is DELIBERATELY
// NOT PORTED, and the reason is a type mismatch rather than an oversight.
//
// The oracle's `store.Cohort` (zig/src/engine/routing/store.zig:644) has
// SEVEN fields: project_id, validation_policy_version, vendor, role, tier,
// work_type, complexity. `routing_policy_version` is NOT among them — in the
// oracle it lives one level up, on `store.Experiment`.
//
// This tree's `ranking::cohort` (ranking.cppm:140) carries EIGHT fields: the
// same seven plus `routing_policy_version`, folded in by the task-6096 port.
// Constructing one from a profile would therefore require this module to
// supply a `routing_policy_version` that the oracle's `Profile.cohort` never
// supplies and never sees — inventing a value, which is exactly the failure
// mode a behavior-preserving port exists to avoid.
//
// Nothing in this tree needs it: `models resolve` never builds a cohort, and
// `ranking` builds its own from experiment rows that DO carry the eighth
// field. Reinstating this belongs with whatever first needs a
// profile-derived cohort, together with a decision about which type is right.

/// @brief The three possible results of compiling a packet.
///
/// `not_ready_reason` and `policy_gap` are distinct alternatives precisely so
/// a caller cannot conflate a broken TASK with an unconfigured POLICY.
export using outcome = std::variant<profile, not_ready_reason, policy_gap>;

/// @brief Project a READY packet's facts into a profile.
///
/// Passing facts from a packet that is not ready is a programming error —
/// readiness is the packet layer's contract, and re-deriving it here would let
/// the two disagree. The Zig original asserts this; this port does not
/// re-check it either.
/// @param facts The ready packet's evidence, adapted to this module's `fact`.
/// @return The profile, or the reason it could not be produced.
export [[nodiscard]] auto compile(std::span<const fact> facts) -> outcome;

} // namespace planar::engine::models::profile
