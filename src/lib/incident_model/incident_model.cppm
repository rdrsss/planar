/// @file incident_model.cppm
/// @brief `planar.incident_model` -- the value types shared by the diagnose
/// engine and the incident ledger (plan 1132, tech spec 689).
///
/// Entry points: the `finding`, `coverage_row`, `incident` and `occurrence`
/// records; `fingerprint()` and `evidence_digest()` (SHA-256 over
/// `planar.sha256`); the severity ordinal (`severity_ordinal()`,
/// `incident_severity()`, `compare_severity()`); `check_kind`;
/// `incident_status` (the closed set shown beside every finding); and
/// `compare_findings()`, the one sort order every caller prints.
///
/// Error-boundary contract: nothing here touches a database or the clock, and
/// nothing fails. Parsers return `std::nullopt` for text outside their closed
/// set; every other function is total. Severities are compared by ordinal and
/// never by text, so a rename of a name cannot reorder findings.
export module planar.incident_model;

import std;

namespace planar::incident_model {

/// @brief The incident severity scale, ordered by ordinal (`info` 0 to
/// `critical` 4). `low` and `critical` are reserved: no catalog check
/// produces them today.
export enum class severity { info, low, medium, high, critical };

/// @brief The severity a check reports in a diagnosis.
export enum class diagnostic_severity { info, warning, error };

/// @brief Whether a check's condition can stop being true.
export enum class check_kind {
  state, ///< A condition that can stop being true (an active lapsed claim).
  event  ///< Something that happened and cannot un-happen.
};

/// @brief The lifecycle state of a recorded incident.
export enum class incident_state { open, acknowledged, reopened, resolved, dismissed };

/// @brief The closed set of statuses shown beside a finding.
export enum class incident_status { new_incident, recurring, escalated, reopened, dismissed, resolved, not_recorded };

/// @brief How an input was read in one run (decision 1346).
export enum class coverage_state { observed, unavailable, disabled, not_applicable };

/// @brief The closed set of incident sources, each with one defined writer.
export enum class incident_source { diagnose_check, claim_failure, cli_failure, conversational };

/// @brief The ordinal of `s` on the incident scale: `info` 0 through `critical` 4.
/// @param s The severity.
/// @return The ordinal.
export auto severity_ordinal(severity s) noexcept -> int;

/// @brief The text of an incident severity (`info`, `low`, `medium`, `high`, `critical`).
/// @param s The severity.
/// @return A static name.
export auto severity_name(severity s) noexcept -> std::string_view;

/// @brief Parses an incident severity name.
/// @param text The name.
/// @return The severity, or `std::nullopt` outside the closed set.
export auto parse_severity(std::string_view text) noexcept -> std::optional<severity>;

/// @brief Maps a diagnostic severity onto the incident scale: `info` to
/// `info`, `warning` to `medium`, `error` to `high`.
/// @param s The diagnostic severity.
/// @return The incident severity.
export auto incident_severity(diagnostic_severity s) noexcept -> severity;

/// @brief The text of a diagnostic severity (`info`, `warning`, `error`).
/// @param s The diagnostic severity.
/// @return A static name.
export auto diagnostic_severity_name(diagnostic_severity s) noexcept -> std::string_view;

/// @brief Parses a diagnostic severity name.
/// @param text The name.
/// @return The severity, or `std::nullopt` outside the closed set.
export auto parse_diagnostic_severity(std::string_view text) noexcept -> std::optional<diagnostic_severity>;

/// @brief Compares two incident severities by ordinal.
/// @param a The left severity.
/// @param b The right severity.
/// @return `a` ordered against `b` by ordinal.
export auto compare_severity(severity a, severity b) noexcept -> std::strong_ordering;

/// @brief The higher of two incident severities by ordinal.
/// @param a The left severity.
/// @param b The right severity.
/// @return The higher severity.
export auto max_severity(severity a, severity b) noexcept -> severity;

/// @brief Whether a finding of this diagnostic severity is recorded as an
/// incident (decision 1352: `info` findings are shown only).
/// @param s The diagnostic severity.
/// @return `true` for `warning` and `error`.
export auto is_recorded(diagnostic_severity s) noexcept -> bool;

/// @brief The text of a check kind (`state`, `event`).
/// @param k The kind.
/// @return A static name.
export auto check_kind_name(check_kind k) noexcept -> std::string_view;

/// @brief Parses a check kind.
/// @param text The name.
/// @return The kind, or `std::nullopt` outside the closed set.
export auto parse_check_kind(std::string_view text) noexcept -> std::optional<check_kind>;

/// @brief The text of an incident state (`open`, `acknowledged`, `reopened`, `resolved`, `dismissed`).
/// @param s The state.
/// @return A static name.
export auto incident_state_name(incident_state s) noexcept -> std::string_view;

/// @brief Parses an incident state.
/// @param text The name.
/// @return The state, or `std::nullopt` outside the closed set.
export auto parse_incident_state(std::string_view text) noexcept -> std::optional<incident_state>;

/// @brief The text of an incident status: `new`, `recurring`, `escalated`,
/// `reopened`, `dismissed`, `resolved` or `not-recorded`.
/// @param s The status.
/// @return A static name.
export auto incident_status_name(incident_status s) noexcept -> std::string_view;

/// @brief Parses an incident status.
/// @param text The name, with `not-recorded` hyphenated.
/// @return The status, or `std::nullopt` outside the closed set.
export auto parse_incident_status(std::string_view text) noexcept -> std::optional<incident_status>;

/// @brief Every incident status, in the order the recording summary lists them.
/// @return The closed set.
export auto all_incident_statuses() noexcept -> std::span<const incident_status>;

/// @brief The text of a coverage state (`observed`, `unavailable`, `disabled`, `not_applicable`).
/// @param s The state.
/// @return A static name.
export auto coverage_state_name(coverage_state s) noexcept -> std::string_view;

/// @brief The text of an incident source (`diagnose_check`, `claim_failure`, `cli_failure`, `conversational`).
/// @param s The source.
/// @return A static name.
export auto incident_source_name(incident_source s) noexcept -> std::string_view;

/// @brief Parses an incident source.
/// @param text The name.
/// @return The source, or `std::nullopt` outside the closed set.
export auto parse_incident_source(std::string_view text) noexcept -> std::optional<incident_source>;

/// @brief A reference to one entity: a lower-case kind and an id, written `kind:id`.
export struct entity_ref {
  std::string  kind;   ///< Lower-case letters and underscores, for example `claim`.
  std::int64_t id = 0; ///< The entity's row id.

