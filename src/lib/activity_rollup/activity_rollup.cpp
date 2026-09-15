/// @file activity_rollup.cpp
/// @brief Implementation of `planar.activity_rollup` (see activity_rollup.cppm).
///
/// The three queries, their ordering, the `coalesce` choices and the
/// no-actions-but-claims fallback are MOVED VERBATIM from
/// `engine/tree/walk.cpp`, where they were a deliberate documented duplicate
/// of the oracle's `summary.forEntity` (task 6282, decision D19). Only the
/// error enum and the namespace changed. Do not "tidy" the SQL here: these
/// bytes are the transcription, and `engine_tree`'s existing walk tests pin
/// the behaviour they produce.

module;

module planar.activity_rollup;

import std;
import planar.db;

namespace planar::activity_rollup {

/// @brief The oracle's `summary.forEntity`, transcribed.
///
/// Returns absent when the entity has NEITHER actions NOR claims. The
/// oracle deliberately checks the claim table for ANY row, not just an
/// active one: a released claim still represents activity worth surfacing.
///
/// Swallows query failures as "no activity" at the call site, not here —
/// the fold-in is additive and must never fail the whole walk.
auto for_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<std::optional<activity_summary>, rollup_error> {
  std::optional<std::string> action_kind;
  std::optional<std::string> vendor;
  std::optional<std::string> event_at;

  {
    auto stmt = conn.prepare("select action_kind, vendor, coalesce(ended_at, started_at) as event_at "
                             "from agent_actions where entity_kind = ? and entity_id = ? "
                             "order by event_at desc, id desc limit 1");
    if (!stmt) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (auto bound = stmt->bind_text(1, entity_kind); !bound) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(2, entity_id); !bound) {
      return std::unexpected(rollup_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (*stepped == db::step_result::row) {
      action_kind = stmt->column_text(0);
      vendor      = stmt->column_text(1);
      event_at    = stmt->column_text(2);
    }
  }

  std::int64_t active_count = 0;
  {
    auto stmt = conn.prepare("select count(*) from agent_work_claims "
                             "where entity_kind = ? and entity_id = ? and status = 'active' "
                             "and lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')");
    if (!stmt) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (auto bound = stmt->bind_text(1, entity_kind); !bound) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(2, entity_id); !bound) {
      return std::unexpected(rollup_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (*stepped == db::step_result::row) {
      active_count = stmt->column_int64(0);
    }
  }

  if (!action_kind.has_value()) {
    auto stmt = conn.prepare("select claim_token, vendor, coalesce(released_at, claimed_at) as event_at "
                             "from agent_work_claims where entity_kind = ? and entity_id = ? "
                             "order by event_at desc, id desc limit 1");
    if (!stmt) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (auto bound = stmt->bind_text(1, entity_kind); !bound) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(2, entity_id); !bound) {
      return std::unexpected(rollup_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(rollup_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      return std::optional<activity_summary>{}; // no actions and no claims
    }
    // Claim-only fallback: the oracle reports an EMPTY action kind here,
    // which the text renderer then prints as the literal word `claim`.
    action_kind = std::string{};
    vendor      = stmt->column_text(1);
    event_at    = stmt->column_text(2);
  }

  return std::optional<activity_summary>{activity_summary{
      .latest_action_kind = action_kind.value_or(std::string{}),
      .latest_vendor      = vendor.value_or(std::string{}),
      .last_event_at      = event_at.value_or(std::string{}),
      .active_claim_count = active_count,
  }};
}

/// @brief `for_entity` with query failures folded to "no activity".
auto activity_for(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id) -> std::optional<activity_summary> {
  auto got = for_entity(conn, entity_kind, entity_id);
  if (!got) {
    return std::nullopt;
  }
  return *got;
}

} // namespace planar::activity_rollup
