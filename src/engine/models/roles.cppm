/// @file roles.cppm
/// @brief `planar.engine.models.roles` — every runtime role resolves from its
/// authoritative packet class, or says why it could not (plan 996, task 6111).
///
/// Behavior-preserving port (D2) of `zig/src/engine/routing/roles.zig` (325
/// lines, of which ~202 are implementation and 123 are colocated Zig `test`
/// blocks that this port replaces with its own Catch2 fixtures).
///
/// Nine roles run in the delivery loop, and they do not all have the same
/// evidence available at the moment they are dispatched:
///
///   * **Pre-task roles** — planner, spec-reviewer, ingestor, orchestrator —
///     run BEFORE a task exists. Their authority is a planning packet, which
///     establishes readiness but carries no work type or complexity, because
///     there is no unit of work yet to classify.
///   * **Task-bound roles** — coder, test-coder, reviewer, research, janitor —
///     run against a specific task. Their authority is that task's packet,
///     compiled into a profile with a derived tier, work type, and complexity.
///
/// When the authoritative packet is absent or not ready, resolution reports
/// the configured static fallback AND the reason — never a derived tier. That
/// distinction is the point: a tier invented from an unready packet looks
/// identical to one derived from a complete one, and an operator reading the
/// output has no way to tell that the routing decision rested on nothing.
///
/// ## WHY THIS LIVES IN `engine/models`
///
/// Same argument as `profile.cppm`, which see. This module's vocabulary is
/// `tier` plus `profile::outcome`, and both already live in this bucket.
///
/// The planning packet reaches `resolve_planning` as a
/// `std::optional<bool>`, not as `engine_ingest`'s `planning_packet`, so no
/// layer-2-to-layer-2 edge is created. That is the packet's ENTIRE
/// contribution to this decision and not a narrowing for convenience: the
/// oracle's `resolvePlanning` calls `p.ready()` and reads no other field of
/// the packet it is handed.
///
/// The optional carries the third state the bool cannot. `std::nullopt` means
/// NO packet was supplied (`no_packet`); `false` means one was supplied and is
/// unready (`packet_not_ready`). Those are different answers to "why is this
/// dispatch not backed by evidence?", and collapsing them into one bool would
/// erase exactly the distinction the verb exists to show.
///
/// Likewise `packet_policy_version` is a PARAMETER rather than an import: the
/// version constant lives on the packet in `engine_ingest`, and taking it as
/// an argument is what keeps this module free of that dependency.
///
/// ## THE WIRE SPELLING OF A ROLE IS NOT ITS ENUM SPELLING
///
/// `--role` is HYPHENATED on the command line (`spec-reviewer`, `test-coder`)
/// and UNDERSCORED in the enum. `role_from_wire` maps one to the other by
/// replacing `-` with `_`, which is what the oracle does
/// (`zig/src/cmd/planar/handlers/models.zig:546`).
///
/// That mapping is a no-op on an ALREADY-underscored name, so the oracle also
/// accepts `--role spec_reviewer` and `--role test_coder` — an undocumented
/// second spelling that `--help` does not list. Verified against the oracle,
/// which exits 0 and reports `"role":"spec_reviewer"`. Reproduced here
/// because it is the observable contract; see this module's tests, and
/// `role_to_text` for the emitted (always underscored) spelling.
module;

export module planar.engine.models.roles;

import std;
import planar.engine.models.registry;
import planar.engine.models.ranking;
import planar.engine.models.profile;

namespace planar::engine::models::roles {

using ranking::complexity;
using ranking::work_type;
using registry::tier;

/// @brief The version stamped on every resolution this module produces.
export inline constexpr std::string_view resolution_version = "routing-roles-v1";

/// @brief Every role the delivery loop dispatches.
export enum class role : std::uint8_t {
  planner,       ///< Pre-task: drafts specs and roadmaps.
  spec_reviewer, ///< Pre-task: adversarially reviews drafts. Wire: `spec-reviewer`.
  ingestor,      ///< Pre-task: decomposes specs into a task graph.
  orchestrator,  ///< Pre-task: dispatches the delivery loop.
  coder,         ///< Task-bound: implements one task.
  test_coder,    ///< Task-bound: authors verification. Wire: `test-coder`.
  reviewer,      ///< Task-bound: reviews coder output.
  research,      ///< Task-bound: read-only investigation.
  janitor        ///< Task-bound: finalization and integration.
};

/// @brief Which packet is authoritative for a role.
///
/// A property of WHEN the role runs, not of what it does.
export enum class packet_class : std::uint8_t {
  planning, ///< Authority is a planning packet; the role runs before any task.
  task      ///< Authority is a task packet compiled into a profile.
};

/// @brief Where a resolution's routing values came from.
///
/// An operator must be able to tell a derived decision from a fallback at a
/// glance.
export enum class source : std::uint8_t {
  packet,         ///< Derived from a ready authoritative packet.
  static_fallback ///< A policy floor the operator configured in advance.
};

/// @brief Why the authoritative packet could not be used.
export enum class fallback_reason : std::uint8_t {
  no_packet,        ///< No packet was supplied at all.
  packet_not_ready, ///< The packet exists but its readiness reasons are non-empty.
  /// The packet is ready, but policy could not produce a profile (e.g. a
  /// populated metric with no threshold covering it).
  policy_not_ready
};

/// @brief Parse a role from its COMMAND-LINE spelling.
///
/// Hyphens are mapped to underscores before the lookup, so both
/// `spec-reviewer` and `spec_reviewer` resolve — see this module's header for
/// why the second spelling is accepted.
/// @param text The wire spelling.
/// @return The role, or `std::nullopt` for anything else.
export [[nodiscard]] auto role_from_wire(std::string_view text) -> std::optional<role>;

/// @brief Render a role as the oracle's EMITTED spelling, which is underscored.
///
/// This is `@tagName` in the oracle, so `spec_reviewer` and `test_coder` —
/// NOT the hyphenated form `--role` accepts.
/// @param value The role.
/// @return The underscored spelling.
export [[nodiscard]] auto role_to_text(role value) -> std::string_view;

/// @brief The authoritative packet class for a role.
/// @param value The role.
/// @return `planning` for pre-task roles, `task` for task-bound roles.
export [[nodiscard]] auto class_of(role value) -> packet_class;

/// @brief Render a packet class as the oracle's own text.
/// @param value The class.
/// @return The wire spelling.
export [[nodiscard]] auto packet_class_to_text(packet_class value) -> std::string_view;

/// @brief Render a source as the oracle's own text.
/// @param value The source.
/// @return The wire spelling.
export [[nodiscard]] auto source_to_text(source value) -> std::string_view;

/// @brief Render a fallback reason as the oracle's own text.
/// @param value The reason.
/// @return The wire spelling.
export [[nodiscard]] auto fallback_reason_to_text(fallback_reason value) -> std::string_view;

/// @brief The configured static fallback for a role.
///
/// Deliberately tier-only: a fallback is a policy floor an operator chose in
/// advance, not a derived classification, so it carries no work type or
/// complexity to masquerade as packet-derived evidence.
export struct static_fallback {
  tier tier_ = tier::medium; ///< The tier an operator configured in advance.
};

/// @brief One role's resolved routing decision.
export struct resolution {
  role         role_   = role::coder;             ///< The role being resolved.
  packet_class class_  = packet_class::task;      ///< Its authoritative packet class.
  source       source_ = source::static_fallback; ///< Packet-derived, or a configured floor.

