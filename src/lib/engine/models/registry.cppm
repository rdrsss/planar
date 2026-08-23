/// @file registry.cppm
/// @brief `planar.engine.models.registry` — the opaque candidate registry
/// behind the ten `planar models registry *` schema leaves (plan 996, task
/// 6096).
///
/// Behavior-preserving port (D2) of the registry half of
/// zig/src/engine/routing/store.zig. Owns the SQLite reads and writes for the
/// three tables migration 00030 introduced on the registration side:
///
///   - `routing_candidates`         — one row per (vendor, candidate_id)
///                                    opaque registration.
///   - `routing_candidate_bindings` — the explicit (candidate, role, tier)
///                                    allow-list. A candidate with no binding
///                                    for a role/tier is never eligible for it.
///   - `routing_host_observations`  — append-only, versioned host capability
///                                    evidence. Never updated in place.
///
/// ## Opacity is the organizing rule
///
/// Planar derives NOTHING from a candidate identifier's text. A candidate is
/// an opaque string plus explicitly recorded evidence; there is no parsing of
/// vendor prefixes, no version comparison, no family inference. Every
/// capability claim has to arrive as a `routing_host_observations` row an
/// operator or host actually wrote. `evaluate_eligibility` below is the whole
/// decision surface, and each of its six gates is independent.
///
/// ## Oracle-derived semantics (NOT inferred from the Zig source)
///
/// Each of the following was established by running
/// `./zig/zig-out/bin/planar` against a scratch database, not by reading its
/// implementation:
///
/// - **`observation_fresh` compares timestamps as BYTES, not as dates.** The
///   gate is `now < expires_at` under plain lexicographic ordering. For the
///   ISO-8601 `...Z` spellings the CLI accepts this agrees with chronological
///   order, but it is genuinely a string compare and a differently-formatted
///   timestamp would sort, not parse. Captured:
///
///       $Z models registry observe --candidate 1 --host claude-code \
///           --version 1 --availability available --spawn-verification verified \
///           --evidence-ref ev-1 --captured-at 2026-08-01T00:00:00Z \
///           --expires-at 2026-09-01T00:00:00Z
///       $Z models registry eligibility --candidate 1 --host claude-code \
///           --role coder --tier medium --now 2026-08-15T00:00:00Z
///         -> ..."observation_fresh":true...
///       ... --now 2026-10-15T00:00:00Z
///         -> ..."observation_fresh":false,... "host_observation_expired"
///
/// - **Host identity is never substituted.** An observation made by host A
///   does not make a candidate available on host B, regardless of version.
///   Captured — the same candidate that is fully eligible on `claude-code`
///   reports `provider_cli_unavailable` + `exact_spawn_unverified` +
///   `host_observation_expired` on `other-host`:
///
///       $Z models registry eligibility --candidate 1 --host other-host \
///           --role coder --tier medium --now 2026-08-15T00:00:00Z \
///           --override-supported --policy-permits
///         -> {"candidate":1,"host":"other-host","eligible":false,...
///             "reasons":["provider_cli_unavailable","exact_spawn_unverified",
///                        "host_observation_expired"]}
///
/// - **`role_tier_bound` folds the registration's `enabled` flag in.** It is
///   `enabled AND binding_present`, so disabling a candidate revokes every
///   binding at once without deleting a row. This is why a disabled candidate
///   reports `role_tier_not_bound` rather than some separate "disabled"
///   reason — captured against candidate 2 (added `--disabled`), which named
///   all six reasons.
///
/// - **`bind` is idempotent; `unbind` is not existence-checked.** A repeat
///   `bind` of the same (candidate, role, tier) exits 0 and writes nothing
///   (`on conflict do nothing`); an `unbind` of a binding that was never
///   created also exits 0. Captured — running the same bind twice both
///   produced exit 0 with empty stdout, and `models registry list` still
///   reported `bindings=1`.
///
/// - **`bind` against a missing candidate is a QUERY failure, not
///   `not_found`.** The FK rejects it and the oracle reports exit 1
///   `error: binding candidate: QueryFailed` — NOT the `NotFound` that
///   `remove` and `eligibility` produce for the same missing id. Captured;
///   this asymmetry is real and is preserved here rather than smoothed over.
///
/// - **`update` writes `enabled` unconditionally.** There is no
///   `--enabled` flag: the handler passes `!--disabled`, so
///   `models registry update --candidate 2 --order 7` on a DISABLED candidate
///   silently re-enables it. Captured — candidate 2 went from
///   `enabled=false` to `enabled=true order=7`.
///
/// ## What is NOT ported, named rather than silently dropped
///
/// - **`models resolve`,** the fourteenth `models` leaf. It rests on
///   zig/src/engine/routing/{roles,profile,packet}.zig (325 + 726 + 1674
///   lines) — role classification, the profile rule compiler, and live
///   task/planning packet assembly. That is a self-contained subsystem three
///   times this bucket's size and it reaches into plans, tasks, questions,
///   decisions, and routing facts. Deferred WITH its dependency, not
///   architecturally blocked. Its refusal surface IS captured for the cycle
///   that ports it: `models resolve --role coder` (no `--task`) exits 2 with
///   `error: --task is required for task-bound role 'coder'`, and
///   `--role nosuch` exits 2 with `error: unknown role 'nosuch'`.
///
/// - **`policy.audit` rows.** No ported bucket writes them (see
///   engine/planning/CMakeLists.txt, engine/promotion, engine/runtime,
///   engine/runs). No `models` leaf reads or emits audit rows, so no
///   observable CLI contract is affected.

