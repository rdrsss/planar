/// @file annotation.cpp
/// @brief Implementation of `planar.engine.planning.annotation` (plan 996,
/// task 6094). See annotation.cppm for scope, omissions, and the
/// oracle-derived output shapes.

module planar.engine.planning.annotation;

import std;
import planar.json_text;
import planar.db;
import planar.log;
import planar.scope_ref;
import planar.policy;
import planar.sha256;
import planar.document_authority;
import planar.engine.planning.transitions;

namespace planar::engine::planning::annotation {

// The one shared escape table, layer 1. See json_text.cppm -- the local
// copy this replaced was missing the \b and \f short forms.
using json_text::json_string;

namespace audit = planar::policy::audit;

namespace {

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface.
///
/// Called AFTER the mutation's own write, never before: a refused
/// annotate mutation writes no audit row in the oracle (verified by
/// running `annotate update <id> --status dismissed` against an already
/// `archived` row -- exit 1, `audit_log` untouched).
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, annotation_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(annotation_error::audit_write_failed);
  }
  return {};
}

/// @brief Emit the oracle's inner `<op> exec failed: <ErrorName>` diagnostic
/// ahead of the outer handler error. See
/// `zig/src/engine/planning/annotation.zig`'s `create`/`update`/`remove`
/// for the shapes this ports; mirrors `engine::planning::exec_failed`
/// (task.cpp).
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> annotation_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return annotation_error::query_failed;
}

} // namespace

namespace {

// SQLITE_CONSTRAINT_UNIQUE — the same constant plan.cpp / task.cpp /
// entitylink.cpp already use to detect a UNIQUE violation without
// string-matching the driver's message.
constexpr int k_sqlite_constraint_unique = 2067;
constexpr int k_sqlite_busy              = 5;

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

/// SQLite reports extended busy codes too (for example SQLITE_BUSY_SNAPSHOT),
/// whose low byte remains SQLITE_BUSY.
auto command_db_error(const db::db_error& err) -> annotation_error {
  return (err.code_ & 0xff) == k_sqlite_busy ? annotation_error::busy_source : annotation_error::query_failed;
}

constexpr std::string_view k_select_columns =
    "select id, scope_kind, scope_id, "
    "anchor_kind, anchor_path, anchor_line_start, anchor_line_end, "
    "anchor_commit_sha, anchor_text_hash, anchor_text, "
    "target_kind, target_id, title, slug, body, status, vendor, origin, revision, plan_id, task_id, "
    "created_at, updated_at from annotations";

/// @brief Trim ASCII whitespace the way zig's `std.mem.trim(u8, raw,
/// " \t\r\n")` does — exactly those four bytes, no locale involvement.
auto trim(std::string_view s) -> std::string_view {
  constexpr std::string_view k_ws = " \t\r\n";
  const auto                 b    = s.find_first_not_of(k_ws);
  if (b == std::string_view::npos) {
    return {};
  }
  const auto e = s.find_last_not_of(k_ws);
  return s.substr(b, e - b + 1);
}

auto opt_int(const db::statement& stmt, int index) -> std::optional<std::int64_t> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_int64(index);
}

auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

auto read_row(const db::statement& stmt) -> std::expected<annotation, annotation_error> {
  const auto kind = scope_kind_from_text(stmt.column_text(1));
  if (!kind.has_value()) {
    return std::unexpected(annotation_error::query_failed);
  }
  const auto st = status_from_text(stmt.column_text(15));
  if (!st.has_value()) {
    return std::unexpected(annotation_error::query_failed);
  }
  const auto anchor_kind_text   = stmt.column_text(3);
  const auto stored_anchor_kind = anchor_kind_text == "file"     ? std::optional{anchor_kind::file}
                                  : anchor_kind_text == "entity" ? std::optional{anchor_kind::entity}
                                                                 : std::nullopt;
  if (!stored_anchor_kind.has_value()) {
    return std::unexpected(annotation_error::query_failed);
  }
  std::optional<entity_target> target;
  if (*stored_anchor_kind == anchor_kind::entity) {
    const auto kind_text = stmt.column_text(10);
    const auto kind      = kind_text == "plan"   ? std::optional{target_kind::plan}
                           : kind_text == "task" ? std::optional{target_kind::task}
                                                 : std::nullopt;
    if (!kind.has_value() || stmt.is_null(11)) {
      return std::unexpected(annotation_error::query_failed);
    }
    target = entity_target{.kind = *kind, .id = stmt.column_int64(11)};
  }
  return annotation{
      .id          = stmt.column_int64(0),
      .scope_kind_ = *kind,
      .scope_id    = opt_int(stmt, 2),
      .anchor =
          anchor_fields{
              .path       = stmt.is_null(4) ? std::string{} : stmt.column_text(4),
              .line_start = opt_int(stmt, 5),
              .line_end   = opt_int(stmt, 6),
              .commit_sha = stmt.column_text(7),
              .text_hash  = stmt.column_text(8),
              .text       = stmt.column_text(9),
          },
      .anchor_kind_ = *stored_anchor_kind,
      .target       = std::move(target),
      .title        = opt_text(stmt, 12),
      .slug         = opt_text(stmt, 13),
      .body         = stmt.column_text(14),
      .status_      = *st,
      .vendor       = stmt.column_text(16),
      .origin       = opt_text(stmt, 17),
      .revision     = stmt.column_int64(18),
      .plan_id      = opt_int(stmt, 19),
      .task_id      = opt_int(stmt, 20),
      .tags         = {},
      .created_at   = stmt.column_text(21),
      .updated_at   = stmt.column_text(22),
  };
}

auto load_tags(db::connection& conn, std::int64_t ann_id) -> std::expected<std::vector<std::string>, annotation_error> {
  auto stmt = conn.prepare("select tag from annotation_tags where annotation_id = ? order by tag");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, ann_id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  std::vector<std::string> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(stmt->column_text(0));
  }
  return out;
}

/// @brief Resolve an optional scope-ref slug to the `(scope_kind text,
/// scope_id)` pair the annotation columns store. An unset slug means
/// global. Mirrors the `identity.scope.resolveSlug` call the Zig module
/// makes, routed through the layer-1 `planar.scope_ref` extraction (D19)
/// since `engine_identity` is a forbidden same-layer peer.
auto resolve_scope(db::connection& conn, std::optional<std::string_view> slug)
    -> std::expected<std::pair<scope_kind, std::optional<std::int64_t>>, annotation_error> {
  if (!slug.has_value()) {
    return std::pair<scope_kind, std::optional<std::int64_t>>{scope_kind::global, std::nullopt};
  }
  auto resolved = scope_ref::resolve(conn, *slug);
  if (!resolved) {
    switch (resolved.error()) {
    case scope_ref::error::slug_not_found:
      return std::unexpected(annotation_error::slug_not_found);
    case scope_ref::error::query_failed:
      return std::unexpected(annotation_error::query_failed);
    }
    return std::unexpected(annotation_error::query_failed);
  }
  switch (resolved->kind) {
  case scope_ref::scope_kind::global:
    return std::pair<scope_kind, std::optional<std::int64_t>>{scope_kind::global, resolved->id};
  case scope_ref::scope_kind::association:
    return std::pair<scope_kind, std::optional<std::int64_t>>{scope_kind::association, resolved->id};
  case scope_ref::scope_kind::repo:
    return std::pair<scope_kind, std::optional<std::int64_t>>{scope_kind::repo, resolved->id};
  }
  return std::unexpected(annotation_error::query_failed);
}

