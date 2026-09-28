/// @file routing.cpp
/// @brief SQLite implementation of immutable preview/confirm dispatch authorization.
module planar.engine.routing;
import std;
import planar.db;

namespace planar::engine::routing {
namespace {
auto bind_text(db::statement& s, int& i, std::string_view value) -> bool {
  return s.bind_text(i++, value).has_value();
}
auto bind_opt_text(db::statement& s, int& i, const std::optional<std::string>& value) -> bool {
  return value ? s.bind_text(i++, *value).has_value() : s.bind_null(i++).has_value();
}
auto bind_opt_int(db::statement& s, int& i, const std::optional<std::int64_t>& value) -> bool {
  return value ? s.bind_int64(i++, *value).has_value() : s.bind_null(i++).has_value();
}
auto same(std::string_view a, std::string_view b) -> bool {
  return a == b;
}
auto same_opt(const std::optional<std::string>& a, const std::optional<std::string>& b) -> bool {
  return a == b;
}
auto read_opt(const db::statement& s, int index) -> std::optional<std::string> {
  return s.is_null(index) ? std::nullopt : std::optional{s.column_text(index)};
}
} // namespace
auto classify_stale(const binding& frozen, const binding& current, std::string_view expires, bool consumed, std::string_view now)
    -> std::optional<stale_reason> {
  if (consumed)
    return stale_reason::already_consumed;
  if (now >= expires)
    return stale_reason::expired;
  if (!same(frozen.packet_digest, current.packet_digest))
    return stale_reason::packet_changed;
  if (!same(frozen.profile_digest, current.profile_digest))
    return stale_reason::profile_changed;
  if (!same(frozen.policy_digest, current.policy_digest))
    return stale_reason::policy_changed;
  if (!same(frozen.capability_digest, current.capability_digest))
    return stale_reason::capability_changed;
  if (!same(frozen.vendor, current.vendor) || !same(frozen.role, current.role) || !same(frozen.tier, current.tier) ||
      !same(frozen.work_type, current.work_type) || !same(frozen.complexity, current.complexity) ||
      !same(frozen.validation_policy, current.validation_policy) || !same(frozen.routing_policy, current.routing_policy))
    return stale_reason::cohort_changed;
  if (frozen.candidate_id != current.candidate_id)
    return stale_reason::candidate_changed;
  if (!same_opt(frozen.claim, current.claim) || !same_opt(frozen.claim_status, current.claim_status))
    return stale_reason::claim_changed;
  return std::nullopt;
}
auto valid(std::string_view value) -> bool {
  return !value.empty() && std::ranges::none_of(value, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
auto error_name(error value) -> std::string_view {
  switch (value) {
  case error::invalid_value:
    return "InvalidValue";
  case error::unknown_preview:
    return "UnknownPreview";
  case error::stale_preview:
    return "StalePreview";
  case error::query_failed:
    return "QueryFailed";
  }
  return "QueryFailed";
}
auto stale_name(stale_reason value) -> std::string_view {
  switch (value) {
  case stale_reason::expired:
    return "expired";
  case stale_reason::already_consumed:
    return "already_consumed";
  case stale_reason::packet_changed:
    return "packet_changed";
  case stale_reason::profile_changed:
    return "profile_changed";
  case stale_reason::policy_changed:
    return "policy_changed";
  case stale_reason::capability_changed:
    return "capability_changed";
  case stale_reason::cohort_changed:
    return "cohort_changed";
  case stale_reason::claim_changed:
    return "claim_changed";
  case stale_reason::candidate_changed:
    return "candidate_changed";
  }
  return "unknown";
}
auto preview(db::connection& c, const binding& b, std::string_view expires) -> std::expected<preview_result, error> {
  if (!valid(expires) || !valid(b.work_item) || !valid(b.vendor) || !valid(b.role) || !valid(b.host))
    return std::unexpected(error::invalid_value);
  auto s = c.prepare(
      "insert into routing_dispatch_previews "
      "(preview_token,task_id,logical_work_item_id,project_id,validation_policy_version,routing_policy_version,profile_rule_"
      "version,vendor,role,tier,work_type,complexity,packet_digest,profile_digest,policy_digest,capability_digest,requested_"
      "candidate_id,host_id,assignment_class,experiment_id,claim_token,claim_status_at_preview,evidence_state,expires_at) values "
      "(lower(hex(randomblob(16))),?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) returning id,preview_token");
  if (!s)
    return std::unexpected(error::query_failed);
  int i = 1;
  if (!bind_opt_int(*s, i, b.task_id) || !bind_text(*s, i, b.work_item) || !s->bind_int64(i++, b.project_id) ||
      !bind_text(*s, i, b.validation_policy) || !bind_text(*s, i, b.routing_policy) || !bind_text(*s, i, b.profile_rule) ||
      !bind_text(*s, i, b.vendor) || !bind_text(*s, i, b.role) || !bind_text(*s, i, b.tier) || !bind_text(*s, i, b.work_type) ||
      !bind_text(*s, i, b.complexity) || !bind_text(*s, i, b.packet_digest) || !bind_text(*s, i, b.profile_digest) ||
      !bind_text(*s, i, b.policy_digest) || !bind_text(*s, i, b.capability_digest) || !s->bind_int64(i++, b.candidate_id) ||
      !bind_text(*s, i, b.host) || !bind_text(*s, i, b.assignment_class) || !bind_opt_int(*s, i, b.experiment_id) ||
      !bind_opt_text(*s, i, b.claim) || !bind_opt_text(*s, i, b.claim_status) || !bind_text(*s, i, b.evidence_state) ||
      !bind_text(*s, i, expires))
    return std::unexpected(error::query_failed);
  auto step = s->step();
  if (!step || *step != db::step_result::row)
    return std::unexpected(error::query_failed);
  return preview_result{.id = s->column_int64(0), .token = s->column_text(1)};
}
auto confirm(db::connection& c, std::string_view token, std::string_view key, const binding& current, std::string_view now,
             std::string_view reviewer, std::string_view decision, stale_reason* stale) -> std::expected<confirm_result, error> {
  if (!valid(token) || !valid(key) || !valid(now))
    return std::unexpected(error::invalid_value);
  auto tx = c.begin_transaction(db::lock_mode::immediate);
  if (!tx)
    return std::unexpected(error::query_failed);
  auto s = c.prepare("select "
                     "id,task_id,logical_work_item_id,project_id,validation_policy_version,routing_policy_version,profile_rule_"
                     "version,vendor,role,tier,work_type,complexity,packet_digest,profile_digest,policy_digest,capability_digest,"
                     "requested_candidate_id,host_id,assignment_class,experiment_id,claim_token,claim_status_at_preview,evidence_"
                     "state,expires_at,consumed_at is not null from routing_dispatch_previews where preview_token=?");
  if (!s || !s->bind_text(1, token))
    return std::unexpected(error::query_failed);
  auto step = s->step();
  if (!step)
    return std::unexpected(error::query_failed);
  if (*step == db::step_result::done)
    return std::unexpected(error::unknown_preview);
  binding    b{.task_id           = s->is_null(1) ? std::nullopt : std::optional{s->column_int64(1)},
               .work_item         = s->column_text(2),
               .validation_policy = s->column_text(4),
               .routing_policy    = s->column_text(5),
               .profile_rule      = s->column_text(6),
               .vendor            = s->column_text(7),
               .role              = s->column_text(8),
               .tier              = s->column_text(9),
               .work_type         = s->column_text(10),
               .complexity        = s->column_text(11),
               .project_id        = s->column_int64(3),
               .candidate_id      = s->column_int64(16),
               .packet_digest     = s->column_text(12),
               .profile_digest    = s->column_text(13),
               .policy_digest     = s->column_text(14),
               .capability_digest = s->column_text(15),
               .host              = s->column_text(17),
               .assignment_class  = s->column_text(18),
               .evidence_state    = s->column_text(22),
               .experiment_id     = s->is_null(19) ? std::nullopt : std::optional{s->column_int64(19)},
               .claim             = read_opt(*s, 20),
               .claim_status      = read_opt(*s, 21)};
  auto const preview_id = s->column_int64(0);
  if (auto why = classify_stale(b, current, s->column_text(23), s->column_int64(24) != 0, now)) {
    if (stale)
      *stale = *why;
    return std::unexpected(error::stale_preview);
  }
  auto selected_done = s->step();
  if (!selected_done || *selected_done != db::step_result::done)
    return std::unexpected(error::query_failed);
  auto out = c.prepare("insert into routing_dispatch_snapshots "
                       "(dispatch_key,task_id,logical_work_item_id,project_id,validation_policy_version,routing_policy_version,"
                       "profile_rule_version,vendor,role,tier,work_type,complexity,packet_digest,policy_digest,capability_digest,"
                       "requested_candidate_id,assignment_class,operator_decision,reviewer_disposition,terminal_state,confirmed_"
                       "at) values (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'pending',?) returning id");
  if (!out)
    return std::unexpected(error::query_failed);
  int i = 1;
  if (!bind_text(*out, i, key) || !bind_opt_int(*out, i, b.task_id) || !bind_text(*out, i, b.work_item) ||
      !out->bind_int64(i++, b.project_id) || !bind_text(*out, i, b.validation_policy) || !bind_text(*out, i, b.routing_policy) ||
      !bind_text(*out, i, b.profile_rule) || !bind_text(*out, i, b.vendor) || !bind_text(*out, i, b.role) ||
      !bind_text(*out, i, b.tier) || !bind_text(*out, i, b.work_type) || !bind_text(*out, i, b.complexity) ||
      !bind_text(*out, i, b.packet_digest) || !bind_text(*out, i, b.policy_digest) || !bind_text(*out, i, b.capability_digest) ||
      !out->bind_int64(i++, b.candidate_id) || !bind_text(*out, i, b.assignment_class) || !bind_text(*out, i, decision) ||
      !bind_text(*out, i, reviewer) || !bind_text(*out, i, now))
    return std::unexpected(error::query_failed);
  auto inserted = out->step();
  if (!inserted || *inserted != db::step_result::row)
    return std::unexpected(error::query_failed);
  auto const dispatch_id = out->column_int64(0);
  auto       finished    = out->step();
  if (!finished || *finished != db::step_result::done)
    return std::unexpected(error::query_failed);
  auto consume =
      c.prepare("update routing_dispatch_previews set consumed_at=?, consumed_dispatch_id=? where id=? and consumed_at is null");
  if (!consume || !consume->bind_text(1, now) || !consume->bind_int64(2, dispatch_id) || !consume->bind_int64(3, preview_id) ||
      !consume->step())
    return std::unexpected(error::query_failed);
  auto changed = c.prepare("select changes()");
  if (!changed || !changed->step() || changed->column_int64(0) != 1) {
    if (stale)
      *stale = stale_reason::already_consumed;
    return std::unexpected(error::stale_preview);
  }
  if (!tx->commit())
    return std::unexpected(error::query_failed);
  return confirm_result{.dispatch_id = dispatch_id};
}
} // namespace planar::engine::routing