module;

export module planar.engine.models.registry;

import std;
import planar.db;

namespace planar::engine::models::registry {

/// @brief The routing tier set, mirroring the schema's CHECK constraint on
/// `routing_candidate_bindings.tier`.
export enum class tier {
  small,  ///< Cheapest/fastest tier.
  medium, ///< The codified default tier.
  large   ///< Reserved for schema/architectural work.
};

/// @brief Parse the schema's tier text.
/// @param text One of `"small"`, `"medium"`, `"large"`.
/// @return The tier, or `std::nullopt` for anything else.
export auto tier_from_text(std::string_view text) -> std::optional<tier>;

/// @brief Render a tier as the schema's own text.
/// @param value The tier to render.
/// @return `"small"`, `"medium"`, or `"large"`.
export auto tier_to_text(tier value) -> std::string_view;

/// @brief What a host reported about provider reachability.
///
/// THREE values, not two. `unknown` is distinct from `unavailable`: "we did
/// not find out" is not the same claim as "we checked and it is down", and
/// collapsing them would let an unprobed host look like a probed one. Both
/// fail the `cli_available` gate, but only one of them is a finding.
/// Oracle-confirmed that the CLI accepts all three
/// (`--availability unknown` stores a row; `--availability bogus` is
/// `error: invalid availability: bogus`).
export enum class availability {
  available,   ///< The host can reach the provider surface.
  unavailable, ///< The host checked and it is NOT reachable.
  unknown      ///< The host did not establish either way.
};

/// @brief Parse the schema's availability text.
/// @param text One of `"available"`, `"unavailable"`, `"unknown"`.
/// @return The value, or `std::nullopt` for anything else.
export auto availability_from_text(std::string_view text) -> std::optional<availability>;

/// @brief Render an availability as the schema's own text.
/// @param value The value to render.
/// @return The wire spelling.
export auto availability_to_text(availability value) -> std::string_view;

/// @brief What a host reported about an exact spawn against this candidate.
///
/// FOUR values, not two. `unverified` (never attempted), `failed` (the spawn
/// itself did not complete), and `mismatch` (it completed and returned a
/// DIFFERENT identity) are three genuinely different findings, and `mismatch`
/// in particular is the alarm condition the whole opaque-identity discipline
/// exists to surface. All three fail the `exact_spawn_verified` gate, but an
/// operator reading the row needs to know which one happened.
/// Oracle-confirmed the CLI accepts all four.
export enum class spawn_verification {
  verified,   ///< A real spawn returned the exact requested identity.
  unverified, ///< No spawn was attempted.
  failed,     ///< The spawn did not complete.
  mismatch    ///< The spawn completed and returned a DIFFERENT identity.
};

/// @brief Parse the schema's spawn-verification text.
/// @param text One of `"verified"`, `"unverified"`, `"failed"`, `"mismatch"`.
/// @return The value, or `std::nullopt` for anything else.
export auto spawn_verification_from_text(std::string_view text) -> std::optional<spawn_verification>;

/// @brief Render a spawn verification as the schema's own text.
/// @param value The value to render.
/// @return The wire spelling.
export auto spawn_verification_to_text(spawn_verification value) -> std::string_view;

/// @brief One `routing_candidates` row.
export struct registration {
  std::int64_t id = 0;                      ///< The autoincrement key; the operator-facing handle.
  std::string  vendor;                      ///< Opaque vendor string; never parsed.
  std::string  candidate_id;                ///< Opaque candidate identifier; never parsed.
  bool         enabled              = true; ///< Master switch; folds into `role_tier_bound`.
  std::int64_t fallback_order       = 0;    ///< Deterministic fallback position; lower is earlier.
  std::int64_t registration_version = 1;    ///< Bumped by every `update`.
  std::string  compatibility_source;        ///< `native` or `legacy_config`.
};

/// @brief One `routing_candidate_bindings` row.
export struct binding {
  std::int64_t candidate_id = 0;     ///< The bound candidate's `routing_candidates.id`.
  std::string  role;                 ///< Opaque role string; never parsed.
  tier         tier_ = tier::medium; ///< The allowed tier.
};

/// @brief One `routing_host_observations` row.
export struct host_observation {
  std::int64_t       id           = 0;                                     ///< The autoincrement key.
  std::int64_t       candidate_id = 0;                                     ///< The observed candidate.
  std::string        host_id;                                              ///< The observing host; never substituted.
  std::int64_t       observation_version = 1;                              ///< Monotonic per (candidate, host).
  availability       availability_       = availability::unavailable;      ///< Reported reachability.
  spawn_verification spawn_verification_ = spawn_verification::unverified; ///< Exact-spawn proof.
  std::string        evidence_ref;                                         ///< Opaque pointer to the evidence.
  std::string        captured_at;                                          ///< When the evidence was taken.
  std::string        expires_at;                                           ///< After which it is stale; compared BYTEWISE.
};

/// @brief A registration together with its bindings and newest observation.
///
/// This is the shape `models registry list` and `models registry export`
/// serialize. `latest_observation` is unset when the candidate has none — or,
/// for `get_for_host`, when the requested host specifically has none.
export struct candidate {
  registration                    registration_;      ///< The `routing_candidates` row.
  std::vector<binding>            bindings;           ///< Ordered by role then tier.
  std::optional<host_observation> latest_observation; ///< Newest by version then id.
};

/// @brief Failure surface for every operation in this module.
export enum class registry_error {
  not_found,     ///< No candidate with that id.
  conflict,      ///< A UNIQUE / FK insert conflict.
  invalid_value, ///< A value the engine refuses before reaching SQLite.
  query_failed   ///< Any other SQLite failure.
};

/// @brief Reject values that cannot safely cross a JSON or argv boundary.
///
/// Deliberately permissive about punctuation, whitespace, and leading dashes:
/// candidate identifiers are OPAQUE data, and a registry that rejected an
/// identifier a vendor actually ships would be worse than one that stores it
/// verbatim. Only the empty string and C0/DEL control bytes are refused.
/// @param value The candidate text.
/// @return True when the value may be stored.
export auto valid_opaque_value(std::string_view value) -> bool;

/// @brief Arguments to `create`.
export struct create_args {
  std::string_view vendor;                          ///< Opaque vendor string.
  std::string_view candidate_id;                    ///< Opaque candidate identifier.
  bool             enabled              = true;     ///< `models registry add` passes `!--disabled`.
  std::int64_t     fallback_order       = 0;        ///< Must be >= 0.
  std::string_view compatibility_source = "native"; ///< `native` or `legacy_config` only.
};

/// @brief Register one exact opaque candidate.
///
/// Every failure — a duplicate (vendor, candidate_id), a rejected opaque
/// value, a negative order, an unknown compatibility source — surfaces as
/// `conflict` or `invalid_value`; the oracle renders both through the same
/// `error: registering opaque candidate: <Name>` prefix (exit 3 for
/// `Conflict`, captured).
/// @param conn An open, migrated database connection.
/// @param args The registration's opaque identity and fallback position.
/// @return The new `routing_candidates.id`.
export auto create(db::connection& conn, const create_args& args) -> std::expected<std::int64_t, registry_error>;

/// @brief Look up a candidate's id by its opaque (vendor, candidate_id) pair.
/// @param conn An open, migrated database connection.
/// @param vendor The opaque vendor string.
/// @param candidate_id The opaque candidate identifier.
/// @return The id, or `std::nullopt` when unregistered.
export auto find_id(db::connection& conn, std::string_view vendor, std::string_view candidate_id)
    -> std::expected<std::optional<std::int64_t>, registry_error>;

/// @brief Set a candidate's enabled state and fallback order, bumping
/// `registration_version`.
///
/// `enabled` is written unconditionally — see this module's header for the
/// oracle capture showing `models registry update` re-enabling a disabled
/// candidate as a side effect of reordering it.
/// @param conn An open, migrated database connection.
/// @param id The candidate to update.
/// @param enabled The new master switch value.
/// @param fallback_order The new fallback position; must be >= 0.
/// @return Success, or `not_found` when no row matched.
export auto update(db::connection& conn, std::int64_t id, bool enabled, std::int64_t fallback_order)
    -> std::expected<void, registry_error>;

/// @brief Remove a candidate when no immutable evidence references it.
///
/// The schema's `on delete restrict` foreign keys from
/// `routing_dispatch_snapshots` and `routing_terminal_samples` are what make
/// this safe: a candidate any recorded evidence points at cannot be deleted,
/// so the audit trail can never be orphaned.
/// @param conn An open, migrated database connection.
/// @param id The candidate to remove.
/// @return Success, or `not_found` when no row matched.
export auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, registry_error>;

/// @brief Allow one role and tier for a candidate, idempotently.
/// @param conn An open, migrated database connection.
/// @param candidate_id The candidate to bind.
/// @param role The opaque role string.
/// @param tier_ The allowed tier.
/// @return Success (including when the binding already existed), or
/// `query_failed` when the candidate does not exist — see this module's
/// header for why that is NOT `not_found`.
export auto bind(db::connection& conn, std::int64_t candidate_id, std::string_view role, tier tier_)
    -> std::expected<void, registry_error>;

/// @brief Remove one explicit role and tier binding.
///
/// Not existence-checked: unbinding something that was never bound succeeds.
/// @param conn An open, migrated database connection.
/// @param candidate_id The candidate to unbind.
/// @param role The opaque role string.
/// @param tier_ The tier to remove.
/// @return Success.
export auto unbind(db::connection& conn, std::int64_t candidate_id, std::string_view role, tier tier_)
    -> std::expected<void, registry_error>;

/// @brief Arguments to `observe`.
export struct observe_args {
  std::int64_t       candidate_id = 0;                                     ///< The observed candidate.
  std::string_view   host_id;                                              ///< The observing host.
  std::int64_t       observation_version = 1;                              ///< Must be > 0.
  availability       availability_       = availability::unavailable;      ///< Reported reachability.
  spawn_verification spawn_verification_ = spawn_verification::unverified; ///< Exact-spawn proof.
  std::string_view   evidence_ref;                                         ///< Opaque pointer to the evidence.
  std::string_view   captured_at;                                          ///< When the evidence was taken.
  std::string_view   expires_at;                                           ///< Must sort strictly AFTER `captured_at`.
};

/// @brief Append an exact, versioned host capability observation.
///
/// `expires_at` must be strictly greater than `captured_at` under the same
/// BYTEWISE comparison `evaluate_eligibility` uses for freshness — an
/// observation that expires before it was captured is refused rather than
/// stored as permanently stale.
/// @param conn An open, migrated database connection.
/// @param args The observation's host, version, verdicts, and validity window.
/// @return The new `routing_host_observations.id`.
export auto observe(db::connection& conn, const observe_args& args) -> std::expected<std::int64_t, registry_error>;

/// @brief List every registration with its bindings and newest observation.
///
/// Ordered `vendor, fallback_order, candidate_id` — the deterministic
/// fallback sequence, not insertion order. `latest_observation` here is the
/// newest across ALL hosts; use `get_for_host` when host identity matters.
/// @param conn An open, migrated database connection.
/// @return Every candidate, in fallback order.
export auto list(db::connection& conn) -> std::expected<std::vector<candidate>, registry_error>;

/// @brief Read one candidate, resolving its observation against ONE host.
///
/// Observations made by other hosts are never substituted, even when they
/// carry a larger version number — see this module's header for the capture.
/// @param conn An open, migrated database connection.
/// @param id The candidate to read.
/// @param host_id The host whose observation is authoritative here.
/// @return The candidate, or `not_found`.
export auto get_for_host(db::connection& conn, std::int64_t id, std::string_view host_id)
    -> std::expected<candidate, registry_error>;

/// @brief The six independent eligibility gates.
///
/// Every gate is a separate question with a separate answer. They are never
/// collapsed into a single boolean at the storage layer, because an operator
/// asking "why is this candidate not being used?" needs the specific gate,
/// not a verdict.
export struct eligibility {
  bool cli_available                   = false; ///< The host reported the provider reachable.
  bool exact_spawn_verified            = false; ///< An exact spawn was proven.
  bool role_tier_bound                 = false; ///< Enabled AND explicitly bound for this role/tier.
  bool role_surface_override_supported = false; ///< The host surface can carry the override.
  bool host_policy_permits             = false; ///< Host policy allows this candidate.
  bool observation_fresh               = false; ///< `now` sorts strictly before `expires_at`.

