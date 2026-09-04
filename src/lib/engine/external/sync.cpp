/// @file sync.cpp
/// @brief Implementation of `planar.engine.external.sync`. See sync.cppm for
/// the derived conflict rule, the outcome-vs-error split, and the four CAS
/// guards.

module;

#include "sha256.hpp"

#include <glaze/glaze.hpp>

module planar.engine.external.sync;

import std;
import planar.db;
import planar.adapter;
import planar.json_text;
import planar.engine.external.link;

namespace planar::engine::external::sync {

namespace {

/// @brief The `tasks`/`plans`/`questions`/`artifacts` triple a link syncs.
struct entity_fields {
  std::string title;      ///< The entity title.
  std::string status;     ///< The entity status.
  std::string updated_at; ///< The entity's last-modified stamp; the CAS key.
};

/// @brief The table backing one link's entity kind.
///
/// Only FOUR of the seven `external_entity_kind` values have a
/// title/status/updated_at triple; the other three
/// (`test_scenario`/`decision`/`session`) are `unsupported_entity_kind`. The
/// Zig original's switch has exactly these four arms.
///
/// Still used for READS only, after decision 996 (task 6419) removed this
/// module's one WRITE use (`apply_remote_to_local`'s `update {}` — see
/// `local_diff`'s header). `local_entity_fields` below reads through this
/// on every path, including from `planar-ext`, whose write authorizer
/// restricts INSERT/UPDATE/DELETE, not SELECT.
/// @param kind The link's entity kind.
/// @return The table name, or unset when the kind has no syncable triple.
auto table_for(link::external_entity_kind kind) -> std::optional<std::string_view> {
  switch (kind) {
  case link::external_entity_kind::task:
    return "tasks";
  case link::external_entity_kind::plan:
    return "plans";
  case link::external_entity_kind::question:
    return "questions";
  case link::external_entity_kind::artifact:
    return "artifacts";
  default:
    return std::nullopt;
  }
}

/// @brief Read one entity's title/status/updated_at.
/// @param conn The connection.
/// @param kind Which local table.
/// @param entity_id The local row id.
/// @return The triple, or the failure.
auto local_entity_fields(db::connection& conn, link::external_entity_kind kind, std::int64_t entity_id)
    -> std::expected<entity_fields, sync_error> {
  auto const table = table_for(kind);
  if (!table.has_value()) {
    return std::unexpected(sync_error::unsupported_entity_kind);
  }
  // `coalesce(..,'')` on both text columns, matching the Zig original: a NULL
  // title must read as the empty string so it compares equal to an empty
  // remote title rather than being a distinct third state.
  auto stmt =
      conn.prepare(std::format("select coalesce(title,''), coalesce(status,''), updated_at from {} where id = ?", *table));
  if (!stmt || !stmt->bind_int64(1, entity_id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::not_found);
  }
  return entity_fields{.title = stmt->column_text(0), .status = stmt->column_text(1), .updated_at = stmt->column_text(2)};
}

/// @brief Insert one `sync_events` row.
/// @param conn The connection.
/// @param link_id The link.
/// @param direction `pull` or `push`.
/// @param event_outcome The outcome text.
/// @param fields_changed_json A JSON array, or unset for SQL NULL.
/// @param detail Free text, or unset for SQL NULL.
/// @param context_json The evidence blob, or unset for SQL NULL.
/// @return The new event id, or the failure.
auto insert_sync_event(db::connection& conn, std::int64_t link_id, std::string_view direction, std::string_view event_outcome,
                       const std::optional<std::string>& fields_changed_json, const std::optional<std::string>& detail,
                       const std::optional<std::string>& context_json) -> std::expected<std::int64_t, sync_error> {
  auto stmt = conn.prepare("insert into sync_events (link_id, direction, outcome, fields_changed, detail, context_json) "
                           "values (?, ?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(sync_error::query_failed);
  }
  auto const bind_opt = [&](int index, const std::optional<std::string>& value) {
    return value.has_value() ? stmt->bind_text(index, *value).has_value() : stmt->bind_null(index).has_value();
  };
  if (!stmt->bind_int64(1, link_id) || !stmt->bind_text(2, direction) || !stmt->bind_text(3, event_outcome) ||
      !bind_opt(4, fields_changed_json) || !bind_opt(5, detail) || !bind_opt(6, context_json)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(sync_error::query_failed);
  }
  return stmt->column_int64(0);
}

/// @brief Record an adapter failure on a best-effort basis.
///
/// Deliberately swallows its own failures, exactly as the Zig original's
/// `bestEffortErrorWrite` does: this runs on a path that is ALREADY failing,
/// and turning a bookkeeping failure into the reported error would replace
/// the real diagnosis (`TransportFailed`) with a misleading one.
/// @param conn The connection.
/// @param link_id The link.
/// @param direction `pull` or `push`.
/// @param detail The adapter error tag.
auto best_effort_error_write(db::connection& conn, std::int64_t link_id, std::string_view direction, std::string_view detail)
    -> void {
  if (!link::update_sync_state(conn, link_id, link::sync_status::error)) {
    return;
  }
  (void)insert_sync_event(conn, link_id, direction, "error", std::nullopt, std::string(detail), std::nullopt);
}

/// @brief Render a field-name list as a JSON array, or unset when empty.
///
/// NULL rather than `[]` for the empty case, matching the Zig original's
/// `marshalFieldsChanged` — a `noop` event's `fields_changed` column is SQL
/// NULL, which is observable through `audit trail --link --json`.
/// @param fields The field names.
/// @return The JSON array, or unset.
auto marshal_fields_changed(std::span<const std::string> fields) -> std::optional<std::string> {
  if (fields.empty()) {
    return std::nullopt;
  }
  std::string out = "[";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      out += ",";
    }
    json_text::append_json_string(out, fields[i]);
  }
  out += "]";
  return out;
}