auto validate_entity_target(db::connection& conn, const entity_target& target,
                            const std::pair<scope_kind, std::optional<std::int64_t>>& scope)
    -> std::expected<void, annotation_error> {
  const auto table = target.kind == target_kind::plan ? "plans" : "tasks";
  auto       stmt  = conn.prepare(std::format("select scope_kind, scope_id from {} where id = ?", table));
  if (!stmt || !stmt->bind_int64(1, target.id)) {
    return std::unexpected(annotation_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(annotation_error::target_not_found);
  }
  const auto actual_kind = scope_kind_from_text(stmt->column_text(0));
  const auto actual_id   = opt_int(*stmt, 1);
  if (!actual_kind.has_value() || *actual_kind != scope.first || actual_id != scope.second) {
    return std::unexpected(annotation_error::target_scope_mismatch);
  }
  return {};
}

auto load_thread(db::connection& conn, annotation& row) -> std::expected<void, annotation_error> {
  if (row.anchor_kind_ == anchor_kind::file)
    return {};
  auto messages = conn.prepare("select id,body,revision,vendor,origin,created_at,updated_at from annotation_messages "
                               "where annotation_id=? order by created_at,id");
  if (!messages || !messages->bind_int64(1, row.id))
    return std::unexpected(annotation_error::query_failed);
  for (;;) {
    auto step = messages->step();
    if (!step)
      return std::unexpected(annotation_error::query_failed);
    if (*step == db::step_result::done)
      break;
    row.messages.push_back({.id       = messages->column_int64(0),
                            .body     = std::string{messages->column_text(1)},
                            .revision = messages->column_int64(2),
                            .vendor   = std::string{messages->column_text(3)},
                            .origin = messages->is_null(4) ? std::nullopt : std::optional{std::string{messages->column_text(4)}},
                            .created_at = std::string{messages->column_text(5)},
                            .updated_at = std::string{messages->column_text(6)}});
    auto& message = row.messages.back();
    auto  history = conn.prepare("select revision,body,updated_at from annotation_message_revisions "
                                 "where message_id=? order by revision");
    if (!history || !history->bind_int64(1, message.id))
      return std::unexpected(annotation_error::query_failed);
    for (;;) {
      auto history_step = history->step();
      if (!history_step)
        return std::unexpected(annotation_error::query_failed);
      if (*history_step == db::step_result::done)
        break;
      message.history.push_back({.revision   = history->column_int64(0),
                                 .body       = std::string{history->column_text(1)},
                                 .updated_at = std::string{history->column_text(2)}});
    }
  }
  auto anchor = conn.prepare("select document_kind,document_id,content_revision,anchor_kind,start_block_key,end_block_key,"
                             "start_offset,end_offset,normalized_quote,prefix_context,suffix_context,anchor_state,revision "
                             "from annotation_contextual_anchors where annotation_id=?");
  if (!anchor || !anchor->bind_int64(1, row.id))
    return std::unexpected(annotation_error::query_failed);
  auto step = anchor->step();
  if (!step)
    return std::unexpected(annotation_error::query_failed);
  if (*step == db::step_result::done)
    return {};
  row.contextual = contextual_anchor{.document_kind    = std::string{anchor->column_text(0)},
                                     .document_id      = anchor->column_int64(1),
                                     .content_revision = std::string{anchor->column_text(2)},
                                     .kind             = std::string{anchor->column_text(3)},
                                     .start_block_key  = std::string{anchor->column_text(4)},
                                     .end_block_key    = std::string{anchor->column_text(5)},
                                     .start_offset     = opt_int(*anchor, 6),
                                     .end_offset       = opt_int(*anchor, 7),
                                     .normalized_quote = std::string{anchor->column_text(8)},
                                     .prefix_context   = std::string{anchor->column_text(9)},
                                     .suffix_context   = std::string{anchor->column_text(10)},
                                     .state            = std::string{anchor->column_text(11)},
                                     .revision         = anchor->column_int64(12)};
  auto segments  = conn.prepare("select block_key,quote from annotation_anchor_segments where annotation_id=? order by ordinal");
  if (!segments || !segments->bind_int64(1, row.id))
    return std::unexpected(annotation_error::query_failed);
  for (;;) {
    auto segment_step = segments->step();
    if (!segment_step)
      return std::unexpected(annotation_error::query_failed);
    if (*segment_step == db::step_result::done)
      return {};
    row.contextual->segments.emplace_back(segments->column_text(0), segments->column_text(1));
  }
}

/// @brief The single `UPDATE annotations SET status = ?` both the three
/// lifecycle verbs and `update(status = ...)` funnel through.
auto write_status(db::connection& conn, std::int64_t id, status new_status) -> std::expected<void, annotation_error> {
  auto stmt = conn.prepare("update annotations set status = ?, revision = revision + 1, "
                           "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, status_to_text(new_status)); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(annotation_error::query_failed);
  }
  return {};
}

auto transition(db::connection& conn, std::int64_t id, status new_status, std::string_view verb_label)
    -> std::expected<annotation, annotation_error> {
  auto current = show(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  // Both `illegal_transition` AND `unknown_status` map to
  // `terminal_status`, mirroring zig's annotation.transition -- that
  // spelling is what bulk_apply and sweep observe.
  auto allowed =
      check_transition(transition_kind::annotation, status_to_text(current->status_), status_to_text(new_status), false);
  if (!allowed) {
    return std::unexpected(annotation_error::terminal_status);
  }
  if (auto w = write_status(conn, id, new_status); !w) {
    return std::unexpected(w.error());
  }
  // ORACLE: the summary is the VERB LABEL (`resolve` / `dismiss` /
  // `archive`), NOT the resulting status name -- captured as
  // `status_change|annotation|1|resolve`, not `...|resolved`. The two are
  // one letter apart and nothing rendered surfaces either.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                     .entity  = {.kind = "annotation", .id = id},
                                                     .summary = verb_label});
      !a) {
    return std::unexpected(a.error());
  }
  return show(conn, id);
}

// SHA-256 is layer-1 `planar.sha256` (task 6759). This file used to carry a
// second implementation of it a few hundred lines from the `sha256::hex` call
// it already made -- one of five in the tree, counted by round constants. The
// digests are compared against STORED annotation hashes, so the copies were
// verified to agree as ORDERED constant sequences before any was removed.

// --- JSON helpers ----------------------------------------------------------

auto json_optional_int(std::optional<std::int64_t> v) -> std::string {
  return v.has_value() ? std::format("{}", *v) : std::string{"null"};
}

auto json_optional_string(const std::optional<std::string>& v) -> std::string {
  return v.has_value() ? json_string(*v) : std::string{"null"};
}

} // namespace

// ---------------------------------------------------------------------------
// Enum text
// ---------------------------------------------------------------------------

auto status_from_text(std::string_view s) -> std::optional<status> {
  if (s == "active") {
    return status::active;
  }
  if (s == "resolved") {
    return status::resolved;
  }
  if (s == "dismissed") {
    return status::dismissed;
  }
  if (s == "archived") {
    return status::archived;
  }
  return std::nullopt;
}

auto status_to_text(status s) -> std::string_view {
  switch (s) {
  case status::active:
    return "active";
  case status::resolved:
    return "resolved";
  case status::dismissed:
    return "dismissed";
  case status::archived:
    return "archived";
  }
  return "active"; // unreachable
}

auto scope_kind_from_text(std::string_view s) -> std::optional<scope_kind> {
  if (s == "global") {
    return scope_kind::global;
  }
  if (s == "repo") {
    return scope_kind::repo;
  }
  if (s == "association") {
    return scope_kind::association;
  }
  return std::nullopt;
}

auto scope_kind_to_text(scope_kind k) -> std::string_view {
  switch (k) {
  case scope_kind::global:
    return "global";
  case scope_kind::repo:
    return "repo";
  case scope_kind::association:
    return "association";
  }
  return "global"; // unreachable
}

auto is_terminal(status s) -> bool {
  return s == status::resolved || s == status::dismissed || s == status::archived;
}

auto source_uuid(db::connection& conn) -> std::expected<std::string, annotation_error> {
  auto stmt = conn.prepare("select source_uuid from annotation_source_identity where singleton = 1");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  auto step = stmt->step();
  if (!step || *step != db::step_result::row) {
    return std::unexpected(annotation_error::query_failed);
  }
  return stmt->column_text(0);
}

