/// @file registry.cpp
/// @brief Implementation of `planar.engine.models.registry` (plan 996, task
/// 6096). See registry.cppm for scope, the oracle-derived semantics, and the
/// cut list.

module planar.engine.models.registry;

import std;
import planar.db;

namespace planar::engine::models::registry {

namespace {

/// @brief Guarantee a non-null `data()` pointer for an empty view.
///
/// `sqlite3_bind_text(stmt, i, nullptr, 0, ...)` binds SQL **NULL**, not the
/// empty string, so a default-constructed `std::string_view` silently
/// violates the NOT NULL constraints all over `routing_candidates` and
/// `routing_host_observations`. Same defect and same local remedy as
/// engine/planning/annotation.cpp's and engine/runs/lifecycle.cpp's `nn`;
/// task 6097 tracks the root fix in `db::statement::bind_text` itself.
///
/// This guard is not merely defensive here: `valid_opaque_value` already
/// refuses the empty string for every OPAQUE column, but `create`'s
/// `compatibility_source` and every read-back path can still see a
/// default-constructed view from a caller.
auto nn(std::string_view s) -> std::string_view {
  return s.data() == nullptr ? std::string_view{""} : s;
}

/// @brief Bytewise ordering, matching Zig's `std::mem.order(u8, ...)`.
///
/// Deliberately NOT a date comparison — see registry.cppm's header for the
/// oracle capture proving the freshness gate sorts timestamps rather than
/// parsing them.
auto bytes_less(std::string_view lhs, std::string_view rhs) -> bool {
  return lhs.compare(rhs) < 0;
}

auto read_registration(const db::statement& stmt) -> registration {
  return registration{
      .id                   = stmt.column_int64(0),
      .vendor               = stmt.column_text(1),
      .candidate_id         = stmt.column_text(2),
      .enabled              = stmt.column_int64(3) != 0,
      .fallback_order       = stmt.column_int64(4),
      .registration_version = stmt.column_int64(5),
      .compatibility_source = stmt.column_text(6),
  };
}

constexpr std::string_view k_registration_columns = "select id, vendor, candidate_id, enabled, fallback_order, "
                                                    "registration_version, compatibility_source "
                                                    "from routing_candidates";

/// @brief Read a candidate's bindings, ordered `role, tier`.
auto read_bindings(db::connection& conn, std::int64_t id) -> std::expected<std::vector<binding>, registry_error> {
  auto stmt = conn.prepare("select role, tier from routing_candidate_bindings "
                           "where candidate_id = ? order by role, tier");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_int64(1, id)) {
    return std::unexpected(registry_error::query_failed);
  }
  std::vector<binding> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(registry_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    const auto tier_text = stmt->column_text(1);
    const auto parsed    = tier_from_text(tier_text);
    if (!parsed) {
      // The schema CHECK makes this unreachable for rows this engine wrote;
      // refusing rather than defaulting keeps a hand-edited database from
      // silently reporting a binding it does not have.
      return std::unexpected(registry_error::invalid_value);
    }
    out.push_back(binding{.candidate_id = id, .role = stmt->column_text(0), .tier_ = *parsed});
  }
  return out;
}

/// @brief Read one observation row from a statement positioned on a row.
auto read_observation(const db::statement& stmt, std::int64_t candidate_id) -> std::expected<host_observation, registry_error> {
  const auto avail = availability_from_text(stmt.column_text(3));
  const auto spawn = spawn_verification_from_text(stmt.column_text(4));
  if (!avail || !spawn) {
    return std::unexpected(registry_error::invalid_value);
  }
  return host_observation{
      .id                  = stmt.column_int64(0),
      .candidate_id        = candidate_id,
      .host_id             = stmt.column_text(1),
      .observation_version = stmt.column_int64(2),
      .availability_       = *avail,
      .spawn_verification_ = *spawn,
      .evidence_ref        = stmt.column_text(5),
      .captured_at         = stmt.column_text(6),
      .expires_at          = stmt.column_text(7),
  };
}

constexpr std::string_view k_observation_columns = "select id, host_id, observation_version, availability, "
                                                   "spawn_verification, evidence_ref, captured_at, expires_at "
                                                   "from routing_host_observations";

/// @brief Newest observation across every host.
auto read_latest_observation(db::connection& conn, std::int64_t id)
    -> std::expected<std::optional<host_observation>, registry_error> {
  auto stmt = conn.prepare(std::string(k_observation_columns) +
                           " where candidate_id = ? order by observation_version desc, id desc limit 1");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_int64(1, id)) {
    return std::unexpected(registry_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(registry_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::optional<host_observation>{};
  }
  auto row = read_observation(*stmt, id);
  if (!row) {
    return std::unexpected(row.error());
  }
  return std::optional<host_observation>{*row};
}

/// @brief Newest observation made by ONE host. Never substitutes another
/// host's observation, even a higher-versioned one.
auto read_latest_observation_for_host(db::connection& conn, std::int64_t id, std::string_view host_id)
    -> std::expected<std::optional<host_observation>, registry_error> {
  auto stmt = conn.prepare(std::string(k_observation_columns) +
                           " where candidate_id = ? and host_id = ? order by observation_version desc, id desc limit 1");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_int64(1, id) || !stmt->bind_text(2, nn(host_id))) {
    return std::unexpected(registry_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(registry_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::optional<host_observation>{};
  }
  auto row = read_observation(*stmt, id);
  if (!row) {
    return std::unexpected(row.error());
  }
  return std::optional<host_observation>{*row};
}

} // namespace

auto tier_from_text(std::string_view text) -> std::optional<tier> {
  if (text == "small") {
    return tier::small;
  }
  if (text == "medium") {
    return tier::medium;
  }
  if (text == "large") {
    return tier::large;
  }
  return std::nullopt;
}

auto tier_to_text(tier value) -> std::string_view {
  switch (value) {
  case tier::small:
    return "small";
  case tier::medium:
    return "medium";
  case tier::large:
    return "large";
  }
  return "medium";
}

auto availability_from_text(std::string_view text) -> std::optional<availability> {
  if (text == "available") {
    return availability::available;
  }
  if (text == "unavailable") {
    return availability::unavailable;
  }
  if (text == "unknown") {
    return availability::unknown;
  }
  return std::nullopt;
}

auto availability_to_text(availability value) -> std::string_view {
  switch (value) {
  case availability::available:
    return "available";
  case availability::unavailable:
    return "unavailable";
  case availability::unknown:
    return "unknown";
  }
  return "unknown";
}

auto spawn_verification_from_text(std::string_view text) -> std::optional<spawn_verification> {
  if (text == "verified") {
    return spawn_verification::verified;
  }
  if (text == "unverified") {
    return spawn_verification::unverified;
  }
  if (text == "failed") {
    return spawn_verification::failed;
  }
  if (text == "mismatch") {
    return spawn_verification::mismatch;
  }
  return std::nullopt;
}

auto spawn_verification_to_text(spawn_verification value) -> std::string_view {
  switch (value) {
  case spawn_verification::verified:
    return "verified";
  case spawn_verification::unverified:
    return "unverified";
  case spawn_verification::failed:
    return "failed";
  case spawn_verification::mismatch:
    return "mismatch";
  }
  return "unverified";
}

auto valid_opaque_value(std::string_view value) -> bool {
  if (value.empty()) {
    return false;
  }
  for (const char raw : value) {
    const auto c = static_cast<unsigned char>(raw);
    if (c < 0x20 || c == 0x7F) {
      return false;
    }
  }
  return true;
}

auto create(db::connection& conn, const create_args& args) -> std::expected<std::int64_t, registry_error> {
  if (!valid_opaque_value(args.vendor) || !valid_opaque_value(args.candidate_id) || args.fallback_order < 0 ||
      !(args.compatibility_source == "native" || args.compatibility_source == "legacy_config")) {
    return std::unexpected(registry_error::invalid_value);
  }
  auto stmt = conn.prepare("insert into routing_candidates "
                           "(vendor, candidate_id, enabled, fallback_order, compatibility_source) "
                           "values (?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_text(1, nn(args.vendor)) || !stmt->bind_text(2, nn(args.candidate_id)) ||
      !stmt->bind_int64(3, args.enabled ? 1 : 0) || !stmt->bind_int64(4, args.fallback_order) ||
      !stmt->bind_text(5, nn(args.compatibility_source))) {
    return std::unexpected(registry_error::query_failed);
  }
  auto stepped = stmt->step();
  // The Zig original maps EVERY execParams failure here to Conflict, not just
  // a uniqueness violation, and the oracle renders it as exit 3
  // `error: registering opaque candidate: Conflict`. Preserved verbatim
  // rather than "improved" into a finer-grained mapping the CLI contract does
  // not expose.
  if (!stepped || *stepped != db::step_result::row) {
    return std::unexpected(registry_error::conflict);
  }
  return stmt->column_int64(0);
}

auto find_id(db::connection& conn, std::string_view vendor, std::string_view candidate_id)
    -> std::expected<std::optional<std::int64_t>, registry_error> {
  auto stmt = conn.prepare("select id from routing_candidates where vendor = ? and candidate_id = ?");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_text(1, nn(vendor)) || !stmt->bind_text(2, nn(candidate_id))) {
    return std::unexpected(registry_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(registry_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::optional<std::int64_t>{};
  }
  return std::optional<std::int64_t>{stmt->column_int64(0)};
}

auto update(db::connection& conn, std::int64_t id, bool enabled, std::int64_t fallback_order)
    -> std::expected<void, registry_error> {
  if (fallback_order < 0) {
    return std::unexpected(registry_error::invalid_value);
  }
  // `returning id` stands in for zig's `changes() != 1` check: a row comes
  // back exactly when one matched. `planar.db`'s connection exposes no
  // `sqlite3_changes` accessor, and RETURNING is the same signal.
  auto stmt = conn.prepare("update routing_candidates set enabled = ?, fallback_order = ?, "
                           "registration_version = registration_version + 1, "
                           "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                           "where id = ? returning id");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_int64(1, enabled ? 1 : 0) || !stmt->bind_int64(2, fallback_order) || !stmt->bind_int64(3, id)) {
    return std::unexpected(registry_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(registry_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(registry_error::not_found);
  }
  return {};
}

auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, registry_error> {
  auto stmt = conn.prepare("delete from routing_candidates where id = ? returning id");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_int64(1, id)) {
    return std::unexpected(registry_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    // An `on delete restrict` foreign key from recorded evidence lands here.
    return std::unexpected(registry_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(registry_error::not_found);
  }
  return {};
}

auto bind(db::connection& conn, std::int64_t candidate_id, std::string_view role, tier tier_)
    -> std::expected<void, registry_error> {
  if (!valid_opaque_value(role)) {
    return std::unexpected(registry_error::invalid_value);
  }
  auto stmt = conn.prepare("insert into routing_candidate_bindings (candidate_id, role, tier) "
                           "values (?, ?, ?) on conflict(candidate_id, role, tier) do nothing");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_int64(1, candidate_id) || !stmt->bind_text(2, nn(role)) || !stmt->bind_text(3, tier_to_text(tier_))) {
    return std::unexpected(registry_error::query_failed);
  }
  // A missing candidate trips the FK here and surfaces as `query_failed`,
  // NOT `not_found` — see registry.cppm's header for the oracle capture of
  // that asymmetry.
  if (!stmt->step()) {
    return std::unexpected(registry_error::query_failed);
  }
  return {};
}

auto unbind(db::connection& conn, std::int64_t candidate_id, std::string_view role, tier tier_)
    -> std::expected<void, registry_error> {
  auto stmt = conn.prepare("delete from routing_candidate_bindings "
                           "where candidate_id = ? and role = ? and tier = ?");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_int64(1, candidate_id) || !stmt->bind_text(2, nn(role)) || !stmt->bind_text(3, tier_to_text(tier_))) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->step()) {
    return std::unexpected(registry_error::query_failed);
  }
  return {};
}

auto observe(db::connection& conn, const observe_args& args) -> std::expected<std::int64_t, registry_error> {
  if (!valid_opaque_value(args.host_id) || !valid_opaque_value(args.evidence_ref) || args.observation_version <= 0 ||
      !bytes_less(args.captured_at, args.expires_at)) {
    return std::unexpected(registry_error::invalid_value);
  }
  auto stmt = conn.prepare("insert into routing_host_observations "
                           "(candidate_id, host_id, observation_version, availability, "
                           " spawn_verification, evidence_ref, captured_at, expires_at) "
                           "values (?, ?, ?, ?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(registry_error::query_failed);
  }
  if (!stmt->bind_int64(1, args.candidate_id) || !stmt->bind_text(2, nn(args.host_id)) ||
      !stmt->bind_int64(3, args.observation_version) || !stmt->bind_text(4, availability_to_text(args.availability_)) ||
      !stmt->bind_text(5, spawn_verification_to_text(args.spawn_verification_)) || !stmt->bind_text(6, nn(args.evidence_ref)) ||
      !stmt->bind_text(7, nn(args.captured_at)) || !stmt->bind_text(8, nn(args.expires_at))) {
    return std::unexpected(registry_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::unexpected(registry_error::conflict);
  }
  return stmt->column_int64(0);
}

auto list(db::connection& conn) -> std::expected<std::vector<candidate>, registry_error> {
  std::vector<registration> registrations;
  {
    auto stmt = conn.prepare(std::string(k_registration_columns) + " order by vendor, fallback_order, candidate_id");
    if (!stmt) {
      return std::unexpected(registry_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(registry_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      registrations.push_back(read_registration(*stmt));
    }
  }

  std::vector<candidate> out;
  out.reserve(registrations.size());
  for (auto& reg : registrations) {
    auto bindings = read_bindings(conn, reg.id);
    if (!bindings) {
      return std::unexpected(bindings.error());
    }
    auto observation = read_latest_observation(conn, reg.id);
    if (!observation) {
      return std::unexpected(observation.error());
    }
    out.push_back(candidate{
        .registration_      = std::move(reg),
        .bindings           = std::move(*bindings),
        .latest_observation = std::move(*observation),
    });
  }
  return out;
}

auto get_for_host(db::connection& conn, std::int64_t id, std::string_view host_id) -> std::expected<candidate, registry_error> {
  if (!valid_opaque_value(host_id)) {
    return std::unexpected(registry_error::invalid_value);
  }
  registration reg{};
  {
    auto stmt = conn.prepare(std::string(k_registration_columns) + " where id = ?");
    if (!stmt) {
      return std::unexpected(registry_error::query_failed);
    }
    if (!stmt->bind_int64(1, id)) {
      return std::unexpected(registry_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(registry_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      return std::unexpected(registry_error::not_found);
    }
    reg = read_registration(*stmt);
  }

  auto bindings = read_bindings(conn, id);
  if (!bindings) {
    return std::unexpected(bindings.error());
  }
  auto observation = read_latest_observation_for_host(conn, id, host_id);
  if (!observation) {
    return std::unexpected(observation.error());
  }
  return candidate{
      .registration_      = std::move(reg),
      .bindings           = std::move(*bindings),
      .latest_observation = std::move(*observation),
  };
}

auto eligibility::eligible() const -> bool {
  return cli_available && exact_spawn_verified && role_tier_bound && role_surface_override_supported && host_policy_permits &&
         observation_fresh;
}

auto eligibility_reason_to_text(eligibility_reason value) -> std::string_view {
  switch (value) {
  case eligibility_reason::provider_cli_unavailable:
    return "provider_cli_unavailable";
  case eligibility_reason::exact_spawn_unverified:
    return "exact_spawn_unverified";
  case eligibility_reason::role_tier_not_bound:
    return "role_tier_not_bound";
  case eligibility_reason::role_surface_override_unsupported:
    return "role_surface_override_unsupported";
  case eligibility_reason::host_policy_denied:
    return "host_policy_denied";
  case eligibility_reason::host_observation_expired:
    return "host_observation_expired";
  }
  return "";
}

auto reasons(const eligibility& gates) -> std::vector<eligibility_reason> {
  std::vector<eligibility_reason> out;
  if (!gates.cli_available) {
    out.push_back(eligibility_reason::provider_cli_unavailable);
  }
  if (!gates.exact_spawn_verified) {
    out.push_back(eligibility_reason::exact_spawn_unverified);
  }
  if (!gates.role_tier_bound) {
    out.push_back(eligibility_reason::role_tier_not_bound);
  }
  if (!gates.role_surface_override_supported) {
    out.push_back(eligibility_reason::role_surface_override_unsupported);
  }
  if (!gates.host_policy_permits) {
    out.push_back(eligibility_reason::host_policy_denied);
  }
  if (!gates.observation_fresh) {
    out.push_back(eligibility_reason::host_observation_expired);
  }
  return out;
}

auto evaluate_eligibility(const eligibility_input& input) -> eligibility {
  const bool has_observation = input.observation.has_value();
  return eligibility{
      .cli_available        = has_observation && input.observation->availability_ == availability::available,
      .exact_spawn_verified = has_observation && input.observation->spawn_verification_ == spawn_verification::verified,
      .role_tier_bound      = input.enabled && input.binding_present,
      .role_surface_override_supported = input.role_surface_override_supported,
      .host_policy_permits             = input.host_policy_permits,
      .observation_fresh               = has_observation && bytes_less(input.now, input.observation->expires_at),
  };
}

auto identity_verification_to_text(identity_verification value) -> std::string_view {
  switch (value) {
  case identity_verification::matched:
    return "matched";
  case identity_verification::missing_actual_identity:
    return "missing_actual_identity";
  case identity_verification::vendor_mismatch:
    return "vendor_mismatch";
  case identity_verification::candidate_mismatch:
    return "candidate_mismatch";
  }
  return "";
}

auto verify_actual_identity(std::string_view requested_vendor, std::string_view requested_candidate,
                            std::optional<std::string_view> actual_vendor, std::optional<std::string_view> actual_candidate)
    -> identity_verification {
  if (!actual_vendor.has_value() || !actual_candidate.has_value()) {
    return identity_verification::missing_actual_identity;
  }
  if (requested_vendor != *actual_vendor) {
    return identity_verification::vendor_mismatch;
  }
  if (requested_candidate != *actual_candidate) {
    return identity_verification::candidate_mismatch;
  }
  return identity_verification::matched;
}

} // namespace planar::engine::models::registry
