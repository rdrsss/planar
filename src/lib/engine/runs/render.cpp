/// @file render.cpp
/// @brief Implementation of `planar.engine.runs.render` (plan 996, task
/// 6095). See render.cppm for scope and the oracle-derived output shapes.

module;

#include <glaze/json/read.hpp>

module planar.engine.runs.render;

import std;
import planar.json_text;
import planar.engine.runs.lifecycle;

namespace planar::engine::runs::render {

// The one shared escape table, layer 1. See json_text.cppm.
using json_text::append_json_string;

namespace {

/// @brief Append a stored raw-JSON blob verbatim, or the literal `null`.
///
/// The blob is NOT re-escaped and NOT re-formatted: whatever bytes the column
/// holds are spliced straight into the enclosing object. See render.cppm's
/// "Raw-JSON embedding" section for the whitespace round-trip capture that
/// makes this observable.
auto append_raw_json_or_null(std::string& out, const std::optional<std::string>& blob) -> void {
  if (blob.has_value()) {
    out.append(*blob);
  } else {
    out.append("null");
  }
}

auto append_string_or_null(std::string& out, const std::optional<std::string>& value) -> void {
  if (value.has_value()) {
    append_json_string(out, *value);
  } else {
    out.append("null");
  }
}

/// @brief Append the `events` array, shared verbatim between `bench show` and
/// `run show` — the one part of the two JSON renderers that IS identical.
auto append_events_array(std::string& out, std::span<const lifecycle::event_row> events) -> void {
  out.append(",\"events\":[");
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    const auto& ev = events[i];
    out.append(std::format("{{\"id\":{},\"seq\":{},\"kind\":", ev.id, ev.seq));
    append_json_string(out, ev.kind);
    out.append(",\"payload\":");
    append_raw_json_or_null(out, ev.payload);
    out.append(",\"created_at\":");
    append_json_string(out, ev.created_at);
    out.push_back('}');
  }
  out.push_back(']');
}

/// @brief Append the shared `events (N):` text section.
///
/// The blank line before the header, the parenthesised count, and the
/// `  [seq] kind` / `  [seq] kind: payload` row forms are all identical
/// between the two show leaves.
auto append_events_text(std::string& out, std::span<const lifecycle::event_row> events) -> void {
  out.append(std::format("\nevents ({}):\n", events.size()));
  for (const auto& ev : events) {
    if (ev.payload.has_value()) {
      out.append(std::format("  [{}] {}: {}\n", ev.seq, ev.kind, *ev.payload));
    } else {
      out.append(std::format("  [{}] {}\n", ev.seq, ev.kind));
    }
  }
}

} // namespace

auto is_known_arm(std::string_view arm) -> bool {
  return std::ranges::find(k_known_arms, arm) != k_known_arms.end();
}

auto is_valid_terminal_status(std::string_view status) -> bool {
  return std::ranges::find(k_terminal_statuses, status) != k_terminal_statuses.end();
}

auto is_valid_json_payload(std::string_view blob) -> bool {
  // `glz::validate_json` (not a bare `glz::read_json`) is the purpose-built
  // probe that enables Glaze's `validate_trailing_whitespace` — without it
  // `{"a":1} junk` reads the object and silently ignores the tail, which the
  // oracle refuses at exit 2. Same reasoning and same call as
  // engine/config/templates.cpp's `is_valid_json` (task 6086).
  return !glz::validate_json(blob);
}

auto render_unknown_arm_warning(std::string_view arm) -> std::string {
  return std::format("warn: bench start: unrecognized arm '{}'; recognized arms: strict, eligibility, grouped", arm);
}

auto render_run_not_found(std::string_view leaf, std::string_view run_uid) -> std::string {
  return std::format("{}: run '{}' not found", leaf, run_uid);
}

auto render_invalid_status(std::string_view leaf, std::string_view status) -> std::string {
  return std::format("{}: invalid --status '{}'; expected completed, aborted, or error", leaf, status);
}

auto render_invalid_touch_kind(std::string_view kind) -> std::string {
  return std::format("bench touch: invalid --kind '{}'; expected declared or actual", kind);
}

auto render_invalid_json(std::string_view leaf, std::string_view flag, std::string_view blob) -> std::string {
  return std::format("{}: {} is not valid JSON: {}", leaf, flag, blob);
}

auto render_duplicate_run_uid(std::string_view run_uid) -> std::string {
  return std::format("bench start: run_uid '{}' already exists", run_uid);
}

auto render_duplicate_seq(std::int64_t seq, std::string_view run_uid) -> std::string {
  return std::format("bench event: seq {} already used for run '{}'", seq, run_uid);
}