namespace {

auto command_name(command_kind operation) -> std::string_view {
  switch (operation) {
  case command_kind::create:
    return "create";
  case command_kind::edit:
    return "edit";
  case command_kind::replace_tags:
    return "replace-tags";
  case command_kind::resolve:
    return "resolve";
  case command_kind::dismiss:
    return "dismiss";
  case command_kind::archive:
    return "archive";
  case command_kind::remove:
    return "remove";
  case command_kind::bulk_resolve:
    return "bulk-resolve";
  case command_kind::bulk_dismiss:
    return "bulk-dismiss";
  case command_kind::bulk_archive:
    return "bulk-archive";
  case command_kind::reply:
    return "reply";
  case command_kind::edit_message:
    return "edit-message";
  case command_kind::reanchor:
    return "reanchor";
  }
  return "unknown";
}

// A receipt digest is an equality fingerprint, never a security primitive.
// Length-prefixing makes every field boundary unambiguous, including embedded
// delimiters and the distinction between omitted and explicitly-null values.
auto command_digest(const command_args& args) -> std::string {
  std::string material;
  const auto  append = [&](std::string_view value) {
    material += std::format("{}:", value.size());
    material += value;
  };
  const auto optional = [&](const std::optional<std::string_view>& value) {
    append(value ? "present" : "omitted");
    if (value)
      append(*value);
  };
  append(command_name(args.operation));
  append(args.source_uuid);
  optional(args.scope);
  append(std::format("{}", args.annotation_id.value_or(-1)));
  append(std::format("{}", args.expected_revision.value_or(-1)));
  append(args.clear_title ? "null" : "not-null");
  optional(args.title);
  optional(args.body);
  append(args.target ? (args.target->kind == target_kind::plan ? "plan" : "task") : "none");
  append(std::format("{}", args.target ? args.target->id : -1));
  append(std::format("{}", args.tags.size()));
  for (const auto& tag : args.tags)
    append(tag);
  if (args.bulk_filter) {
    append("bulk-filter");
    optional(args.bulk_filter->anchor_path);
    optional(args.bulk_filter->vendor);
    optional(args.bulk_filter->tag);
    optional(args.bulk_filter->scope);
    append(std::format("{}", args.bulk_filter->plan_id.value_or(-1)));
    append(std::format("{}", args.bulk_filter->task_id.value_or(-1)));
    append(args.bulk_filter->status_ ? status_to_text(*args.bulk_filter->status_) : "none");
  }
  // Schema-40 receipts contain precisely the fields above. Only extend the
  // fingerprint when a request actually uses the schema-41 command surface.
  // This preserves old UUID replays while preventing new fields from aliasing
  // an otherwise identical legacy payload.
  if (args.message_id || args.document_kind || args.document_id || args.document_version || args.content_revision ||
      args.contextual_kind || args.start_block_key || args.end_block_key || args.start_offset || args.end_offset ||
      args.normalized_quote || args.prefix_context || args.suffix_context || !args.anchor_segments.empty()) {
    append(std::format("{}", args.message_id.value_or(-1)));
    optional(args.document_kind);
    append(std::format("{}", args.document_id.value_or(-1)));
    append(std::format("{}", args.document_version.value_or(-1)));
    optional(args.content_revision);
    optional(args.contextual_kind);
    optional(args.start_block_key);
    optional(args.end_block_key);
    append(std::format("{}", args.start_offset.value_or(-1)));
    append(std::format("{}", args.end_offset.value_or(-1)));
    optional(args.normalized_quote);
    optional(args.prefix_context);
    optional(args.suffix_context);
    for (const auto& [key, quote] : args.anchor_segments) {
      append(key);
      append(quote);
    }
  }
  return sha256::hex(material);
}

auto receipt_from_row(db::statement& stmt, bool replayed) -> operation_receipt {
  return {.operation_uuid   = std::string(stmt.column_text(0)),
          .source_uuid      = std::string(stmt.column_text(1)),
          .payload_digest   = std::string(stmt.column_text(2)),
          .annotation_id    = opt_int(stmt, 3),
          .revision         = opt_int(stmt, 4),
          .outcome          = std::string(stmt.column_text(5)),
          .created_at       = std::string(stmt.column_text(6)),
          .affected_count   = opt_int(stmt, 7),
          .message_id       = opt_int(stmt, 8),
          .message_revision = opt_int(stmt, 9),
          .replayed         = replayed};
}

// Called only under execute_command's immediate transaction. The projection's
// nested savepoint keeps validation and persistence in the same source snapshot.
auto write_contextual_anchor(db::connection& conn, const command_args& args, std::int64_t id)
    -> std::expected<void, annotation_error> {
  if (!args.document_kind || !args.document_id || args.document_version != 1 || !args.content_revision || !args.contextual_kind ||
      !args.start_block_key || !args.end_block_key)
    return std::unexpected(annotation_error::invalid_anchor);
  const bool range = *args.contextual_kind == "range";
  const bool block = *args.contextual_kind == "block";
  if ((!range && !block) || args.anchor_segments.size() > 256 || args.prefix_context.value_or("").size() > 1024 ||
      args.suffix_context.value_or("").size() > 1024)
    return std::unexpected(annotation_error::invalid_anchor);
  auto thread = show(conn, id);
  if (!thread)
    return std::unexpected(thread.error());
  if (*args.document_kind != "plan" && *args.document_kind != "artifact")
    return std::unexpected(annotation_error::invalid_anchor);
  const auto document_table = *args.document_kind == "plan" ? "plans" : "artifacts";
  auto       owner          = conn.prepare(std::format("select scope_kind, scope_id from {} where id = ?", document_table));
  if (!owner || !owner->bind_int64(1, *args.document_id))
    return std::unexpected(annotation_error::query_failed);
  auto owner_step = owner->step();
  if (!owner_step)
    return std::unexpected(annotation_error::query_failed);
  if (*owner_step != db::step_result::row)
    return std::unexpected(annotation_error::target_not_found);
  if (scope_kind_from_text(owner->column_text(0)) != thread->scope_kind_ || opt_int(*owner, 1) != thread->scope_id)
    return std::unexpected(annotation_error::target_scope_mismatch);
  auto doc = document_authority::project(conn, *args.document_kind, *args.document_id);
  if (!doc)
    return std::unexpected(annotation_error::invalid_anchor);
  if (doc->source_uuid != args.source_uuid)
    return std::unexpected(annotation_error::source_mismatch);
  if (doc->content_revision != *args.content_revision)
    return std::unexpected(annotation_error::revision_conflict);
  std::string normalized;
  if (block) {
    if (args.start_offset || args.end_offset || !args.anchor_segments.empty() || args.start_block_key != args.end_block_key ||
        !args.normalized_quote.value_or("").empty() ||
        std::ranges::find(doc->passages, *args.start_block_key, &document_authority::passage::key) == doc->passages.end())
      return std::unexpected(annotation_error::invalid_anchor);
  } else {
    if (!args.start_offset || !args.end_offset || *args.start_offset < 0 || *args.end_offset < 0 || args.anchor_segments.empty())
      return std::unexpected(annotation_error::invalid_anchor);
    document_authority::range_evidence evidence{.content_revision = std::string{*args.content_revision},
                                                .start_key        = std::string{*args.start_block_key},
                                                .start_offset     = static_cast<std::size_t>(*args.start_offset),
                                                .end_key          = std::string{*args.end_block_key},
                                                .end_offset       = static_cast<std::size_t>(*args.end_offset)};
    for (const auto& [key, quote] : args.anchor_segments) {
      if (key.size() > 512 || quote.empty() || quote.size() > 16384)
        return std::unexpected(annotation_error::invalid_anchor);
      evidence.covered_keys.push_back(key);
      evidence.segment_quotes.push_back(quote);
    }
    auto validated = document_authority::validate(*doc, evidence);
    if (!validated || validated->normalized_quote.empty() || validated->normalized_quote.size() > 65536)
      return std::unexpected(annotation_error::invalid_anchor);
    normalized = std::move(validated->normalized_quote);
    if (args.normalized_quote && *args.normalized_quote != normalized)
      return std::unexpected(annotation_error::invalid_anchor);
  }
  auto erase = conn.prepare("delete from annotation_anchor_segments where annotation_id = ?");
  if (!erase || !erase->bind_int64(1, id) || !erase->step())
    return std::unexpected(annotation_error::query_failed);
  auto write = conn.prepare(
      "insert into annotation_contextual_anchors(annotation_id,schema_version,document_kind,document_id,document_version,"
      "content_revision,anchor_kind,start_block_key,end_block_key,start_offset,end_offset,normalized_quote,prefix_context,"
      "suffix_context) values (?,1,?,?,1,?,?,?,?,?,?,?,?,?) on conflict(annotation_id) do update set "
      "document_kind=excluded.document_kind,document_id=excluded.document_id,content_revision=excluded.content_revision,"
      "anchor_kind=excluded.anchor_kind,start_block_key=excluded.start_block_key,end_block_key=excluded.end_block_key,"
      "start_offset=excluded.start_offset,end_offset=excluded.end_offset,normalized_quote=excluded.normalized_quote,"
      "prefix_context=excluded.prefix_context,suffix_context=excluded.suffix_context,anchor_state='attached',"
      "revision=annotation_contextual_anchors.revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')");
  if (!write || !write->bind_int64(1, id) || !write->bind_text(2, *args.document_kind) ||
      !write->bind_int64(3, *args.document_id) || !write->bind_text(4, *args.content_revision) ||
      !write->bind_text(5, *args.contextual_kind) || !write->bind_text(6, *args.start_block_key) ||
      !write->bind_text(7, *args.end_block_key) ||
      !(args.start_offset ? write->bind_int64(8, *args.start_offset) : write->bind_null(8)) ||
      !(args.end_offset ? write->bind_int64(9, *args.end_offset) : write->bind_null(9)) || !write->bind_text(10, normalized) ||
      !write->bind_text(11, args.prefix_context.value_or("")) || !write->bind_text(12, args.suffix_context.value_or("")) ||
      !write->step())
    return std::unexpected(annotation_error::invalid_anchor);
  for (std::size_t ordinal = 0; ordinal < args.anchor_segments.size(); ++ordinal) {
    auto segment = conn.prepare("insert into annotation_anchor_segments values (?,?,?,?)");
    if (!segment || !segment->bind_int64(1, id) || !segment->bind_int64(2, static_cast<std::int64_t>(ordinal)) ||
        !segment->bind_text(3, args.anchor_segments[ordinal].first) ||
        !segment->bind_text(4, args.anchor_segments[ordinal].second) || !segment->step())
      return std::unexpected(annotation_error::invalid_anchor);
  }
  return {};
}

} // namespace

auto show_receipt(db::connection& conn, std::string_view source, std::string_view operation_uuid)
    -> std::expected<std::optional<operation_receipt>, annotation_error> {
  auto stmt = conn.prepare("select operation_uuid, source_uuid, payload_digest, annotation_id, revision, outcome, created_at, "
                           "affected_count, message_id, message_revision "
                           "from annotation_operation_receipts where source_uuid = ? and operation_uuid = ?");
  if (!stmt || !stmt->bind_text(1, source) || !stmt->bind_text(2, operation_uuid))
    return std::unexpected(annotation_error::query_failed);
  auto step = stmt->step();
  if (!step)
    return std::unexpected(annotation_error::query_failed);
  if (*step == db::step_result::done)
    return std::optional<operation_receipt>{};
  return std::optional<operation_receipt>{receipt_from_row(*stmt, true)};
}

