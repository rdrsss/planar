/// @file agentrender.cpp
/// @brief Implementation of `planar.engine.runtime.agentrender`. See the
/// module interface for the terminator contract and the field-order rule.

module planar.engine.runtime.agentrender;

import std;
import planar.json_text;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;

namespace planar::engine::runtime::agentrender {

namespace aa = agentactivity;

// The one shared escape table, layer 1. See json_text.cppm.
using json_text::append_json_string;

namespace {

/// @brief Append `"key":<string|null>`.
/// @param out The buffer.
/// @param key The key, unquoted.
/// @param value The value, or unset for `null`.
auto field_text(std::string& out, std::string_view key, const std::optional<std::string>& value) -> void {
  out.append(std::format("\"{}\":", key));
  if (value.has_value()) {
    append_json_string(out, *value);
  } else {
    out.append("null");
  }
}

/// @brief Append `"key":<int|null>`.
/// @param out The buffer.
/// @param key The key, unquoted.
/// @param value The value, or unset for `null`.
auto field_int(std::string& out, std::string_view key, const std::optional<std::int64_t>& value) -> void {
  if (value.has_value()) {
    out.append(std::format("\"{}\":{}", key, *value));
  } else {
    out.append(std::format("\"{}\":null", key));
  }
}

/// @brief Append `"key":"<token>"` for an optional enumerator, or `null`.
///
/// The tokens are drawn from closed CHECK sets, so they contain nothing
/// the escaper would touch — but they still go through it, because the
/// alternative is a second, un-audited path for emitting a JSON string.
/// @tparam T The enumerator type.
/// @param out The buffer.
/// @param key The key, unquoted.
/// @param value The value, or unset for `null`.
template <typename T> auto field_enum(std::string& out, std::string_view key, const std::optional<T>& value) -> void {
  out.append(std::format("\"{}\":", key));
  if (value.has_value()) {
    append_json_string(out, aa::to_text(*value));
  } else {
    out.append("null");
  }
}

} // namespace

// =========================================================================
// Fragments
// =========================================================================

auto append_claim(std::string& out, const aa::claim& value) -> void {
  append_claim_view(out, value, claim_view_extras{});
}

auto append_claim_view(std::string& out, const aa::claim& value, const claim_view_extras& extras) -> void {
  out.append(std::format("{{\"id\":{},\"claim_token\":", value.id));
  append_json_string(out, value.claim_token);
  out.append(std::format(",\"session_id\":{},\"entity_kind\":", value.session_id));
  append_json_string(out, aa::to_text(value.kind));
  out.append(std::format(",\"entity_id\":{}", value.entity_id));
  if (extras.entity_scope.has_value()) {
    out.append(",\"entity_scope\":{\"kind\":");
    append_json_string(out, extras.entity_scope->kind);
    out.append(",\"slug\":");
    if (extras.entity_scope->slug.has_value()) {
      append_json_string(out, *extras.entity_scope->slug);
    } else {
      out.append("null");
    }
    out.push_back('}');
  }
  out.append(",\"claim_scope\":");
  append_json_string(out, aa::to_text(value.scope));
  out.append(",\"status\":");
  append_json_string(out, aa::to_text(value.status));
  out.append(",\"vendor\":");
  append_json_string(out, value.vendor);
  out.push_back(',');
  field_text(out, "vendor_session_id", value.vendor_session_id);
  out.push_back(',');
  field_text(out, "role", value.role);
  out.push_back(',');
  field_text(out, "model", value.model);
  out.push_back(',');
  field_int(out, "worktree_id", value.worktree_id);
  out.push_back(',');
  field_text(out, "worktree_path", value.worktree_path);
  out.push_back(',');
  field_text(out, "repo_root", value.repo_root);
  out.push_back(',');
  field_text(out, "branch", value.branch);
  out.push_back(',');
  field_text(out, "head_sha_at_claim", value.head_sha_at_claim);
  out.push_back(',');
  field_enum(out, "dirty_at_claim", value.dirty_at_claim);
  out.push_back(',');
  field_text(out, "purpose", value.purpose);
  out.push_back(',');
  field_text(out, "base_ref", value.base_ref);
  out.append(",\"claimed_at\":");
  append_json_string(out, value.claimed_at);
  out.append(",\"last_heartbeat_at\":");
  append_json_string(out, value.last_heartbeat_at);
  out.append(",\"lease_expires_at\":");
  append_json_string(out, value.lease_expires_at);
  out.push_back(',');
  field_text(out, "released_at", value.released_at);
  out.push_back(',');
  field_text(out, "release_reason", value.release_reason);
  out.push_back(',');
  field_enum(out, "failure_category", value.category);
  out.push_back(',');
  field_int(out, "run_id", value.run_id);
  out.push_back(',');
  field_text(out, "stage", value.stage);
  if (extras.supervision.has_value()) {
    out.append(extras.supervision->engine ? ",\"supervisor\":\"engine\"," : ",\"supervisor\":\"caller\",");
    field_text(out, "attempt_id", extras.supervision->attempt_id);
  }
  if (extras.include_latest_action) {
    out.append(",\"latest_action\":");
    if (extras.latest_action.has_value()) {
      out.append("{\"kind\":");
      append_json_string(out, aa::to_text(extras.latest_action->kind));
      out.push_back(',');
      field_text(out, "summary", extras.latest_action->summary);
      out.append(",\"started_at\":");
      append_json_string(out, extras.latest_action->started_at);
      out.push_back('}');
    } else {
      out.append("null");
    }
  }
  out.push_back('}');
}

auto append_task(std::string& out, const aa::task_row& value) -> void {
  out.append(std::format("{{\"id\":{},\"scope_kind\":", value.id));
  append_json_string(out, value.scope_kind);
  out.push_back(',');
  field_int(out, "scope_id", value.scope_id);
  out.push_back(',');
  field_int(out, "plan_id", value.plan_id);
  out.push_back(',');
  field_int(out, "parent_task_id", value.parent_task_id);
  out.append(",\"title\":");
  append_json_string(out, value.title);
  out.push_back(',');
  field_text(out, "body", value.body);
  out.push_back(',');
  field_text(out, "slug", value.slug);
  out.append(",\"status\":");
  append_json_string(out, value.status);
  out.append(std::format(",\"priority\":{},", value.priority));
  field_text(out, "next_action", value.next_action);
  out.push_back(',');
  field_text(out, "due_at", value.due_at);
  out.append(",\"created_at\":");
  append_json_string(out, value.created_at);
  out.append(",\"updated_at\":");
  append_json_string(out, value.updated_at);
  out.push_back('}');
}

auto append_action(std::string& out, const aa::action& value) -> void {
  out.append(std::format("{{\"id\":{},\"session_id\":{},", value.id, value.session_id));
  field_int(out, "session_entry_id", value.session_entry_id);
  out.push_back(',');
  field_int(out, "parent_action_id", value.parent_action_id);
  out.push_back(',');
  field_int(out, "claim_id", value.claim_id);
  out.append(",\"action_kind\":");
  append_json_string(out, aa::to_text(value.kind));
  out.push_back(',');
  field_enum(out, "entity_kind", value.entity);
  out.push_back(',');
  field_int(out, "entity_id", value.entity_id);
  out.append(",\"vendor\":");
  append_json_string(out, value.vendor);
  out.push_back(',');
  field_text(out, "vendor_role", value.vendor_role);
  out.push_back(',');
  field_text(out, "model", value.model);
  out.append(",\"started_at\":");
  append_json_string(out, value.started_at);
  out.push_back(',');
  field_text(out, "ended_at", value.ended_at);
  out.push_back(',');
  field_enum(out, "outcome", value.result);
  out.push_back(',');
  field_text(out, "summary", value.summary);
  out.push_back(',');
  field_text(out, "head_sha", value.head_sha);
  out.push_back(',');
  field_enum(out, "dirty", value.dirty);
  out.push_back(',');
  field_text(out, "metadata", value.metadata);
  out.push_back('}');
}

// =========================================================================
// pull / peek
// =========================================================================

auto no_work_json() -> std::string {
  return "{\"ok\":true,\"no_work\":true}\n";
}

auto no_work_text() -> std::string {
  return "no_work\n";
}

auto pull_json(const agentatomic::pull_result& result, const aa::task_row& task) -> std::string {
  std::string out = "{\"ok\":true,\"no_work\":false,\"claim_token\":";
  append_json_string(out, result.acquired->claim_token);
  out.append(",\"claim\":");
  append_claim(out, *result.acquired);
  out.append(",\"task\":");
  append_task(out, task);
  out.append(std::format(",\"action_id\":{}}}\n", result.action_id));
  return out;
}

auto pull_text(const agentatomic::pull_result& result) -> std::string {
  return std::format("pulled task:{} claim:{} action:{}\n", result.task_id, result.acquired->claim_token, result.action_id);
}

auto peek_json(const aa::task_row& task) -> std::string {
  std::string out = "{\"ok\":true,\"no_work\":false,\"task\":";
  append_task(out, task);
  out.append("}\n");
  return out;
}

auto peek_text(const aa::task_row& task) -> std::string {
  return std::format("next: task:{} status:{}\n", task.id, task.status);
}

// =========================================================================
// claim / heartbeat
// =========================================================================

auto claim_json(const aa::claim& value) -> std::string {
  std::string out = "{\"ok\":true,\"claim_token\":";
  append_json_string(out, value.claim_token);
  out.append(",\"claim\":");
  append_claim(out, value);
  out.append("}\n");
  return out;
}

auto claim_text(const aa::claim& value) -> std::string {
  return std::format("claim:{} entity:{}:{} status:{}\n", value.claim_token, aa::to_text(value.kind), value.entity_id,
                     aa::to_text(value.status));
}

auto heartbeat_text(const aa::claim& value) -> std::string {
  return std::format("ok claim:{} expires:{}\n", value.claim_token, value.lease_expires_at);
}

// =========================================================================
// terminal verbs
// =========================================================================

auto terminal_json(const agentatomic::terminal_result& result, const aa::task_row& task, std::string_view trailing_fields)
    -> std::string {
  std::string out = "{\"ok\":true,\"claim_token\":";
  append_json_string(out, result.released.claim_token);
  out.append(",\"claim\":");
  append_claim(out, result.released);
  out.append(",\"task\":");
  append_task(out, task);
  out.append(trailing_fields);
  out.append("}\n");
  return out;
}

auto terminal_json(const agentatomic::terminal_result& result, std::string_view trailing_fields) -> std::string {
  std::string out = "{\"ok\":true,\"claim_token\":";
  append_json_string(out, result.released.claim_token);
  out.append(",\"claim\":");
  append_claim(out, result.released);
  out.append(",\"task\":null");
  out.append(trailing_fields);
  out.append("}\n");
  return out;
}

auto terminal_text(const agentatomic::terminal_result& result) -> std::string {
  return std::format("ok entity:{}:{} claim_status:{}\n", aa::to_text(result.released.kind), result.released.entity_id,
                     aa::to_text(result.released.status));
}

auto terminal_text(const agentatomic::terminal_result& result, const aa::task_row& task) -> std::string {
  return std::format("ok task:{} status:{} claim_status:{}\n", task.id, task.status, aa::to_text(result.released.status));
}

// =========================================================================
// abort / claim-associate
// =========================================================================

auto abort_json(const aa::claim& value, std::int64_t aborting_session) -> std::string {
  std::string out = "{\"ok\":true,\"claim_token\":";
  append_json_string(out, value.claim_token);
  out.append(",\"claim\":");
  append_claim(out, value);
  out.append(std::format(",\"aborting_session\":{}}}\n", aborting_session));
  return out;
}

auto abort_text(const aa::claim& value, std::int64_t aborting_session) -> std::string {
  return std::format("aborted claim:{} by session:{}\n", value.claim_token, aborting_session);
}

auto associate_json(std::int64_t updated) -> std::string {
  return std::format("{{\"ok\":true,\"updated\":{}}}\n", updated);
}

auto associate_text(std::int64_t updated, std::string_view claim_token) -> std::string {
  return std::format("ok updated:{} claim:{}\n", updated, claim_token);
}

auto supervisor_json(const agentatomic::associate_result& result, std::optional<std::int64_t> run_updated) -> std::string {
  std::string out = "{\"ok\":true,\"claim_token\":";
  append_json_string(out, result.held.claim_token);
  out += result.engine ? ",\"supervisor\":\"engine\",\"attempt_id\":" : ",\"supervisor\":\"caller\",\"attempt_id\":";
  if (result.attempt_id.has_value()) {
    append_json_string(out, *result.attempt_id);
  } else {
    out += "null";
  }
  out += std::format(",\"changed\":{}", result.changed ? "true" : "false");
  if (run_updated.has_value()) {
    out += std::format(",\"updated\":{}", *run_updated);
  }
  out += "}\n";
  return out;
}

auto supervisor_text(const agentatomic::associate_result& result, std::optional<std::int64_t> run_updated) -> std::string {
  auto out = std::format("ok claim:{} supervisor:{} attempt:{} changed:{}", result.held.claim_token,
                         result.engine ? "engine" : "caller", result.attempt_id.value_or("-"), result.changed ? "true" : "false");
  if (run_updated.has_value()) {
    out += std::format(" updated:{}", *run_updated);
  }
  out += "\n";
  return out;
}

// =========================================================================
// action start / end
// =========================================================================

auto action_json(const aa::action& value) -> std::string {
  std::string out = std::format("{{\"ok\":true,\"action_id\":{},\"action\":", value.id);
  append_action(out, value);
  out.append("}\n");
  return out;
}

auto action_start_text(const aa::action& value, std::string_view claim_token) -> std::string {
  return std::format("action:{} kind:{} claim:{}\n", value.id, aa::to_text(value.kind), claim_token);
}

auto action_end_text(std::int64_t action_id, aa::outcome result) -> std::string {
  return std::format("ok action:{} outcome:{}\n", action_id, aa::to_text(result));
}

// =========================================================================
// reconcile
// =========================================================================

auto reconcile_json(const aa::reconcile_result& result, const aa::reconcile_runs_result& runs, bool dry_run) -> std::string {
  std::string out = std::format("{{\"ok\":true,\"claims_marked_stale\":{},\"actions_closed\":{},\"runs_abandoned\":{}",
                                result.claims_marked_stale, result.actions_closed, runs.abandoned);
  if (dry_run) {
    out.append(",\"candidates\":[");
    for (std::size_t i = 0; i < result.candidates.size(); ++i) {
      if (i > 0) {
        out.push_back(',');
      }
      auto const& candidate = result.candidates[i];
      out.append("{\"kind\":");
      append_json_string(out, aa::to_text(candidate.kind));
      out.append(std::format(",\"id\":{},\"claim\":", candidate.entity_id));
      append_claim(out, candidate);
      out.push_back('}');
    }
    out.append("],\"run_candidates\":[");
    for (std::size_t i = 0; i < runs.candidates.size(); ++i) {
      if (i > 0) {
        out.push_back(',');
      }
      auto const& candidate = runs.candidates[i];
      // UNESCAPED, deliberately — see reconcile_json's doc comment. The
      // oracle's format string is a bare `"{s}"` here and nowhere else.
      out.append(std::format("{{\"id\":{},\"run_identifier\":\"{}\",\"pid\":{},\"plan_id\":{}}}", candidate.id,
                             candidate.run_identifier, candidate.pid, candidate.plan_id.value_or(0)));
    }
    out.push_back(']');
  }
  // Engine-supervised claims and `centurion` runs the sweep left alone
  // (plan 1033 task 6489). Emitted only when there are any, so a
  // caller-only sweep prints exactly the bytes it always did.
  if (!result.skipped_engine.empty() || !runs.skipped_engine.empty()) {
    out.append(",\"skipped_engine_claims\":[");
    for (std::size_t i = 0; i < result.skipped_engine.size(); ++i) {
      if (i > 0) {
        out.push_back(',');
      }
      auto const& skipped = result.skipped_engine[i];
      out.append("{\"kind\":");
      append_json_string(out, aa::to_text(skipped.kind));
      out.append(std::format(",\"id\":{},\"claim_token\":", skipped.entity_id));
      append_json_string(out, skipped.claim_token);
      out.push_back('}');
    }
    out.append("],\"skipped_engine_runs\":[");
    for (std::size_t i = 0; i < runs.skipped_engine.size(); ++i) {
      if (i > 0) {
        out.push_back(',');
      }
      auto const& skipped = runs.skipped_engine[i];
      out.append(std::format("{{\"id\":{},\"run_identifier\":", skipped.id));
      append_json_string(out, skipped.run_identifier);
      out.append(std::format(",\"pid\":{}}}", skipped.pid));
    }
    out.push_back(']');
  }
  out.append("}\n");
  return out;
}

namespace {

/// @brief The engine-supervised lines `reconcile_text` appends, or nothing.
/// @param result The claim sweep.
/// @param runs The run sweep.
/// @return The lines, newline-terminated; empty when nothing was skipped.
auto skipped_text(const aa::reconcile_result& result, const aa::reconcile_runs_result& runs) -> std::string {
  if (result.skipped_engine.empty() && runs.skipped_engine.empty()) {
    return {};
  }
  std::string out = std::format("skipped (engine-supervised; --override-supervisor to take over): {} claim(s), {} run(s)\n",
                                result.skipped_engine.size(), runs.skipped_engine.size());
  for (auto const& skipped : result.skipped_engine) {
    out.append(std::format("  {}:{} token:{}\n", aa::to_text(skipped.kind), skipped.entity_id, skipped.claim_token));
  }
  for (auto const& skipped : runs.skipped_engine) {
    out.append(std::format("  run:{} pid:{} identifier:{}\n", skipped.id, skipped.pid, skipped.run_identifier));
  }
  return out;
}

} // namespace

auto reconcile_text(const aa::reconcile_result& result, const aa::reconcile_runs_result& runs, bool dry_run) -> std::string {
  if (!dry_run) {
    return std::format("reconciled: {} claim(s) stale, {} action(s) closed, {} run(s) abandoned\n", result.claims_marked_stale,
                       result.actions_closed, runs.abandoned) +
           skipped_text(result, runs);
  }
  std::string out =
      std::format("dry-run: {} claim candidate(s), {} run candidate(s)\n", result.candidates.size(), runs.candidates.size());
  for (auto const& candidate : result.candidates) {
    out.append(std::format("  {}:{} token:{}\n", aa::to_text(candidate.kind), candidate.entity_id, candidate.claim_token));
  }
  for (auto const& candidate : runs.candidates) {
    out.append(std::format("  run:{} pid:{} identifier:{}\n", candidate.id, candidate.pid, candidate.run_identifier));
  }
  return out + skipped_text(result, runs);
}

} // namespace planar::engine::runtime::agentrender