/// @brief The database's own `now`, in the format every timestamp column uses.
/// @param conn The connection.
/// @return The stamp, or the failure.
auto db_now(db::connection& conn) -> std::expected<std::string, sync_error> {
  auto stmt = conn.prepare("select strftime('%Y-%m-%dT%H:%M:%fZ','now')");
  if (!stmt) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return std::unexpected(sync_error::query_failed);
  }
  return stmt->column_text(0);
}

/// @brief Build the conflict-evidence blob written to
/// `sync_events.context_json`.
///
/// The key ORDER is part of the contract — an operator diffing two evidence
/// blobs compares bytes — and it is the order the Zig struct literal
/// declares: version, token, observed_at, local{title,status,updated_at,
/// source}, remote{title,status,version,source}. The two `source` strings are
/// literals, not derived from anything.
/// @param conn The connection, for the observation timestamp.
/// @param link_id The link.
/// @param local The local side at the moment of the conflict.
/// @param remote The remote side.
/// @return The JSON blob, or the failure.
auto conflict_evidence_json(db::connection& conn, std::int64_t link_id, const entity_fields& local,
                            const adapter::remote_state& remote) -> std::expected<std::string, sync_error> {
  auto const observed_at = db_now(conn);
  if (!observed_at) {
    return std::unexpected(observed_at.error());
  }
  auto const token =
      evidence_token(link_id, local.title, local.status, local.updated_at, remote.title, remote.status, remote.version);

  std::string out = R"({"version":1,"token":)";
  json_text::append_json_string(out, token);
  out += R"(,"observed_at":)";
  json_text::append_json_string(out, *observed_at);
  out += R"(,"local":{"title":)";
  json_text::append_json_string(out, local.title);
  out += R"(,"status":)";
  json_text::append_json_string(out, local.status);
  out += R"(,"updated_at":)";
  json_text::append_json_string(out, local.updated_at);
  out += R"(,"source":"planar entity"},"remote":{"title":)";
  json_text::append_json_string(out, remote.title);
  out += R"(,"status":)";
  json_text::append_json_string(out, remote.status);
  out += R"(,"version":)";
  json_text::append_json_string(out, remote.version);
  out += R"(,"source":"external adapter pull"}})";
  return out;
}

/// @brief The parsed contents of one conflict event.
struct loaded_conflict_event {
  std::int64_t link_id = 0;    ///< The link the event belongs to.
  std::string  event_outcome;  ///< The event's outcome text.
  std::string  token;          ///< The recorded evidence token.
  std::string  remote_title;   ///< The recorded remote title.
  std::string  remote_status;  ///< The recorded remote status.
  std::string  remote_version; ///< The recorded remote version.
};