auto execute_command(db::connection& conn, const command_args& args) -> std::expected<operation_receipt, annotation_error> {
  if (args.operation_uuid.empty() || args.source_uuid.empty())
    return std::unexpected(annotation_error::invalid_command);
  auto actual_source = source_uuid(conn);
  if (!actual_source)
    return std::unexpected(actual_source.error());
  if (*actual_source != args.source_uuid)
    return std::unexpected(annotation_error::source_mismatch);
  const auto digest   = command_digest(args);
  auto       existing = show_receipt(conn, args.source_uuid, args.operation_uuid);
  if (!existing)
    return std::unexpected(existing.error());
  if (existing->has_value()) {
    if ((**existing).payload_digest != digest)
      return std::unexpected(annotation_error::receipt_conflict);
    return **existing;
  }
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx)
    return std::unexpected(command_db_error(tx.error()));
  // Recheck under the write lock; a concurrent retry must not double-create.
  existing = show_receipt(conn, args.source_uuid, args.operation_uuid);
  if (!existing)
    return std::unexpected(existing.error());
  if (existing->has_value()) {
    if ((**existing).payload_digest != digest)
      return std::unexpected(annotation_error::receipt_conflict);
    return **existing;
  }

  std::optional<std::int64_t> id;
  std::optional<std::int64_t> revision;
  std::optional<std::int64_t> affected_count;
  std::optional<std::int64_t> message_id;
  std::optional<std::int64_t> message_revision;
  if (args.annotation_id) {
    auto current = show(conn, *args.annotation_id);
    if (!current)
      return std::unexpected(current.error());
    if (current->anchor_kind_ == anchor_kind::entity) {
      // Request ownership assertions are distinct from the operator's
      // membership-aware authorization selector and legacy scope patches.
      if (args.target &&
          (!current->target || args.target->kind != current->target->kind || args.target->id != current->target->id))
        return std::unexpected(annotation_error::target_scope_mismatch);
      if (args.scope) {
        auto owner = resolve_scope(conn, args.scope);
        if (!owner)
          return std::unexpected(owner.error());
        if (owner->first != current->scope_kind_ || owner->second != current->scope_id)
          return std::unexpected(annotation_error::target_scope_mismatch);
      }
      if (!current->target)
        return std::unexpected(annotation_error::invalid_anchor);
      if (auto owner = validate_entity_target(conn, *current->target, {current->scope_kind_, current->scope_id}); !owner)
        return std::unexpected(owner.error());
    }
  }
  if (args.operation == command_kind::bulk_resolve || args.operation == command_kind::bulk_dismiss ||
      args.operation == command_kind::bulk_archive) {
    if (!args.bulk_filter)
      return std::unexpected(annotation_error::invalid_command);
    auto items = list(conn, *args.bulk_filter);
    if (!items)
      return std::unexpected(items.error());
    const auto  action = args.operation == command_kind::bulk_resolve   ? bulk_action::resolve
                         : args.operation == command_kind::bulk_dismiss ? bulk_action::dismiss
                                                                        : bulk_action::archive;
    std::size_t count  = 0;
    for (const auto& a : *items) {
      const bool already = (action == bulk_action::resolve && a.status_ == status::resolved) ||
                           (action == bulk_action::dismiss && a.status_ == status::dismissed) ||
                           (action == bulk_action::archive && a.status_ == status::archived);
      if (already || (action != bulk_action::archive && is_terminal(a.status_)))
        continue;
      auto changed = action == bulk_action::resolve   ? resolve(conn, a.id)
                     : action == bulk_action::dismiss ? dismiss(conn, a.id)
                                                      : archive(conn, a.id);
      if (!changed) {
        if (changed.error() == annotation_error::terminal_status)
          continue;
        return std::unexpected(changed.error());
      }
      ++count;
    }
    affected_count = static_cast<std::int64_t>(count);
    // Aggregate receipts intentionally carry no per-row revision: every
    // affected annotation has its own revision and audit row.
  } else if (args.operation == command_kind::create) {
    if (!args.target.has_value())
      return std::unexpected(annotation_error::invalid_command);
    create_args input{.anchor       = {},
                      .anchor_kind_ = anchor_kind::entity,
                      .target       = args.target,
                      .title        = args.title,
                      .body         = args.body.value_or(""),
                      .origin       = args.origin,
                      .tags         = args.tags,
                      .scope        = args.scope};
    auto        created = create(conn, input);
    if (!created)
      return std::unexpected(created.error());
    id       = created->id;
    revision = created->revision;
    if (args.contextual_kind) {
      if (auto anchor = write_contextual_anchor(conn, args, *id); !anchor)
        return std::unexpected(anchor.error());
    } else if (args.document_kind || args.document_id || args.document_version || args.content_revision || args.start_block_key ||
               args.end_block_key || args.start_offset || args.end_offset || args.normalized_quote || args.prefix_context ||
               args.suffix_context || !args.anchor_segments.empty()) {
      return std::unexpected(annotation_error::invalid_anchor);
    }
  } else if (args.operation == command_kind::reply) {
    if (!args.annotation_id || !args.body || args.body->empty())
      return std::unexpected(annotation_error::invalid_command);
    auto current = show(conn, *args.annotation_id);
    if (!current)
      return std::unexpected(current.error());
    if (current->anchor_kind_ != anchor_kind::entity)
      return std::unexpected(annotation_error::invalid_anchor);
    auto insert_message = conn.prepare(
        "insert into annotation_messages(annotation_id, body, vendor, origin) values (?, ?, ?, ?) returning id, revision");
    if (!insert_message || !insert_message->bind_int64(1, *args.annotation_id) || !insert_message->bind_text(2, *args.body) ||
        !insert_message->bind_text(3, current->vendor) || !insert_message->bind_text(4, args.origin) || !insert_message->step())
      return std::unexpected(annotation_error::query_failed);
    id               = *args.annotation_id;
    revision         = current->revision;
    message_id       = insert_message->column_int64(0);
    message_revision = insert_message->column_int64(1);
    if (auto audit = record_audit(conn, {.verb    = audit::verb::update,
                                         .entity  = {.kind = "annotation", .id = *args.annotation_id},
                                         .summary = "append annotation thread reply"});
        !audit)
      return std::unexpected(audit.error());
  } else if (args.operation == command_kind::edit_message) {
    if (!args.annotation_id || !args.message_id || !args.expected_revision || !args.body || args.body->empty())
      return std::unexpected(annotation_error::invalid_command);
    auto edit = conn.prepare("update annotation_messages set body = ?, revision = revision + 1, "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                             "where id = ? and annotation_id = ? and revision = ? returning revision");
    if (!edit || !edit->bind_text(1, *args.body) || !edit->bind_int64(2, *args.message_id) ||
        !edit->bind_int64(3, *args.annotation_id) || !edit->bind_int64(4, *args.expected_revision))
      return std::unexpected(annotation_error::query_failed);
    auto step = edit->step();
    if (!step)
      return std::unexpected(annotation_error::query_failed);
    if (*step == db::step_result::done)
      return std::unexpected(annotation_error::revision_conflict);
    id               = *args.annotation_id;
    message_id       = args.message_id;
    message_revision = edit->column_int64(0);
    // The first message is the legacy body projection. Advance its thread
    // revision too, so an old editor cannot overwrite a newer message edit.
    auto first = conn.prepare("select id from annotation_messages where annotation_id = ? order by created_at, id limit 1");
    if (!first || !first->bind_int64(1, *id))
      return std::unexpected(annotation_error::query_failed);
    auto first_step = first->step();
    if (!first_step || *first_step == db::step_result::done)
      return std::unexpected(annotation_error::query_failed);
    if (first->column_int64(0) == *message_id) {
      auto sync = conn.prepare("update annotations set body = ?, revision = revision + 1, "
                               "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
      if (!sync || !sync->bind_text(1, *args.body) || !sync->bind_int64(2, *id) || !sync->step())
        return std::unexpected(annotation_error::query_failed);
    }
    auto current = show(conn, *id);
    if (!current)
      return std::unexpected(current.error());
    revision = current->revision;
    if (auto audit = record_audit(conn, {.verb    = audit::verb::update,
                                         .entity  = {.kind = "annotation", .id = *args.annotation_id},
                                         .summary = "edit annotation thread message"});
        !audit)
      return std::unexpected(audit.error());
  } else if (args.operation == command_kind::reanchor) {
    if (!args.annotation_id || !args.expected_revision || !args.document_kind || !args.document_id || !args.document_version ||
        !args.contextual_kind || !args.start_block_key || !args.end_block_key)
      return std::unexpected(annotation_error::invalid_command);
    auto current = show(conn, *args.annotation_id);
    if (!current)
      return std::unexpected(current.error());
    if (current->anchor_kind_ != anchor_kind::entity || current->revision != *args.expected_revision)
      return std::unexpected(current->revision == *args.expected_revision ? annotation_error::invalid_anchor
                                                                          : annotation_error::revision_conflict);
    auto anchor_revision = conn.prepare("select revision from annotation_contextual_anchors where annotation_id = ?");
    if (!anchor_revision || !anchor_revision->bind_int64(1, *args.annotation_id))
      return std::unexpected(annotation_error::query_failed);
    auto anchor_step = anchor_revision->step();
    if (!anchor_step)
      return std::unexpected(annotation_error::query_failed);
    const bool exists = *anchor_step == db::step_result::row;
    if (auto anchor = write_contextual_anchor(conn, args, *args.annotation_id); !anchor)
      return std::unexpected(anchor.error());
    auto bump = conn.prepare("update annotations set revision = revision + 1, "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ? and revision = ?");
    if (!bump || !bump->bind_int64(1, *args.annotation_id) || !bump->bind_int64(2, *args.expected_revision) || !bump->step())
      return std::unexpected(annotation_error::query_failed);
    id       = *args.annotation_id;
    revision = *args.expected_revision + 1;
    if (!exists) {
      if (auto audit = record_audit(conn, {.verb    = audit::verb::update,
                                           .entity  = {.kind = "annotation", .id = *args.annotation_id},
                                           .summary = "explicit contextual re-anchor"});
          !audit)
        return std::unexpected(audit.error());
    }
  } else {
    if (!args.annotation_id.has_value() || !args.expected_revision.has_value())
      return std::unexpected(annotation_error::invalid_command);
    auto current = show(conn, *args.annotation_id);
    if (!current)
      return std::unexpected(current.error());
    if (current->revision != *args.expected_revision)
      return std::unexpected(annotation_error::revision_conflict);
    id = current->id;
    if (args.operation == command_kind::edit) {
      auto changed = update(conn, *id, {.title = args.title, .clear_title = args.clear_title, .body = args.body});
      if (!changed)
        return std::unexpected(changed.error());
      revision = changed->revision;
    } else if (args.operation == command_kind::replace_tags) {
      auto clear = conn.prepare("delete from annotation_tags where annotation_id = ?");
      if (!clear || !clear->bind_int64(1, *id) || !clear->step())
        return std::unexpected(annotation_error::query_failed);
      for (const auto& tag : args.tags) {
        if (trim(tag).empty())
          return std::unexpected(annotation_error::empty_tag);
        auto insert = conn.prepare("insert into annotation_tags(annotation_id, tag) values (?, ?) on conflict do nothing");
        if (!insert || !insert->bind_int64(1, *id) || !insert->bind_text(2, trim(tag)) || !insert->step())
          return std::unexpected(annotation_error::query_failed);
      }
      auto bump = conn.prepare(
          "update annotations set revision = revision + 1, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
      if (!bump || !bump->bind_int64(1, *id) || !bump->step())
        return std::unexpected(annotation_error::query_failed);
      if (auto audit = record_audit(conn, {.verb = audit::verb::update, .entity = {.kind = "annotation", .id = *id}}); !audit)
        return std::unexpected(audit.error());
      auto changed = show(conn, *id);
      if (!changed)
        return std::unexpected(changed.error());
      revision = changed->revision;
    } else if (args.operation == command_kind::resolve || args.operation == command_kind::dismiss ||
               args.operation == command_kind::archive) {
      auto changed = args.operation == command_kind::resolve   ? resolve(conn, *id)
                     : args.operation == command_kind::dismiss ? dismiss(conn, *id)
                                                               : archive(conn, *id);
      if (!changed)
        return std::unexpected(changed.error());
      revision = changed->revision;
    } else if (args.operation == command_kind::remove) {
      revision     = current->revision + 1;
      auto removed = remove(conn, *id);
      if (!removed)
        return std::unexpected(removed.error());
    } else
      return std::unexpected(annotation_error::invalid_command);
  }
  auto insert = conn.prepare(
      "insert into annotation_operation_receipts(operation_uuid, source_uuid, payload_digest, "
      "annotation_id, revision, outcome, affected_count, message_id, message_revision) values (?, ?, ?, ?, ?, ?, ?, ?, ?)");
  if (!insert || !insert->bind_text(1, args.operation_uuid) || !insert->bind_text(2, args.source_uuid) ||
      !insert->bind_text(3, digest) || !(id ? insert->bind_int64(4, *id) : insert->bind_null(4)) ||
      !(revision ? insert->bind_int64(5, *revision) : insert->bind_null(5)) ||
      !insert->bind_text(6, command_name(args.operation)) ||
      !(affected_count ? insert->bind_int64(7, *affected_count) : insert->bind_null(7)) ||
      !(message_id ? insert->bind_int64(8, *message_id) : insert->bind_null(8)) ||
      !(message_revision ? insert->bind_int64(9, *message_revision) : insert->bind_null(9)) || !insert->step())
    return std::unexpected(annotation_error::query_failed);
  auto committed = tx->commit();
  if (!committed)
    return std::unexpected(command_db_error(committed.error()));
  auto receipt = show_receipt(conn, args.source_uuid, args.operation_uuid);
  if (!receipt || !receipt->has_value())
    return std::unexpected(annotation_error::query_failed);
  auto result     = **receipt;
  result.replayed = false;
  return result;
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

auto show(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(annotation_error::not_found);
  }
  auto row = read_row(*stmt);
  if (!row) {
    return std::unexpected(row.error());
  }
  auto tags = load_tags(conn, row->id);
  if (!tags) {
    return std::unexpected(tags.error());
  }
  row->tags = std::move(*tags);
  if (auto thread = load_thread(conn, *row); !thread)
    return std::unexpected(thread.error());
  return row;
}

auto show_by_slug(db::connection& conn, std::string_view slug) -> std::expected<annotation, annotation_error> {
  auto stmt = conn.prepare("select id from annotations where slug = ?");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, slug); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(annotation_error::not_found);
  }
  return show(conn, stmt->column_int64(0));
}

