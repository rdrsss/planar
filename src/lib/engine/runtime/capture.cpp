/// @file capture.cpp
/// @brief Implementation of `planar.engine.runtime.capture` (plan 996,
/// task 6094). See capture.cppm for scope, cut list, and the
/// oracle-captured output shapes.

module planar.engine.runtime.capture;

import std;
import planar.json_text;
import planar.db;
import planar.engine.runtime.session;
import planar.engine.runtime.sessioncommits;
import planar.engine.runtime.snapshot;

namespace planar::engine::runtime::capture {

// The one shared escape table, layer 1. See json_text.cppm -- the local
// copy this replaced was missing the \b and \f short forms.
using json_text::json_string;

namespace {

auto from_session_error(session::session_error e) -> capture_error {
  switch (e) {
  case session::session_error::not_found:
    return capture_error::not_found;
  case session::session_error::already_ended:
    return capture_error::already_ended;
  case session::session_error::task_conflict:
    return capture_error::task_conflict;
  case session::session_error::query_failed:
    return capture_error::query_failed;
  }
  return capture_error::query_failed;
}

/// @brief Map a `sessioncommits::commits_error` onto this module's error
/// surface. 1:1 -- see `sessioncommits.cppm` for what each member means.
auto from_commits_error(sessioncommits::commits_error e) -> capture_error {
  switch (e) {
  case sessioncommits::commits_error::not_git:
    return capture_error::not_git;
  case sessioncommits::commits_error::git_failed:
    return capture_error::git_failed;
  case sessioncommits::commits_error::malformed_output:
    return capture_error::malformed_output;
  case sessioncommits::commits_error::query_failed:
    return capture_error::query_failed;
  }
  return capture_error::query_failed;
}

auto from_snapshot_error(snapshot::snapshot_error e) -> capture_error {
  switch (e) {
  case snapshot::snapshot_error::not_found:
    return capture_error::not_found;
  case snapshot::snapshot_error::query_failed:
    return capture_error::query_failed;
  }
  return capture_error::query_failed;
}

/// @brief The vendor tuple every `capture` leaf derives from the
/// environment when no explicit session was named.
struct env_vendor_tuple {
  std::string                vendor;
  std::optional<std::string> vendor_session_id;
};

auto env_tuple() -> env_vendor_tuple {
  return env_vendor_tuple{.vendor = session::vendor_from_env(), .vendor_session_id = session::vendor_session_id_from_env()};
}

auto as_view(const std::optional<std::string>& s) -> std::optional<std::string_view> {
  if (!s.has_value()) {
    return std::nullopt;
  }
  return std::string_view{*s};
}

} // namespace

auto open_session(db::connection& conn, const open_args& args, std::optional<start_git_context> git)
    -> std::expected<session::session, capture_error> {
  auto started = session::start_session(conn, session::start_args{.vendor            = args.vendor,
                                                                  .vendor_session_id = args.vendor_session_id,
                                                                  .task_id           = args.task_id,
                                                                  .model             = args.model});
  if (!started) {
    return std::unexpected(from_session_error(started.error()));
  }

  if (git.has_value()) {
    // Best-effort, exactly as the Zig original: a failed stamp must not
    // lose the session. Re-read afterwards so the caller sees the stamped
    // row (the Zig code discards and re-`getById`s for the same reason).
    (void)session::set_start_git_context_if_unset(conn, started->id, git->repo_root, git->head_sha_at_start);
    auto reread = session::get_by_id(conn, started->id);
    if (reread) {
      started = std::move(reread);
    }
  }

  // Best-effort marker; a failure here is swallowed.
  (void)session::append_entry(conn, started->id, "action", "session opened");
  return std::move(*started);
}

auto close_session(db::connection& conn, std::int64_t session_id, std::optional<std::string_view> summary)
    -> std::expected<void, capture_error> {
  // `recordSessionWindow` (git commit harvest) is deliberately absent --
  // see capture.cppm's cut list. It is a no-op for any session this tree
  // can open, since `repo_root` / `head_sha_at_start` are only ever
  // stamped by a caller that can spawn `git`.
  (void)session::append_entry(conn, session_id, "note", "session ended");
  auto ended = session::end_session(conn, session_id, summary);
  if (!ended) {
    return std::unexpected(from_session_error(ended.error()));
  }
  return {};
}

auto append_note(db::connection& conn, std::int64_t session_id, std::string_view body) -> std::expected<void, capture_error> {
  auto ok = session::append_entry(conn, session_id, "note", body);
  if (!ok) {
    return std::unexpected(from_session_error(ok.error()));
  }
  return {};
}

auto append_command(db::connection& conn, std::int64_t session_id, std::string_view body) -> std::expected<void, capture_error> {
  auto ok = session::append_entry(conn, session_id, "command", body);
  if (!ok) {
    return std::unexpected(from_session_error(ok.error()));
  }
  return {};
}

auto append_file(db::connection& conn, std::int64_t session_id, std::string_view body) -> std::expected<void, capture_error> {
  auto ok = session::append_entry(conn, session_id, "file", body);
  if (!ok) {
    return std::unexpected(from_session_error(ok.error()));
  }
  return {};
}

auto take_snapshot(db::connection& conn, const snapshot_args& args) -> std::expected<snapshot::snapshot, capture_error> {
  auto snap = snapshot::create(conn, snapshot::create_args{.session_id        = args.session_id,
                                                           .task_id           = args.task_id,
                                                           .vendor            = args.vendor,
                                                           .vendor_session_id = args.vendor_session_id,
                                                           .body              = args.body,
                                                           .next_action       = args.next_action});
  if (!snap) {
    return std::unexpected(from_snapshot_error(snap.error()));
  }
  // Best-effort note; the snapshot is returned regardless.
  (void)session::append_entry(conn, args.session_id, "note", std::format("snapshot created: id={}", snap->id));
  return std::move(*snap);
}

auto resolve_session_id(db::connection& conn, std::optional<std::int64_t> explicit_session_id)
    -> std::expected<std::int64_t, capture_error> {
  if (explicit_session_id.has_value()) {
    return *explicit_session_id;
  }
  const auto tuple = env_tuple();
  auto       id    = session::ensure_active(conn, tuple.vendor, as_view(tuple.vendor_session_id));
  if (!id) {
    return std::unexpected(from_session_error(id.error()));
  }
  return *id;
}

auto resolve_existing_session_id(db::connection& conn, std::optional<std::int64_t> explicit_session_id)
    -> std::expected<std::int64_t, capture_error> {
  if (explicit_session_id.has_value()) {
    return *explicit_session_id;
  }
  const auto tuple = env_tuple();
  auto       found = session::active_for_vendor(conn, tuple.vendor, as_view(tuple.vendor_session_id));
  if (!found) {
    return std::unexpected(from_session_error(found.error()));
  }
  if (!found->has_value()) {
    return std::unexpected(capture_error::no_active_session);
  }
  return (*found)->id;
}

auto compose_command_body(std::string_view command, std::optional<std::string_view> outcome) -> std::string {
  if (outcome.has_value()) {
    return std::format("{}\noutcome: {}", command, *outcome);
  }
  return std::string{command};
}

auto compose_file_body(std::string_view path, std::optional<std::string_view> role) -> std::string {
  if (role.has_value()) {
    return std::format("{} [{}]", path, *role);
  }
  return std::string{path};
}

auto render_session_json(const session::session& s) -> std::string {
  std::string out = std::format(R"({{"ok":true,"id":{},"vendor":{})", s.id, json_string(s.vendor));
  if (s.vendor_session_id.has_value()) {
    out += std::format(R"(,"vendor_session_id":{})", json_string(*s.vendor_session_id));
  }
  if (s.task_id.has_value()) {
    out += std::format(R"(,"task_id":{})", *s.task_id);
  }
  out += "}\n";
  return out;
}

auto render_session_text(const session::session& s) -> std::string {
  std::string out = std::format("session {} opened (vendor: {}", s.id, s.vendor);
  if (s.vendor_session_id.has_value()) {
    out += std::format(", vsid: {}", *s.vendor_session_id);
  }
  if (s.task_id.has_value()) {
    out += std::format(", task: {}", *s.task_id);
  }
  out += ")\n";
  return out;
}

auto render_append_json(std::int64_t session_id) -> std::string {
  return std::format("{{\"ok\":true,\"session_id\":{}}}\n", session_id);
}

auto render_append_text(std::string_view what, std::int64_t session_id) -> std::string {
  return std::format("captured {} in session {}\n", what, session_id);
}

auto render_snapshot_json(const snapshot::snapshot& snap) -> std::string {
  std::string out =
      std::format(R"({{"ok":true,"id":{},"session_id":{},"vendor":{})", snap.id, snap.session_id, json_string(snap.vendor));
  if (snap.task_id.has_value()) {
    out += std::format(R"(,"task_id":{})", *snap.task_id);
  }
  out += "}\n";
  return out;
}

auto render_snapshot_text(const snapshot::snapshot& snap) -> std::string {
  std::string out = std::format("snapshot {} created (vendor: {}", snap.id, snap.vendor);
  if (snap.task_id.has_value()) {
    out += std::format(", task: {}", *snap.task_id);
  }
  out += ")\n";
  return out;
}

auto render_end_json(std::int64_t session_id) -> std::string {
  return std::format("{{\"ok\":true,\"id\":{}}}\n", session_id);
}

auto render_end_text(std::int64_t session_id) -> std::string {
  return std::format("session {} ended\n", session_id);
}

auto resolve_next_action(db::connection& conn, std::optional<std::string_view> explicit_next_action,
                         std::optional<std::int64_t> task_id) -> std::expected<std::optional<std::string>, capture_error> {
  if (explicit_next_action.has_value()) {
    return std::optional<std::string>{std::string{*explicit_next_action}};
  }
  if (!task_id.has_value()) {
    return std::optional<std::string>{};
  }
  auto stmt = conn.prepare("select coalesce(next_action,'') from tasks where id = ?");
  if (!stmt) {
    return std::unexpected(capture_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, *task_id); !b) {
    return std::unexpected(capture_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(capture_error::query_failed);
  }
  if (*step != db::step_result::row) {
    // A missing task is NOT an error here -- the Zig handler's `.done`
    // arm is an empty block, leaving `next_action` null.
    return std::optional<std::string>{};
  }
  auto value = stmt->column_text(0);
  if (value.empty()) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{std::move(value)};
}

auto record_commits(db::connection& conn, const record_commits_args& args)
    -> std::expected<record_commits_result, capture_error> {
  auto const found = session::get_by_id(conn, args.session_id);
  if (!found) {
    return std::unexpected(from_session_error(found.error()));
  }

  std::filesystem::path const dir{std::string{args.repo_dir}};

  auto repo_root = sessioncommits::resolve_repo_root_strict(dir);
  if (!repo_root) {
    return std::unexpected(from_commits_error(repo_root.error()));
  }

  auto commits = args.since.has_value() ? sessioncommits::walk_strict(dir, *args.since, std::string_view{*repo_root})
                                        : sessioncommits::resolve_shas(dir, args.shas, std::string_view{*repo_root});
  if (!commits) {
    return std::unexpected(from_commits_error(commits.error()));
  }

  // The whole persist step is one transaction, mirroring the Zig
  // original's `begin immediate` / `commit`: a mid-batch SQL failure
  // rolls back rather than leaving a partial insert.
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(capture_error::query_failed);
  }
  auto const inserted = sessioncommits::record_count(conn, args.session_id, std::nullopt, *commits);
  if (!inserted) {
    return std::unexpected(from_commits_error(inserted.error()));
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(capture_error::query_failed);
  }

  return record_commits_result{
      .session_id     = args.session_id,
      .repo_root      = std::move(*repo_root),
      .commit_count   = commits->size(),
      .inserted_count = *inserted,
  };
}

auto render_commits_json(const record_commits_result& result) -> std::string {
  return std::format(R"({{"ok":true,"session_id":{},"repo_root":{},"commit_count":{},"inserted_count":{}}})"
                     "\n",
                     result.session_id, json_string(result.repo_root), result.commit_count, result.inserted_count);
}

auto render_commits_text(const record_commits_result& result) -> std::string {
  return std::format("session {}: processed {} commits ({} new)\n", result.session_id, result.commit_count,
                     result.inserted_count);
}

} // namespace planar::engine::runtime::capture