/// @brief Read and parse one conflict event's evidence.
///
/// An event with a NULL or unparseable `context_json` is
/// `evidence_changed`, NOT `not_found` — the Zig original makes the same
/// choice, and it is the right one: the event exists, but nothing in it can
/// authorize a resolution.
/// @param conn The connection.
/// @param event_id The event.
/// @return The parsed event, or the failure.
auto load_conflict_event(db::connection& conn, std::int64_t event_id) -> std::expected<loaded_conflict_event, sync_error> {
  auto stmt = conn.prepare("select link_id, outcome, context_json from sync_events where id = ?");
  if (!stmt || !stmt->bind_int64(1, event_id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done || stmt->is_null(0)) {
    // A NULL link_id means the link was deleted out from under the event
    // (`on delete set null`); there is nothing left to resolve against.
    return std::unexpected(sync_error::not_found);
  }
  if (stmt->is_null(2)) {
    return std::unexpected(sync_error::evidence_changed);
  }
  auto const raw    = stmt->column_text(2);
  auto       parsed = glz::read_json<glz::generic>(raw);
  if (!parsed || !parsed->is_object() || !parsed->contains("token") || !parsed->contains("remote")) {
    return std::unexpected(sync_error::evidence_changed);
  }
  auto const& remote = parsed->at("remote");
  if (!remote.is_object()) {
    return std::unexpected(sync_error::evidence_changed);
  }
  auto const field = [&](const glz::generic& obj, std::string_view key) -> std::string {
    if (!obj.contains(key) || !obj.at(key).is_string()) {
      return {};
    }
    return obj.at(key).get<std::string>();
  };
  return loaded_conflict_event{
      .link_id        = stmt->column_int64(0),
      .event_outcome  = stmt->column_text(1),
      .token          = field(*parsed, "token"),
      .remote_title   = field(remote, "title"),
      .remote_status  = field(remote, "status"),
      .remote_version = field(remote, "version"),
  };
}

/// @brief The highest event id on one link, or 0 when it has none.
/// @param conn The connection.
/// @param link_id The link.
/// @return The id, or the failure.
auto latest_event_id(db::connection& conn, std::int64_t link_id) -> std::expected<std::int64_t, sync_error> {
  auto stmt = conn.prepare("select coalesce(max(id), 0) from sync_events where link_id = ?");
  if (!stmt || !stmt->bind_int64(1, link_id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(sync_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::int64_t{0};
  }
  return stmt->column_int64(0);
}

/// @brief Write the remote's title and/or status onto the local entity.
///
/// The four flags are the Zig original's four parameters and each one is
/// separately load-bearing:
///
///   - `skip_empty_remote_status`: when set, an EMPTY remote status is "no
///     opinion" and is never written. `pull_link` sets it; `resolve_conflict`
///     with `keep=remote` sets it too, because the guard has already
///     established the remote is the one being kept.
///   - `allow_missing_local`: when set, a deleted local entity is a silent
///     no-change rather than `not_found`. `pull_link` sets it (the pull still
///     records an event); `resolve_conflict` does not.
///   - `allow_title` / `allow_status`: the per-field gates conflict detection
///     computed. False means "the remote did not change this field since the
///     baseline", and writing it anyway would clobber a local-only edit.
///
/// @brief What `local_diff` found: which fields differ, the remote's value
/// for each one that does, and the entity state the caller would now hold
/// HAD it applied — used to advance the baseline exactly as the removed
/// write would have, without performing the write.
struct diff_result {
  std::vector<std::string>   fields;          ///< Field names that differ, in `title, status` order.
  std::optional<std::string> title;           ///< The remote's title, when `title` is in `fields`.
  std::optional<std::string> status;          ///< The remote's status, when `status` is in `fields`.
  std::string                baseline_title;  ///< `remote.title` if `title` differs, else the CURRENT local title.
  std::string                baseline_status; ///< `remote.status` if `status` differs, else the CURRENT local status.
  bool                       local_present = false; ///< Whether a local row was actually read (false: missing/unsupported).
};

/// @brief Compare the remote state against the local entity and report
/// which fields differ — WITHOUT writing anything.
///
/// A behavior-changing replacement for the removed `apply_remote_to_local`
/// (decision 996, plan 996 task 6419): the write it used to perform,
/// `update {tasks,plans,questions,artifacts} set title = ?, status = ? …`
/// through a table name interpolated from `row.entity_kind`, is exactly
/// the shape `planar-ext`'s connection-level write authorizer (decision
/// 995, `db::connection::restrict_writes_to`) exists to refuse — this
/// module no longer attempts it at all, on either call site. What this
/// function keeps is the SAME comparison logic (same four parameters, same
/// semantics), so `pull_link`'s conflict-vs-apply decision is unchanged;
/// only the "apply" half became "report".
/// @param conn The connection.
/// @param row The link.
/// @param remote The remote state to compare against.
/// @param skip_empty_remote_status Whether an empty remote status is ignored.
/// @param allow_missing_local Whether a deleted local entity is tolerated.
/// @param allow_title Whether the title may be reported as differing.
/// @param allow_status Whether the status may be reported as differing.
/// @return Which fields differ and the remote's values for them, or the failure.
auto local_diff(db::connection& conn, const link::ext_link& row, const adapter::remote_state& remote,
                bool skip_empty_remote_status, bool allow_missing_local, bool allow_title, bool allow_status)
    -> std::expected<diff_result, sync_error> {
  auto const local = local_entity_fields(conn, row.entity_kind, row.entity_id);
  if (!local) {
    if (local.error() == sync_error::not_found && allow_missing_local) {
      return diff_result{};
    }
    if (local.error() == sync_error::unsupported_entity_kind) {
      // The Zig original returns an empty slice here rather than propagating,
      // so a link on an unsyncable kind pulls as `noop` instead of failing.
      return diff_result{};
    }
    return std::unexpected(local.error());
  }

  bool const title_changed = allow_title && !remote.title.empty() && local->title != remote.title;
  bool const status_changed =
      allow_status &&
      (skip_empty_remote_status ? (!remote.status.empty() && local->status != remote.status) : (local->status != remote.status));

  diff_result out;
  out.local_present   = true;
  out.baseline_title  = title_changed ? remote.title : local->title;
  out.baseline_status = status_changed ? remote.status : local->status;
  if (title_changed) {
    out.fields.emplace_back("title");
    out.title = remote.title;
  }
  if (status_changed) {
    out.fields.emplace_back("status");
    out.status = remote.status;
  }
  return out;
}

/// @brief The link status an outcome records.
///
/// `noop` maps to `ok`, NOT to a status of its own — "nothing to do" is a
/// successful sync.
/// @param value The outcome.
/// @return The link status.
auto outcome_to_link_status(outcome value) -> link::sync_status {
  switch (value) {
  case outcome::ok:
  case outcome::noop:
    return link::sync_status::ok;
  case outcome::conflict:
    return link::sync_status::conflict;
  case outcome::error:
    return link::sync_status::error;
  }
  return link::sync_status::ok;
}

/// @brief Map a link-layer failure onto this module's error set.
/// @param err The link error.
/// @return The sync error.
auto from_link_error(link::link_error err) -> sync_error {
  return err == link::link_error::not_found ? sync_error::not_found : sync_error::query_failed;
}

} // namespace

auto outcome_to_event_text(outcome value) -> std::string_view {
  switch (value) {
  case outcome::ok:
    return "ok";
  case outcome::conflict:
    return "conflict";
  case outcome::noop:
    return "noop";
  case outcome::error:
    return "error";
  }
  return "noop";
}

auto resolve_keep_to_text(resolve_keep keep) -> std::string_view {
  return keep == resolve_keep::local ? "local" : "remote";
}

auto evidence_token(std::int64_t link_id, std::string_view local_title, std::string_view local_status,
                    std::string_view local_updated_at, std::string_view remote_title, std::string_view remote_status,
                    std::string_view remote_version) -> std::string {
  // NUL-separated, `v1` prefixed, in exactly this field order. See sha256.hpp
  // for why the digest and the ordering are a contract rather than a choice.
  std::string input = "v1";
  for (auto const part : {std::string_view(std::format("{}", link_id)), local_title, local_status, local_updated_at, remote_title,
                          remote_status, remote_version}) {
    input.push_back('\0');
    input.append(part);
  }
  return sha256::hex(input);
}

auto events_for_link(db::connection& conn, std::int64_t link_id) -> std::expected<std::vector<sync_event>, sync_error> {
  auto stmt = conn.prepare("select id, direction, outcome, fields_changed, detail, context_json, at from sync_events "
                           "where link_id = ? order by id");
  if (!stmt || !stmt->bind_int64(1, link_id)) {
    return std::unexpected(sync_error::query_failed);
  }
  auto const text_opt = [&](int index) -> std::optional<std::string> {
    return stmt->is_null(index) ? std::nullopt : std::optional<std::string>{stmt->column_text(index)};
  };
  std::vector<sync_event> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(sync_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(sync_event{
        .id             = stmt->column_int64(0),
        .direction      = stmt->column_text(1),
        .event_outcome  = stmt->column_text(2),
        .fields_changed = text_opt(3),
        .detail         = text_opt(4),
        .context_json   = text_opt(5),
        .at             = stmt->column_text(6),
    });
  }
  return out;
}

auto pull_link(db::connection& conn, const link::ext_link& row, const adapter::external_adapter& provider)
    -> std::expected<pull_result, sync_error> {
  auto const remote = provider.pull(row.external_id);
  if (!remote) {
    // Outcome, not error — see sync.cppm. This runs BEFORE the transaction is
    // opened, exactly as the Zig original does, so the bookkeeping row
    // survives independently of anything below.
    auto const detail = std::string(adapter::adapter_error_name(remote.error()));
    best_effort_error_write(conn, row.id, "pull", detail);
    return pull_result{.link_id = row.id, .result = outcome::error, .fields_changed = {}, .detail = detail};
  }

  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(sync_error::query_failed);
  }

  pull_result                  result{.link_id = row.id, .result = outcome::noop};
  std::optional<entity_fields> conflict_local;

  if (row.direction == link::sync_direction::write_back) {
    // A write-back link pulls as noop and touches nothing — captured from the
    // oracle, which left task, baseline and link status alone.
    result.result = outcome::noop;
  } else {
    bool allow_title  = true;
    bool allow_status = true;

    auto const base = link::load_baseline(conn, row.id);
    if (!base) {
      return std::unexpected(from_link_error(base.error()));
    }

    // Conflict detection is TWO-WAY ONLY, and only once a baseline exists.
    if (row.direction == link::sync_direction::two_way && base->present()) {
      auto const local = local_entity_fields(conn, row.entity_kind, row.entity_id);
      if (!local) {
        if (local.error() != sync_error::not_found) {
          return std::unexpected(local.error());
        }
      } else {
        conflict_local = *local;

        bool const title_remote_changed  = !remote->title.empty() && remote->title != *base->title;
        bool const title_local_changed   = local->title != *base->title;
        bool const status_remote_changed = !remote->status.empty() && remote->status != *base->status;
        bool const status_local_changed  = local->status != *base->status;

        // The three-part rule, per field. Clause 3 (`remote != local`) is what
        // keeps two sides that independently made the SAME edit from
        // conflicting with each other.
        bool const title_conflict  = title_remote_changed && title_local_changed && remote->title != local->title;
        bool const status_conflict = status_remote_changed && status_local_changed && remote->status != local->status;

        if (title_conflict || status_conflict) {
          result.result = outcome::conflict;
          if (title_conflict) {
            result.fields_changed.emplace_back("title");
          }
          if (status_conflict) {
            result.fields_changed.emplace_back("status");
          }
          result.detail = "local and remote changed since the last successful sync";
        } else {
          // No conflict, but the per-field gates still stand: a field the
          // REMOTE did not change must not be written, or a local-only edit
          // would be silently reverted.
          allow_title  = title_remote_changed;
          allow_status = status_remote_changed;
        }
      }
    }

    if (result.result != outcome::conflict) {
      auto const diff = local_diff(conn, row, *remote, true, true, allow_title, allow_status);
      if (!diff) {
        return std::unexpected(diff.error());
      }
      result.result         = diff->fields.empty() ? outcome::noop : outcome::ok;
      result.fields_changed = diff->fields;
      result.remote_title   = diff->title;
      result.remote_status  = diff->status;

      // The baseline STILL advances exactly as it did when this block wrote
      // the entity (decision 996 changes WHAT gets written, not the
      // baseline formula): `diff->baseline_title`/`baseline_status` are what
      // the entity would now hold HAD it been applied — remote's value for
      // a changed field, the CURRENT local value otherwise — the same pair
      // `local_entity_fields` used to return from its post-apply re-read.
      // This is load-bearing, not cosmetic: D1's first noop pull must still
      // record a baseline (nothing to detect conflicts against otherwise),
      // and `allow_title`/`allow_status` being false means "the remote
      // didn't move since the old baseline", so THAT field keeps the OLD
      // baseline value — a local-only edit must never leak into it (D3).
      if (diff->local_present) {
        auto const baseline_title  = (allow_title || !base->present()) ? diff->baseline_title : *base->title;
        auto const baseline_status = (allow_status || !base->present()) ? diff->baseline_status : *base->status;
        if (auto const stored = link::store_baseline(conn, row.id, baseline_title, baseline_status); !stored) {
          return std::unexpected(from_link_error(stored.error()));
        }
      }
    }
  }

  if (auto const state = link::update_sync_state(conn, row.id, outcome_to_link_status(result.result)); !state) {
    return std::unexpected(from_link_error(state.error()));
  }

  std::optional<std::string> evidence;
  if (result.result == outcome::conflict) {
    if (!conflict_local.has_value()) {
      return std::unexpected(sync_error::not_found);
    }
    auto built = conflict_evidence_json(conn, row.id, *conflict_local, *remote);
    if (!built) {
      return std::unexpected(built.error());
    }
    evidence = std::move(*built);
  }
  auto const written =
      insert_sync_event(conn, row.id, "pull", outcome_to_event_text(result.result), marshal_fields_changed(result.fields_changed),
                        result.detail.empty() ? std::nullopt : std::optional<std::string>{result.detail}, evidence);
  if (!written) {
    return std::unexpected(written.error());
  }

  if (!tx->commit()) {
    return std::unexpected(sync_error::query_failed);
  }
  return result;
}

auto push_link(db::connection& conn, const link::ext_link& row, const adapter::external_adapter& provider)
    -> std::expected<push_result, sync_error> {
  if (row.direction == link::sync_direction::read_only) {
    return std::unexpected(sync_error::read_only);
  }

  auto const fields = local_entity_fields(conn, row.entity_kind, row.entity_id);
  if (!fields) {
    return std::unexpected(fields.error());
  }

  // Title and status only — the Zig original sends exactly these two and
  // leaves assignee/priority unset, so a push never touches them remotely.
  auto const update = provider.push(row.external_id, adapter::field_change_set{.title = fields->title, .status = fields->status});
  if (!update) {
    auto const detail = std::string(adapter::adapter_error_name(update.error()));
    best_effort_error_write(conn, row.id, "push", detail);
    return push_result{.link_id = row.id, .result = outcome::error, .fields_changed = {}, .detail = detail};
  }

  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(sync_error::query_failed);
  }
  if (auto const state = link::update_sync_state(conn, row.id, link::sync_status::ok); !state) {
    return std::unexpected(from_link_error(state.error()));
  }
  // A successful push makes the LOCAL values the new agreed baseline.
  if (auto const stored = link::store_baseline(conn, row.id, fields->title, fields->status); !stored) {
    return std::unexpected(from_link_error(stored.error()));
  }
  if (auto const written = insert_sync_event(conn, row.id, "push", "ok", marshal_fields_changed(update->fields_applied),
                                             std::nullopt, std::nullopt);
      !written) {
    return std::unexpected(written.error());
  }
  if (!tx->commit()) {
    return std::unexpected(sync_error::query_failed);
  }
  return push_result{.link_id = row.id, .result = outcome::ok, .fields_changed = update->fields_applied};
}