auto create(db::connection& conn, const create_args& args) -> std::expected<annotation, annotation_error> {
  auto scope = resolve_scope(conn, args.scope);
  if (!scope) {
    return std::unexpected(scope.error());
  }
  if (args.anchor_kind_ == anchor_kind::file && args.target.has_value()) {
    return std::unexpected(annotation_error::invalid_anchor);
  }
  if (args.anchor_kind_ == anchor_kind::entity && (!args.target.has_value() || !args.anchor.path.empty())) {
    return std::unexpected(annotation_error::invalid_anchor);
  }
  if (args.anchor_kind_ == anchor_kind::entity) {
    if (auto target = validate_entity_target(conn, *args.target, *scope); !target) {
      return std::unexpected(target.error());
    }
  }

  auto stmt = conn.prepare("insert into annotations ("
                           "scope_kind, scope_id, "
                           "anchor_kind, anchor_path, anchor_line_start, anchor_line_end, "
                           "anchor_commit_sha, anchor_text_hash, anchor_text, "
                           "target_kind, target_id, title, slug, body, status, vendor, origin, plan_id, task_id"
                           ") values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(exec_failed("annotation.create", "PrepareFailed"));
  }

  const auto bind_opt_text = [&](int index, std::optional<std::string_view> v) {
    return v.has_value() ? stmt->bind_text(index, *v).has_value() : stmt->bind_null(index).has_value();
  };
  const auto bind_opt_int = [&](int index, std::optional<std::int64_t> v) {
    return v.has_value() ? stmt->bind_int64(index, *v).has_value() : stmt->bind_null(index).has_value();
  };

  const auto target_kind_text = args.target.has_value()
                                    ? std::optional<std::string_view>{args.target->kind == target_kind::plan ? "plan" : "task"}
                                    : std::nullopt;
  const auto target_id        = args.target.has_value() ? std::optional{args.target->id} : std::nullopt;
  const auto plan_id =
      args.anchor_kind_ == anchor_kind::entity && args.target->kind == target_kind::plan ? target_id : args.plan_id;
  const auto task_id =
      args.anchor_kind_ == anchor_kind::entity && args.target->kind == target_kind::task ? target_id : args.task_id;
  const bool bound =
      stmt->bind_text(1, scope_kind_to_text(scope->first)).has_value() && bind_opt_int(2, scope->second) &&
      stmt->bind_text(3, args.anchor_kind_ == anchor_kind::file ? "file" : "entity").has_value() &&
      (args.anchor_kind_ == anchor_kind::file ? stmt->bind_text(4, args.anchor.path).has_value()
                                              : stmt->bind_null(4).has_value()) &&
      bind_opt_int(5, args.anchor.line_start) && bind_opt_int(6, args.anchor.line_end) &&
      stmt->bind_text(7, args.anchor.commit_sha).has_value() && stmt->bind_text(8, args.anchor.text_hash).has_value() &&
      stmt->bind_text(9, args.anchor.text).has_value() && bind_opt_text(10, target_kind_text) && bind_opt_int(11, target_id) &&
      bind_opt_text(12, args.title) && bind_opt_text(13, args.slug) && stmt->bind_text(14, args.body).has_value() &&
      stmt->bind_text(15, status_to_text(args.status_)).has_value() && stmt->bind_text(16, args.vendor).has_value() &&
      bind_opt_text(17, args.origin) && bind_opt_int(18, plan_id) && bind_opt_int(19, task_id);
  if (!bound) {
    return std::unexpected(exec_failed("annotation.create", "BindFailed"));
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(annotation_error::slug_conflict);
    }
    return std::unexpected(exec_failed("annotation.create", "StepFailed"));
  }
  if (*step != db::step_result::row) {
    return std::unexpected(exec_failed("annotation.create", "StepFailed"));
  }
  const auto id = stmt->column_int64(0);

  // Tags: trim, drop empties, de-duplicate -- in FIRST-SEEN order, which
  // does not matter for readback since every read sorts by tag.
  std::vector<std::string_view> seen;
  for (const auto& raw : args.tags) {
    const auto t = trim(raw);
    if (t.empty()) {
      continue;
    }
    if (std::ranges::find(seen, t) != seen.end()) {
      continue;
    }
    seen.push_back(t);
    auto ins = conn.prepare("insert into annotation_tags (annotation_id, tag) values (?, ?)");
    if (!ins) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto b = ins->bind_int64(1, id); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto b = ins->bind_text(2, t); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto s = ins->step(); !s) {
      return std::unexpected(annotation_error::query_failed);
    }
  }

  // ORACLE: `create annotation 'A1'` -- the TITLE in single quotes, and
  // the anchor path when there is no title. Captured against a titled row
  // (`create annotation 'A-alpha'`) and an untitled one.
  const auto summary = std::format("create annotation '{}'", args.title.value_or(std::string_view{args.anchor.path}));
  if (auto a = record_audit(
          conn, audit::record_args{.verb = audit::verb::create, .entity = {.kind = "annotation", .id = id}, .summary = summary});
      !a) {
    return std::unexpected(a.error());
  }

  return show(conn, id);
}

auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<annotation>, annotation_error> {
  std::optional<std::pair<scope_kind, std::optional<std::int64_t>>> scope;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope = *resolved;
  }

  std::string sql = std::format("{} where 1 = 1", k_select_columns);
  if (filter.anchor_path.has_value()) {
    sql += " and anchor_path = ?";
  }
  if (filter.anchor_kind_.has_value()) {
    sql += " and anchor_kind = ?";
  }
  if (filter.target_kind_.has_value()) {
    sql += " and target_kind = ?";
  }
  if (filter.target_id.has_value()) {
    sql += " and target_id = ?";
  }
  if (filter.status_.has_value()) {
    sql += " and status = ?";
  }
  if (filter.plan_id.has_value()) {
    sql += " and plan_id = ?";
  }
  if (filter.task_id.has_value()) {
    sql += " and task_id = ?";
  }
  if (filter.vendor.has_value()) {
    sql += " and vendor = ?";
  }
  if (filter.tag.has_value()) {
    sql += " and id in (select annotation_id from annotation_tags where tag = ?)";
  }
  if (scope.has_value()) {
    switch (scope->first) {
    case scope_kind::global:
      sql += " and scope_kind = 'global'";
      break;
    case scope_kind::association:
      sql += " and scope_kind = 'association' and scope_id = ?";
      break;
    case scope_kind::repo:
      sql += " and scope_kind = 'repo' and scope_id = ?";
      break;
    }
  }
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  int idx = 1;
  if (filter.anchor_path.has_value()) {
    if (auto b = stmt->bind_text(idx++, *filter.anchor_path); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.anchor_kind_.has_value()) {
    if (auto b = stmt->bind_text(idx++, *filter.anchor_kind_ == anchor_kind::file ? "file" : "entity"); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.target_kind_.has_value()) {
    if (auto b = stmt->bind_text(idx++, *filter.target_kind_ == target_kind::plan ? "plan" : "task"); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.target_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.target_id); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.status_.has_value()) {
    if (auto b = stmt->bind_text(idx++, status_to_text(*filter.status_)); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.plan_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.plan_id); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.task_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.task_id); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.vendor.has_value()) {
    if (auto b = stmt->bind_text(idx++, *filter.vendor); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.tag.has_value()) {
    if (auto b = stmt->bind_text(idx++, *filter.tag); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (scope.has_value() && scope->first != scope_kind::global) {
    if (!scope->second.has_value()) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto b = stmt->bind_int64(idx++, *scope->second); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }

  std::vector<annotation> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    auto row = read_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    out.push_back(std::move(*row));
  }
  // Tags are a follow-up query per row, matching the Zig original (which
  // also cannot nest a second statement inside its own step loop).
  for (auto& row : out) {
    auto tags = load_tags(conn, row.id);
    if (!tags) {
      return std::unexpected(tags.error());
    }
    row.tags = std::move(*tags);
    if (auto thread = load_thread(conn, row); !thread)
      return std::unexpected(thread.error());
  }
  return out;
}

auto update(db::connection& conn, std::int64_t id, const update_args& patch) -> std::expected<annotation, annotation_error> {
  std::optional<std::pair<scope_kind, std::optional<std::int64_t>>> scope;
  if (patch.scope.has_value()) {
    auto resolved = resolve_scope(conn, patch.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope = *resolved;
  }

  auto current = show(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }

  if (patch.status_.has_value()) {
    auto allowed =
        check_transition(transition_kind::annotation, status_to_text(current->status_), status_to_text(*patch.status_), false);
    if (!allowed) {
      return std::unexpected(annotation_error::terminal_status);
    }
  }

  std::string sql   = "update annotations set ";
  bool        first = true;
  const auto  sep   = [&]() {
    if (!first) {
      sql += ", ";
    }
    first = false;
  };

  if (scope.has_value()) {
    sep();
    sql += "scope_kind = ?";
    sep();
    sql += "scope_id = ?";
  }
  if (patch.title.has_value() || patch.clear_title) {
    sep();
    sql += "title = ?";
  }
  if (patch.slug.has_value()) {
    sep();
    sql += "slug = ?";
  }
  if (patch.body.has_value()) {
    sep();
    sql += "body = ?";
  }
  if (patch.status_.has_value()) {
    sep();
    sql += "status = ?";
  }
  if (patch.plan_id.has_value()) {
    sep();
    sql += "plan_id = ?";
  }
  if (patch.task_id.has_value()) {
    sep();
    sql += "task_id = ?";
  }
  if (patch.anchor.has_value()) {
    sep();
    sql += "anchor_path = ?, anchor_line_start = ?, anchor_line_end = ?, "
           "anchor_commit_sha = ?, anchor_text_hash = ?, anchor_text = ?";
  }

  // An all-unset patch is a no-op that returns a FRESH SNAPSHOT and does
  // NOT bump updated_at -- matching zig's `if (first) return try show(...)`.
  // This return is BEFORE the audit write below on purpose: a bare
  // `annotate update <id>` writes no `audit_log` row in the oracle either
  // (verified by running it and reading the table back -- the row's
  // `create` entry was the only one present afterwards).
  if (first) {
    return show(conn, id);
  }

  sep();
  sql += "revision = revision + 1, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(exec_failed("annotation.update", "PrepareFailed"));
  }
  int  idx   = 1;
  bool bound = true;
  if (scope.has_value()) {
    bound = bound && stmt->bind_text(idx++, scope_kind_to_text(scope->first)).has_value();
    bound = bound && (scope->second.has_value() ? stmt->bind_int64(idx++, *scope->second).has_value()
                                                : stmt->bind_null(idx++).has_value());
  }
  if (patch.title.has_value() || patch.clear_title) {
    bound = bound && (patch.clear_title ? stmt->bind_null(idx++).has_value() : stmt->bind_text(idx++, *patch.title).has_value());
  }
  if (patch.slug.has_value()) {
    bound = bound && stmt->bind_text(idx++, *patch.slug).has_value();
  }
  if (patch.body.has_value()) {
    bound = bound && stmt->bind_text(idx++, *patch.body).has_value();
  }
  if (patch.status_.has_value()) {
    bound = bound && stmt->bind_text(idx++, status_to_text(*patch.status_)).has_value();
  }
  if (patch.plan_id.has_value()) {
    bound = bound && stmt->bind_int64(idx++, *patch.plan_id).has_value();
  }
  if (patch.task_id.has_value()) {
    bound = bound && stmt->bind_int64(idx++, *patch.task_id).has_value();
  }
  if (patch.anchor.has_value()) {
    const auto& a = *patch.anchor;
    bound         = bound && stmt->bind_text(idx++, a.path).has_value();
    bound = bound &&
            (a.line_start.has_value() ? stmt->bind_int64(idx++, *a.line_start).has_value() : stmt->bind_null(idx++).has_value());
    bound =
        bound && (a.line_end.has_value() ? stmt->bind_int64(idx++, *a.line_end).has_value() : stmt->bind_null(idx++).has_value());
    bound = bound && stmt->bind_text(idx++, a.commit_sha).has_value();
    bound = bound && stmt->bind_text(idx++, a.text_hash).has_value();
    bound = bound && stmt->bind_text(idx++, a.text).has_value();
  }
  bound = bound && stmt->bind_int64(idx, id).has_value();
  if (!bound) {
    return std::unexpected(exec_failed("annotation.update", "BindFailed"));
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(annotation_error::slug_conflict);
    }
    return std::unexpected(exec_failed("annotation.update", "StepFailed"));
  }
  // ORACLE: a patch that carries a status writes `status_change`; any
  // other patch writes `update`. BOTH carry a NULL summary -- verified by
  // running `annotate update 1 --status resolved` and `annotate update 2
  // --title T2 --status dismissed`, each of which produced
  // `status_change|annotation|<id>|<NULL>`. The verb-labelled summaries
  // come only from the dedicated `resolve`/`dismiss`/`archive` verbs.
  if (auto a = record_audit(
          conn, audit::record_args{.verb   = patch.status_.has_value() ? audit::verb::status_change : audit::verb::update,
                                   .entity = {.kind = "annotation", .id = id}});
      !a) {
    return std::unexpected(a.error());
  }
  return show(conn, id);
}

auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, annotation_error> {
  // Verify existence first, so a missing id is not_found rather than a
  // silent no-op.
  auto current = show(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto stmt = conn.prepare("delete from annotations where id = ?");
  if (!stmt) {
    return std::unexpected(exec_failed("annotation.remove", "PrepareFailed"));
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(exec_failed("annotation.remove", "BindFailed"));
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(exec_failed("annotation.remove", "StepFailed"));
  }
  // ORACLE: `delete` with a NULL summary -- the deleted row's title is NOT
  // interpolated, unlike `create`.
  return record_audit(conn, audit::record_args{.verb = audit::verb::delete_, .entity = {.kind = "annotation", .id = id}});
}

auto resolve(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error> {
  return transition(conn, id, status::resolved, "resolve");
}

auto dismiss(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error> {
  return transition(conn, id, status::dismissed, "dismiss");
}

auto archive(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error> {
  return transition(conn, id, status::archived, "archive");
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

auto add_tag(db::connection& conn, std::int64_t ann_id, std::string_view tag) -> std::expected<void, annotation_error> {
  const auto trimmed = trim(tag);
  if (trimmed.empty()) {
    return std::unexpected(annotation_error::empty_tag);
  }
  auto current = show(conn, ann_id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto stmt = conn.prepare("insert into annotation_tags (annotation_id, tag) values (?, ?) "
                           "on conflict(annotation_id, tag) do nothing");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, ann_id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_text(2, trimmed); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(annotation_error::query_failed);
  }
  auto changed = conn.prepare("select changes()");
  if (!changed || !changed->step()) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (changed->column_int64(0) == 0) {
    return {};
  }
  auto revision = conn.prepare(
      "update annotations set revision = revision + 1, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!revision || !revision->bind_int64(1, ann_id) || !revision->step()) {
    return std::unexpected(annotation_error::query_failed);
  }
  return {};
}

auto remove_tag(db::connection& conn, std::int64_t ann_id, std::string_view tag) -> std::expected<void, annotation_error> {
  const auto trimmed = trim(tag);
  if (trimmed.empty()) {
    return std::unexpected(annotation_error::empty_tag);
  }
  auto stmt = conn.prepare("delete from annotation_tags where annotation_id = ? and tag = ?");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, ann_id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_text(2, trimmed); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(annotation_error::query_failed);
  }
  auto changed = conn.prepare("select changes()");
  if (!changed || !changed->step()) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (changed->column_int64(0) == 0) {
    return {};
  }
  auto revision = conn.prepare(
      "update annotations set revision = revision + 1, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!revision || !revision->bind_int64(1, ann_id) || !revision->step()) {
    return std::unexpected(annotation_error::query_failed);
  }
  return {};
}

auto list_tags(db::connection& conn, std::int64_t ann_id) -> std::expected<std::vector<std::string>, annotation_error> {
  return load_tags(conn, ann_id);
}

// ---------------------------------------------------------------------------
// bulk
// ---------------------------------------------------------------------------

auto bulk_apply(db::connection& conn, const list_filter& filter, bulk_action action)
    -> std::expected<std::size_t, annotation_error> {
  // A bulk lifecycle request is one domain operation: a failed row must
  // roll back every earlier row, including their audit records and revisions.
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx)
    return std::unexpected(annotation_error::query_failed);
  auto items = list(conn, filter);
  if (!items) {
    return std::unexpected(items.error());
  }

  std::size_t count = 0;
  for (const auto& a : *items) {
    const bool already_done = (action == bulk_action::resolve && a.status_ == status::resolved) ||
                              (action == bulk_action::dismiss && a.status_ == status::dismissed) ||
                              (action == bulk_action::archive && a.status_ == status::archived);
    if (already_done) {
      continue;
    }
    // resolve/dismiss cannot accept a row at ANY outcome state.
    // archive deliberately does NOT pre-skip: under the retention-tier
    // model resolved and dismissed legally progress to archived.
    if (action != bulk_action::archive && is_terminal(a.status_)) {
      continue;
    }

    std::expected<annotation, annotation_error> updated = action == bulk_action::resolve   ? resolve(conn, a.id)
                                                          : action == bulk_action::dismiss ? dismiss(conn, a.id)
                                                                                           : archive(conn, a.id);
    if (!updated) {
      if (updated.error() == annotation_error::terminal_status) {
        continue;
      }
      return std::unexpected(updated.error());
    }
    ++count;
  }
  if (!tx->commit())
    return std::unexpected(annotation_error::query_failed);
  return count;
}

auto render_bulk_json(std::string_view verb_name, std::size_t count) -> std::string {
  return std::format(R"({{"ok":true,"action":{},"count":{}}})", json_string(verb_name), count);
}

auto render_bulk_text(std::string_view verb_name, std::size_t count) -> std::string {
  return std::format("{}: {} annotation(s)", verb_name, count);
}

// ---------------------------------------------------------------------------
// sweep
// ---------------------------------------------------------------------------

auto sweep_candidates(db::connection& conn, std::int64_t since_days, std::optional<std::string_view> scope_slug)
    -> std::expected<std::vector<std::int64_t>, annotation_error> {
  // Resolve BEFORE building the statement so an unresolvable slug refuses
  // without archiving anything. `resolve_scope(conn, nullopt)` means
  // "global", which is a filter; "no filter" is the absent optional, so the
  // two are kept apart exactly as `list` keeps them apart.
  std::optional<std::pair<scope_kind, std::optional<std::int64_t>>> scope;
  if (scope_slug.has_value()) {
    auto resolved = resolve_scope(conn, scope_slug);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope = *resolved;
  }

  // The cutoff is interpolated rather than bound, matching the Zig
  // original's `bufPrintZ`; `since_days` is an integer, so no injection
  // surface exists. The scope id is bound.
  std::string sql = std::format("select id from annotations"
                                " where anchor_kind = 'file' and status in ('resolved','dismissed')"
                                "   and (julianday('now') - julianday(updated_at)) > {}",
                                since_days);
  if (scope.has_value()) {
    switch (scope->first) {
    case scope_kind::global:
      sql += " and scope_kind = 'global'";
      break;
    case scope_kind::association:
      sql += " and scope_kind = 'association' and scope_id = ?";
      break;
    case scope_kind::repo:
      sql += " and scope_kind = 'repo' and scope_id = ?";
      break;
    }
  }

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (scope.has_value() && scope->first != scope_kind::global) {
    if (!scope->second.has_value()) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, *scope->second); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }

  std::vector<std::int64_t> ids;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    ids.push_back(stmt->column_int64(0));
  }
  return ids;
}

auto sweep(db::connection& conn, std::int64_t since_days, std::optional<std::string_view> scope_slug)
    -> std::expected<std::size_t, annotation_error> {
  auto ids = sweep_candidates(conn, since_days, scope_slug);
  if (!ids) {
    return std::unexpected(ids.error());
  }
  std::size_t count = 0;
  for (const auto id : *ids) {
    auto archived = archive(conn, id);
    if (!archived) {
      if (archived.error() == annotation_error::terminal_status) {
        continue;
      }
      return std::unexpected(archived.error());
    }
    ++count;
  }
  return count;
}

auto render_sweep_json(std::int64_t since_days, std::size_t swept) -> std::string {
  return std::format(R"({{"ok":true,"action":"sweep","since_days":{},"swept":{}}})", since_days, swept);
}

auto render_sweep_text(std::int64_t since_days, std::size_t swept) -> std::string {
  return std::format("sweep: archived {} annotation(s) older than {} day(s)", swept, since_days);
}

// ---------------------------------------------------------------------------
// verify
// ---------------------------------------------------------------------------

auto classify_anchor(std::string_view stored_text_hash, std::optional<std::string_view> file_contents) -> verify_state {
  if (!file_contents.has_value()) {
    return verify_state::stale;
  }
  if (stored_text_hash.empty()) {
    return verify_state::fresh;
  }
  const auto digest = sha256::hex(*file_contents);
  // The Zig comparison takes the first `min(stored.len, 64)` characters of
  // the computed hex and compares against the WHOLE stored value -- so a
  // stored PREFIX matches, and a stored value longer than 64 characters
  // can never match.
  const auto computed = std::string_view{digest}.substr(0, std::min<std::size_t>(stored_text_hash.size(), 64));
  return stored_text_hash == computed ? verify_state::fresh : verify_state::drifted;
}

auto verify_state_to_text(verify_state s) -> std::string_view {
  switch (s) {
  case verify_state::fresh:
    return "fresh";
  case verify_state::drifted:
    return "drifted";
  case verify_state::stale:
    return "stale";
  }
  return "stale"; // unreachable
}

auto render_verify_json(const std::vector<verify_row>& rows) -> std::string {
  std::string out = R"({"ok":true,"rows":[)";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += std::format(R"({{"id":{},"anchor_path":{},"state":"{}"}})", rows[i].id, json_string(rows[i].anchor_path),
                       verify_state_to_text(rows[i].state));
  }
  out += "]}";
  return out;
}

auto render_verify_text(const std::vector<verify_row>& rows) -> std::string {
  if (rows.empty()) {
    return "(no active annotations)\n";
  }
  std::string out;
  for (const auto& r : rows) {
    out += std::format("annotation:{}  {}  [{}]\n", r.id, r.anchor_path, verify_state_to_text(r.state));
  }
  return out;
}

// ---------------------------------------------------------------------------
// show / list rendering
// ---------------------------------------------------------------------------

auto render_json(const annotation& a) -> std::string {
  std::string out;
  out += std::format(R"({{"id":{},"scope_kind":"{}","scope_id":{},)", a.id, scope_kind_to_text(a.scope_kind_),
                     json_optional_int(a.scope_id));
  out += std::format(R"("anchor":{{"kind":"{}","path":{},"line_start":{},"line_end":{},)",
                     a.anchor_kind_ == anchor_kind::file ? "file" : "entity",
                     a.anchor_kind_ == anchor_kind::file ? json_string(a.anchor.path) : "null",
                     json_optional_int(a.anchor.line_start), json_optional_int(a.anchor.line_end));
  out += std::format(R"("commit_sha":{},"text_hash":{},"text":{}}},)", json_string(a.anchor.commit_sha),
                     json_string(a.anchor.text_hash), json_string(a.anchor.text));
  out += "\"target\":";
  if (a.target.has_value()) {
    out += std::format(R"({{"kind":"{}","id":{}}})", a.target->kind == target_kind::plan ? "plan" : "task", a.target->id);
  } else {
    out += "null";
  }
  out += std::format(R"(,"title":{},"slug":{},"body":{},"status":"{}","vendor":{},"origin":{},"revision":{},)",
                     json_optional_string(a.title), json_optional_string(a.slug), json_string(a.body), status_to_text(a.status_),
                     json_string(a.vendor), json_optional_string(a.origin), a.revision);
  out += std::format(R"("plan_id":{},"task_id":{},"tags":[)", json_optional_int(a.plan_id), json_optional_int(a.task_id));
  for (std::size_t i = 0; i < a.tags.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += json_string(a.tags[i]);
  }
  out += std::format(R"(],"created_at":{},"updated_at":{})", json_string(a.created_at), json_string(a.updated_at));
  if (a.anchor_kind_ == anchor_kind::entity) {
    out += ",\"messages\":[";
    for (std::size_t i = 0; i < a.messages.size(); ++i) {
      if (i)
        out += ',';
      const auto& m = a.messages[i];
      out +=
          std::format(R"({{"id":{},"body":{},"revision":{},"vendor":{},"origin":{},"created_at":{},"updated_at":{},"history":[)",
                      m.id, json_string(m.body), m.revision, json_string(m.vendor), json_optional_string(m.origin),
                      json_string(m.created_at), json_string(m.updated_at));
      for (std::size_t j = 0; j < m.history.size(); ++j) {
        if (j)
          out += ',';
        const auto& revision = m.history[j];
        out += std::format(R"({{"revision":{},"body":{},"updated_at":{}}})", revision.revision, json_string(revision.body),
                           json_string(revision.updated_at));
      }
      out += "]}";
    }
    out += "],\"contextual_anchor\":";
    if (!a.contextual)
      out += "null";
    else {
      const auto& c = *a.contextual;
      out += std::format(
          R"({{"schema_version":1,"document_kind":{},"document_id":{},"document_version":1,"content_revision":{},"kind":{},"start_block_key":{},"end_block_key":{},"start_offset":{},"end_offset":{},"normalized_quote":{},"prefix_context":{},"suffix_context":{},"state":{},"revision":{},"segments":[)",
          json_string(c.document_kind), c.document_id, json_string(c.content_revision), json_string(c.kind),
          json_string(c.start_block_key), json_string(c.end_block_key), json_optional_int(c.start_offset),
          json_optional_int(c.end_offset), json_string(c.normalized_quote), json_string(c.prefix_context),
          json_string(c.suffix_context), json_string(c.state), c.revision);
      for (std::size_t i = 0; i < c.segments.size(); ++i) {
        if (i)
          out += ',';
        out +=
            std::format(R"({{"block_key":{},"quote":{}}})", json_string(c.segments[i].first), json_string(c.segments[i].second));
      }
      out += "]}";
    }
  }
  out += '}';
  return out;
}

auto render_list_json(const std::vector<annotation>& items) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(items[i]);
  }
  out += "]";
  return out;
}