  /// @brief Member-wise equality.
  /// @param other The other reference.
  /// @return `true` when kind and id both match.
  auto operator==(const entity_ref& other) const -> bool = default;
};

/// @brief Renders a reference as `kind:id`.
/// @param ref The reference.
/// @return The text.
export auto entity_ref_text(const entity_ref& ref) -> std::string;

/// @brief Parses `kind:id`.
/// @param text The text.
/// @return The reference, or `std::nullopt` when the kind is empty or not lower-case letters and
/// underscores, or the id is not a non-negative integer.
export auto parse_entity_ref(std::string_view text) -> std::optional<entity_ref>;

/// @brief Orders references by kind text, then by numeric id (so `task:9` sorts before `task:10`).
/// @param a The left reference.
/// @param b The right reference.
/// @return The ordering.
export auto compare_entity_refs(const entity_ref& a, const entity_ref& b) -> std::strong_ordering;

/// @brief One violation of one check in one run.
export struct finding {
  std::string              check_id;                             ///< Stable kebab-case check id.
  diagnostic_severity      severity = diagnostic_severity::info; ///< The reported severity.
  entity_ref               primary;                              ///< The entity the finding is about.
  std::vector<entity_ref>  evidence;       ///< Entity refs that define the problem; list `primary` here too.
  std::vector<std::string> evidence_times; ///< Violation-defining timestamps, in the check's fixed order.
  std::string              recovery;       ///< The recovery command or hint; empty when none.
};

/// @brief One input's coverage in one run.
export struct coverage_row {
  std::string    input;                                  ///< The input's name.
  coverage_state state = coverage_state::not_applicable; ///< How it was read.
  std::string    reason;                                 ///< A short code; empty when `observed`.
};

/// @brief The durable row for one fingerprint (decision 1350).
export struct incident {
  std::int64_t                id = 0;      ///< Row id.
  std::string                 fingerprint; ///< Unique across incidents.
  std::string                 check_id;    ///< The producing check; empty for a conversational incident.
  std::string                 category;    ///< A seeded `incident_categories` id.
  incident_source             source = incident_source::diagnose_check; ///< Which writer recorded it.
  std::optional<std::int64_t> plan_id;    ///< The primary entity's plan; null for clusters and plan-less entities.
  std::string                 scope_kind; ///< `repo`, `association` or `global`.
  std::optional<std::int64_t> scope_id;   ///< Null for `global`.
  incident_state              state            = incident_state::open; ///< Lifecycle state.
  severity                    max_observed     = severity::info;       ///< The highest severity ever observed.
  severity                    notified         = severity::info;       ///< The severity at the last notification.
  std::int64_t                occurrence_count = 0;                    ///< Distinct evidence digests, cumulative.
  std::string                 first_seen_at;                           ///< ISO-8601 instant.
  std::string                 last_seen_at;                            ///< ISO-8601 instant.
  std::string                 evidence_from;                           ///< Oldest violation instant seen.
  std::string                 state_changed_at;                        ///< ISO-8601 instant of the last transition.
  std::string                 state_reason;                            ///< The closed reason, `auto`, or empty.
  std::string                 resolved_at;                             ///< Empty unless resolved.
  std::string                 pruned_before;                           ///< Retention watermark; empty when never pruned.
};

/// @brief One distinct observation of an incident's evidence.
export struct occurrence {
  std::int64_t incident_id = 0; ///< The owning incident.
  std::string  evidence_digest; ///< 64 lowercase hex characters.
  std::string  observed_at;     ///< The recording run's evaluation instant.
  std::string  violation_at;    ///< The violation-defining instant.
};

/// @brief Builds a fingerprint: `<check-id>|<sorted, de-duplicated refs>`, for example
/// `claim-superseded-active|claim:4469,task:7312`. Timestamps are never part of it.
/// @param check_id The check id.
/// @param refs The evidence references, in any order, duplicates allowed.
/// @return The fingerprint.
export auto fingerprint(std::string_view check_id, std::span<const entity_ref> refs) -> std::string;

/// @brief The fingerprint of a finding: its check id and evidence references.
/// @param f The finding.
/// @return The fingerprint.
export auto finding_fingerprint(const finding& f) -> std::string;

/// @brief The evidence digest: SHA-256 (lowercase hex) of the fingerprint and the
/// violation-defining timestamps, in the order given. Two observations of the same
/// evidence share a digest; a changed fingerprint or any changed or reordered
/// timestamp changes it.
/// @param fingerprint_text The fingerprint.
/// @param times The violation-defining timestamps.
/// @return 64 lowercase hex characters.
export auto evidence_digest(std::string_view fingerprint_text, std::span<const std::string> times) -> std::string;

/// @brief The digest of a finding's evidence.
/// @param f The finding.
/// @return 64 lowercase hex characters.
export auto finding_digest(const finding& f) -> std::string;

/// @brief The earliest timestamp among a finding's evidence times.
/// @param f The finding.
/// @return The smallest time by text order (ISO-8601 sorts chronologically), or empty when it has none.
export auto earliest_evidence(const finding& f) -> std::string;

/// @brief The fixed finding order every caller prints: severity from highest to lowest by
/// ordinal (`error`, `warning`, `info`), then check id, then primary entity, then earliest
/// evidence time, then fingerprint as a final tie-break.
/// @param a The left finding.
/// @param b The right finding.
/// @return `a` ordered against `b`.
export auto compare_findings(const finding& a, const finding& b) -> std::strong_ordering;

/// @brief Sorts findings into the fixed order.
/// @param findings The findings; reordered in place.
export auto sort_findings(std::vector<finding>& findings) -> void;

} // namespace planar::incident_model