auto resolve_conflict(db::connection& conn, std::int64_t event_id, resolve_keep keep, std::string_view expected_evidence_token,
                      std::string_view expected_local_updated_at, const adapter::external_adapter& provider)
    -> std::expected<resolve_result, sync_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(sync_error::query_failed);
  }

  auto const event = load_conflict_event(conn, event_id);
  if (!event) {
    return std::unexpected(event.error());
  }
  if (event->event_outcome != "conflict") {
    return std::unexpected(sync_error::not_conflict);
  }
  // Guard 2: the token the operator supplied must match what was recorded.
  if (event->token != expected_evidence_token) {
    return std::unexpected(sync_error::evidence_changed);
  }
  // Guard 1a: this must still be the link's latest event.
  auto const latest = latest_event_id(conn, event->link_id);
  if (!latest) {
    return std::unexpected(latest.error());
  }
  if (*latest != event_id) {
    return std::unexpected(sync_error::stale_conflict);
  }

  auto const row = link::show(conn, event->link_id);
  if (!row) {
    return std::unexpected(from_link_error(row.error()));
  }
  // Guard 1b: the link must still be IN conflict. A resolution that already
  // happened, or a pull that cleared it, leaves this event unresolvable.
  if (row->last_sync_status != link::sync_status::conflict) {
    return std::unexpected(sync_error::stale_conflict);
  }

  auto const local = local_entity_fields(conn, row->entity_kind, row->entity_id);
  if (!local) {
    return std::unexpected(local.error());
  }
  // Guard 3: the entity must not have moved since the operator approved.
  if (local->updated_at != expected_local_updated_at) {
    return std::unexpected(sync_error::evidence_changed);
  }

  // Guard 4: a FRESH read must still agree with the recorded evidence, and
  // both versions must be non-empty. The version check is the part that
  // cannot be skipped — matching field VALUES prove nothing about whether the
  // remote moved and came back.
  auto const remote = provider.pull(row->external_id);
  if (!remote) {
    return std::unexpected(sync_error::adapter_failed);
  }
  if (event->remote_version.empty() || remote->version.empty() || remote->title != event->remote_title ||
      remote->status != event->remote_status || remote->version != event->remote_version) {
    return std::unexpected(sync_error::evidence_changed);
  }

  if (keep == resolve_keep::local) {
    if (!provider.push(row->external_id, adapter::field_change_set{.title = local->title, .status = local->status})) {
      return std::unexpected(sync_error::adapter_failed);
    }
  }
  // `keep=remote` sends NOTHING to the provider — captured from the oracle
  // as zero PUTs, unchanged — and, as of decision 996 (task 6419), no
  // longer writes the local entity either: the same authorizer-enforced
  // write boundary `pull_link` documents applies here. The conflict is
  // still cleared below (`sync_status::ok`, baseline reset to the CURRENT
  // — unmodified — local values), so this link stops reporting `conflict`.
  // A subsequent `sync pull` then re-detects the remote/local difference as
  // a plain, non-conflicting `ok` emission (local now matches the reset
  // baseline, only the remote moved), which is exactly the state an agent
  // needs to pick the change up and apply it through `planar`.

  if (auto const state = link::update_sync_state(conn, row->id, link::sync_status::ok); !state) {
    return std::unexpected(from_link_error(state.error()));
  }
  // Baseline reset to the CURRENT local values (unchanged by this call —
  // see above). This is what lets the conflict-detection two-way check
  // stop reporting `conflict` on the next pull.
  auto const resolved = local_entity_fields(conn, row->entity_kind, row->entity_id);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  if (auto const stored = link::store_baseline(conn, row->id, resolved->title, resolved->status); !stored) {
    return std::unexpected(from_link_error(stored.error()));
  }

  auto const direction = keep == resolve_keep::local ? "push" : "pull";
  auto const detail    = std::format("resolved={}; from sync_event={}", resolve_keep_to_text(keep), event_id);
  auto const new_event = insert_sync_event(conn, row->id, direction, "ok", std::nullopt, detail, std::nullopt);
  if (!new_event) {
    return std::unexpected(new_event.error());
  }

  if (!tx->commit()) {
    return std::unexpected(sync_error::query_failed);
  }
  return resolve_result{.ok = true, .event_id = event_id, .new_event_id = *new_event, .keep = keep};
}

auto status(db::connection& conn, const link::list_filter& filter) -> std::expected<std::vector<status_row>, sync_error> {
  auto const links = link::list(conn, filter);
  if (!links) {
    return std::unexpected(sync_error::query_failed);
  }
  std::vector<status_row> out;
  out.reserve(links->size());
  for (auto const& row : *links) {
    out.push_back(status_row{
        .link_id          = row.id,
        .entity_kind      = row.entity_kind,
        .entity_id        = row.entity_id,
        .external_id      = row.external_id,
        .system_id        = row.system_id,
        .last_synced_at   = row.last_synced_at,
        .last_sync_status = row.last_sync_status,
    });
  }
  return out;
}

} // namespace planar::engine::external::sync