auto render_text(const annotation& a) -> std::string {
  std::string out;
  out += std::format("id:          {}\n", a.id);
  if (a.title.has_value()) {
    out += std::format("title:       {}\n", *a.title);
  }
  if (a.slug.has_value()) {
    out += std::format("slug:        {}\n", *a.slug);
  }
  out += std::format("status:      {}\n", status_to_text(a.status_));
  out += std::format("scope:       {}", scope_kind_to_text(a.scope_kind_));
  if (a.scope_id.has_value()) {
    out += std::format(":{}", *a.scope_id);
  }
  out += "\n";
  out += std::format("anchor path: {}\n", a.anchor.path);
  if (a.anchor.line_start.has_value()) {
    if (a.anchor.line_end.has_value()) {
      out += std::format("anchor line: {}-{}\n", *a.anchor.line_start, *a.anchor.line_end);
    } else {
      out += std::format("anchor line: {}\n", *a.anchor.line_start);
    }
  }
  if (!a.anchor.commit_sha.empty()) {
    out += std::format("anchor sha:  {}\n", a.anchor.commit_sha);
  }
  if (!a.anchor.text_hash.empty()) {
    out += std::format("anchor hash: {}\n", a.anchor.text_hash);
  }
  if (!a.vendor.empty()) {
    out += std::format("vendor:      {}\n", a.vendor);
  }
  if (a.plan_id.has_value()) {
    out += std::format("plan:        {}\n", *a.plan_id);
  }
  if (a.task_id.has_value()) {
    out += std::format("task:        {}\n", *a.task_id);
  }
  if (!a.tags.empty()) {
    out += "tags:        ";
    for (std::size_t i = 0; i < a.tags.size(); ++i) {
      if (i > 0) {
        out += ", ";
      }
      out += a.tags[i];
    }
    out += "\n";
  }
  if (!a.body.empty()) {
    out += std::format("body:        {}\n", a.body);
  }
  out += std::format("created:     {}\n", a.created_at);
  out += std::format("updated:     {}\n", a.updated_at);
  return out;
}