auto render_bench_show_json(const lifecycle::run& run_, std::span<const lifecycle::event_row> events,
                            std::span<const lifecycle::touch_row> touches) -> std::string {
  std::string out;
  out.append(std::format("{{\"id\":{},\"run_uid\":", run_.id));
  append_json_string(out, run_.run_uid);
  out.append(std::format(",\"plan_id\":{},\"arm\":", run_.plan_id));
  append_json_string(out, run_.arm);
  out.append(",\"base_sha\":");
  append_json_string(out, run_.base_sha);
  out.append(",\"config_hash\":");
  append_json_string(out, run_.config_hash);
  out.append(",\"config_json\":");
  append_raw_json_or_null(out, run_.config_json);
  out.append(",\"corpus_repo\":");
  append_string_or_null(out, run_.corpus_repo);
  out.append(",\"status\":");
  append_json_string(out, run_.status);
  out.append(",\"started_at\":");
  append_json_string(out, run_.started_at);
  out.append(",\"ended_at\":");
  append_string_or_null(out, run_.ended_at);

  append_events_array(out, events);

  out.append(",\"touches\":[");
  for (std::size_t i = 0; i < touches.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    const auto& t = touches[i];
    out.append(std::format("{{\"id\":{},\"task_id\":{},\"path\":", t.id, t.task_id));
    append_json_string(out, t.path);
    out.append(",\"kind\":");
    append_json_string(out, lifecycle::touch_kind_to_text(t.kind_));
    out.append(",\"created_at\":");
    append_json_string(out, t.created_at);
    out.push_back('}');
  }
  out.push_back(']');

  out.append("}\n");
  return out;
}

auto render_bench_show_text(const lifecycle::run& run_, std::span<const lifecycle::event_row> events,
                            std::span<const lifecycle::touch_row> touches) -> std::string {
  std::string out;
  out.append(std::format("run:         {}\n", run_.run_uid));
  out.append(std::format("plan_id:     {}\n", run_.plan_id));
  out.append(std::format("arm:         {}\n", run_.arm));
  out.append(std::format("status:      {}\n", run_.status));
  out.append(std::format("base_sha:    {}\n", run_.base_sha));
  out.append(std::format("config_hash: {}\n", run_.config_hash));
  // corpus_repo and ended_at rows are omitted entirely when unset — NOT
  // rendered with an empty value. Note config_json has no text row at all.
  if (run_.corpus_repo.has_value()) {
    out.append(std::format("corpus_repo: {}\n", *run_.corpus_repo));
  }
  out.append(std::format("started_at:  {}\n", run_.started_at));
  if (run_.ended_at.has_value()) {
    out.append(std::format("ended_at:    {}\n", *run_.ended_at));
  }

  append_events_text(out, events);

  out.append(std::format("\ntouches ({}):\n", touches.size()));
  for (const auto& t : touches) {
    out.append(std::format("  task={} path={} kind={}\n", t.task_id, t.path, lifecycle::touch_kind_to_text(t.kind_)));
  }
  return out;
}

auto render_run_show_json(const lifecycle::run& run_, std::span<const lifecycle::event_row> events) -> std::string {
  std::string out;
  out.append(std::format("{{\"id\":{},\"run_uid\":", run_.id));
  append_json_string(out, run_.run_uid);
  out.append(std::format(",\"plan_id\":{},\"arm\":", run_.plan_id));
  append_json_string(out, run_.arm);
  out.append(",\"status\":");
  append_json_string(out, run_.status);
  out.append(",\"started_at\":");
  append_json_string(out, run_.started_at);
  out.append(",\"ended_at\":");
  append_string_or_null(out, run_.ended_at);

  append_events_array(out, events);

  out.append("}\n");
  return out;
}

auto render_run_show_text(const lifecycle::run& run_, std::span<const lifecycle::event_row> events) -> std::string {
  std::string out;
  // Width 12, not bench's 13 — `started_at: ` is the longest label here.
  out.append(std::format("run:        {}\n", run_.run_uid));
  out.append(std::format("plan_id:    {}\n", run_.plan_id));
  out.append(std::format("arm:        {}\n", run_.arm));
  out.append(std::format("status:     {}\n", run_.status));
  out.append(std::format("started_at: {}\n", run_.started_at));
  if (run_.ended_at.has_value()) {
    out.append(std::format("ended_at:   {}\n", *run_.ended_at));
  }

  append_events_text(out, events);
  return out;
}

auto render_bench_start(std::string_view run_uid) -> std::string {
  return std::format("{}\n", run_uid);
}

auto render_bench_ok() -> std::string {
  return "ok\n";
}

auto render_run_start_json(std::string_view run_uid, std::int64_t plan_id, std::string_view arm) -> std::string {
  std::string out{"{\"run_uid\":"};
  append_json_string(out, run_uid);
  out.append(std::format(",\"plan_id\":{},\"arm\":", plan_id));
  append_json_string(out, arm);
  out.append("}\n");
  return out;
}

auto render_run_event_json(std::string_view run_uid, std::int64_t seq, std::string_view kind) -> std::string {
  std::string out{"{\"run_uid\":"};
  append_json_string(out, run_uid);
  out.append(std::format(",\"seq\":{},\"kind\":", seq));
  append_json_string(out, kind);
  out.append("}\n");
  return out;
}

auto render_run_finish_json(std::string_view run_uid, std::string_view status) -> std::string {
  std::string out{"{\"run_uid\":"};
  append_json_string(out, run_uid);
  out.append(",\"status\":");
  append_json_string(out, status);
  out.append("}\n");
  return out;
}

} // namespace planar::engine::runs::render