  /// @brief Whether all six gates passed.
  /// @return True only when every gate is true.
  [[nodiscard]] auto eligible() const -> bool;
};

/// @brief The named reason for each failed gate, in gate order.
export enum class eligibility_reason {
  provider_cli_unavailable,          ///< `cli_available` failed.
  exact_spawn_unverified,            ///< `exact_spawn_verified` failed.
  role_tier_not_bound,               ///< `role_tier_bound` failed.
  role_surface_override_unsupported, ///< `role_surface_override_supported` failed.
  host_policy_denied,                ///< `host_policy_permits` failed.
  host_observation_expired           ///< `observation_fresh` failed.
};

/// @brief Render an eligibility reason as the wire text.
/// @param value The reason to render.
/// @return The snake_case wire spelling.
export auto eligibility_reason_to_text(eligibility_reason value) -> std::string_view;

/// @brief Collect the named reasons for every failed gate, in gate order.
///
/// The order is fixed and matches the gate declaration order, so a caller
/// diffing two runs sees a stable sequence.
/// @param gates The evaluated gates.
/// @return One reason per failed gate; empty when eligible.
export auto reasons(const eligibility& gates) -> std::vector<eligibility_reason>;

/// @brief Inputs the host and configuration boundary has already established.
///
/// Planar never derives any of these from a candidate identifier or its
/// display metadata — that is what makes the candidate opaque.
export struct eligibility_input {
  bool                            enabled                         = false; ///< The registration's master switch.
  bool                            binding_present                 = false; ///< An exact (role, tier) binding exists.
  bool                            role_surface_override_supported = false; ///< Host surface capability.
  bool                            host_policy_permits             = false; ///< Host policy verdict.
  std::optional<host_observation> observation;                             ///< The host's newest observation, if any.
  std::string_view                now;                                     ///< Evaluation instant; compared BYTEWISE.
};

/// @brief Evaluate all six gates from already-established inputs.
///
/// Pure: no database access, no clock read, no environment read. The
/// freshness comparison is a BYTE comparison of `now` against
/// `observation->expires_at`, not a date parse — see this module's header.
/// @param input The established host, policy, binding, and observation state.
/// @return The six gate verdicts.
export auto evaluate_eligibility(const eligibility_input& input) -> eligibility;

/// @brief The outcome of comparing a requested identity to an actual one.
export enum class identity_verification {
  matched,                 ///< Vendor and candidate both matched exactly.
  missing_actual_identity, ///< The host reported no actual identity at all.
  vendor_mismatch,         ///< The vendor differed.
  candidate_mismatch       ///< The vendor matched but the candidate did not.
};

/// @brief Render an identity-verification outcome as the wire text.
/// @param value The outcome to render.
/// @return The snake_case wire spelling.
export auto identity_verification_to_text(identity_verification value) -> std::string_view;

/// @brief Compare a requested spawn identity to the actual one, without
/// aliasing.
///
/// Exact byte equality on both halves. There is deliberately no
/// normalization, no case folding, and no alias table: an alias is exactly
/// the mechanism by which a host can silently serve a different model than
/// the one an experiment declared, which would corrupt every terminal sample
/// derived from that dispatch.
///
/// Vendor is checked BEFORE candidate, so a candidate that differs in both
/// halves reports `vendor_mismatch` — captured:
/// `verify-identity --candidate 1 --actual-vendor openai --actual-id cand-1`
/// yields `{"candidate":1,"identity":"vendor_mismatch"}`.
/// @param requested_vendor The registered vendor.
/// @param requested_candidate The registered candidate identifier.
/// @param actual_vendor The vendor the host actually spawned, if reported.
/// @param actual_candidate The candidate the host actually spawned, if reported.
/// @return The verification outcome.
export auto verify_actual_identity(std::string_view requested_vendor, std::string_view requested_candidate,
                                   std::optional<std::string_view> actual_vendor,
                                   std::optional<std::string_view> actual_candidate) -> identity_verification;

} // namespace planar::engine::models::registry