auto render_list_text(const std::vector<annotation>& items) -> std::string {
  if (items.empty()) {
    return "(no annotations)\n";
  }
  std::string out;
  for (const auto& a : items) {
    // Title falls back to the anchor path when the annotation has none.
    const std::string& title = a.title.has_value() ? *a.title : a.anchor.path;
    // zig `{d:>5}  {s:<10}  {s}` over (id, status, title): right-aligned
    // width 5, two spaces, left-aligned width 10, two spaces, title.
    // Oracle: `    1  active      A1`.
    out += std::format("{:>5}  {:<10}  {}\n", a.id, status_to_text(a.status_), title);
  }
  return out;
}

auto render_tag_json(std::int64_t id, std::string_view tag, bool removing) -> std::string {
  return std::format(R"({{"ok":true,"id":{},"tag":{},"action":"{}"}})", id, json_string(tag), removing ? "remove" : "add");
}

auto render_tag_text(std::int64_t id, std::string_view tag, bool removing) -> std::string {
  return std::format("annotation {}: {} tag '{}'", id, removing ? "removed" : "added", tag);
}

auto render_remove_json(std::int64_t id) -> std::string {
  return std::format(R"({{"ok":true,"id":{}}})", id);
}

auto render_remove_text(std::int64_t id) -> std::string {
  return std::format("annotation {} removed", id);
}

} // namespace planar::engine::planning::annotation