  /// The resolved routing tier. For a packet-backed task role this is the
  /// profile's derived FLOOR — an operator may raise it, never lower it, so
  /// the floor is the routing baseline rather than a fixed choice.
  tier tier_ = tier::medium;

  /// The derived work type. Present only when derived from a task packet;
  /// empty for every planning role and every fallback — planning packets
  /// classify no work, and a fallback classifies nothing at all.
  std::optional<work_type> work_type_;
  /// The derived complexity band, under the same rule as `work_type_`.
  std::optional<complexity> complexity_;

  /// Set exactly when `source_ == source::static_fallback`.
  std::optional<fallback_reason> fallback_reason_;
  /// The rule version that produced a packet-derived profile.
  std::optional<std::string_view> rule_version;
};

/// @brief Whether the decision rests on a packet rather than a configured floor.
/// @param value The resolution.
/// @return True when `source_ == source::packet`.
export [[nodiscard]] auto packet_backed(const resolution& value) -> bool;

/// @brief Resolve a task-bound role from its compiled profile outcome.
///
/// The caller must pass a role whose `class_of` is `task`: routing a planning
/// role through a task profile would attach a work type to a role that runs
/// before any work is classified. The oracle asserts this; this port does not
/// re-check it either.
/// @param value The task-bound role.
/// @param outcome The compiled profile outcome, or `std::nullopt` for no packet.
/// @param fallback The configured static fallback.
/// @return The resolution.
export [[nodiscard]] auto resolve_task(role value, const std::optional<profile::outcome>& outcome, static_fallback fallback)
    -> resolution;

/// @brief Resolve a task-bound role starting from packet READINESS.
///
/// `profile::compile` presumes a ready packet, so a caller holding an UNREADY
/// one cannot produce an `outcome` at all. Without this entry point the caller
/// would have to invent a profile-level reason (`missing_required_fact`) to
/// describe a packet-level problem, which would misattribute the failure — the
/// packet is incomplete, the profile rules are fine.
/// @param value The task-bound role.
/// @param packet_ready Whether the task packet is ready.
/// @param outcome The compiled outcome; ignored when `packet_ready` is false.
/// @param fallback The configured static fallback.
/// @return The resolution.
export [[nodiscard]] auto resolve_task_packet(role value, bool packet_ready, const std::optional<profile::outcome>& outcome,
                                              static_fallback fallback) -> resolution;

/// @brief Resolve a pre-task role from its planning packet's readiness.
///
/// A ready planning packet still yields no work type or complexity — there is
/// no task to classify — so a planning role's routing is its configured tier
/// either way. What changes is whether the decision is packet-backed: a ready
/// packet means the role was dispatched against established readiness, and an
/// absent or unready one means it was not, which is exactly what an operator
/// needs to see before approving the spawn.
///
/// `packet_ready` is `std::nullopt` when no packet was supplied at all
/// (`no_packet`), and `false` when one was supplied but is unready
/// (`packet_not_ready`). Collapsing the two would erase the distinction the
/// verb exists to show.
/// @param value The pre-task role.
/// @param packet_ready Readiness, or `std::nullopt` when no packet was supplied.
/// @param fallback The configured static fallback.
/// @param packet_policy_version The packet layer's policy version, stamped on
///        a packet-backed planning resolution. Passed IN rather than imported
///        so this module needs no edge to `engine_ingest`, where the packet
///        and its version constant live.
/// @return The resolution.
export [[nodiscard]] auto resolve_planning(role value, std::optional<bool> packet_ready, static_fallback fallback,
                                           std::string_view packet_policy_version) -> resolution;

} // namespace planar::engine::models::roles
