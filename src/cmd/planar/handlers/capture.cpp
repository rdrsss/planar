/// @file capture.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.capture`.

module planar.cmd.planar.handlers.capture;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.git;
import planar.engine.runtime.capture;
import planar.engine.runtime.session;
import planar.engine.runtime.snapshot;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace cap  = engine::runtime::capture;
namespace sess = engine::runtime::session;

namespace {

using kind_t = domain_error_kind;

/// @brief `--json` was passed.
auto wants_json(const cliapp::parsed_args& args) -> bool {
  return flag_bool(args, "--json");
}

/// @brief A `std::optional<std::string>` viewed as an optional
/// `string_view`, for handing an owned buffer to an engine `args` struct
/// without copying. The referent must outlive the view.
auto as_view(const std::optional<std::string>& value) -> std::optional<std::string_view> {
  if (!value) {
    return std::nullopt;
  }
  return std::string_view{*value};
}

/// @brief The oracle's error body for a `capture_error`, in the shape the
/// Zig handler's `exit.die(ctx, e, "<verb>: {s}", @errorName(e))` produces
/// for the cases its own `switch` does not special-case.
auto generic_body(std::string_view verb, cap::capture_error err) -> std::string {
  std::string_view name = "QueryFailed";
  switch (err) {
  case cap::capture_error::not_found:
    name = "NotFound";
    break;
  case cap::capture_error::already_ended:
    name = "AlreadyEnded";
    break;
  case cap::capture_error::task_conflict:
    name = "TaskConflict";
    break;
  case cap::capture_error::no_active_session:
    name = "NoActiveSession";
    break;
  case cap::capture_error::not_git:
    name = "NotGit";
    break;
  case cap::capture_error::git_failed:
    name = "GitFailed";
    break;
  case cap::capture_error::malformed_output:
    name = "MalformedOutput";
    break;
  case cap::capture_error::query_failed:
    break;
  }
  return std::format("{}: {}", verb, name);
}

/// @brief Resolve the session id an append-style leaf targets: an explicit
/// `--session` wins, otherwise the vendor tuple's active session, CREATED
/// when absent.
///
/// The creating variant. See this module's header for why `end` does not
/// share it.
auto resolve_or_create_session(context& ctx, db::connection& conn, const cliapp::parsed_args& args)
    -> std::expected<std::int64_t, domain_error> {
  if (auto const explicit_id = flag_int(args, "--session")) {
    return *explicit_id;
  }
  auto const tuple = resolve_vendor_tuple(ctx.env());
  auto const id    = sess::ensure_active(conn, tuple.vendor, as_view(tuple.vendor_session_id));
  if (!id) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "ensureActive: QueryFailed"));
  }
  return *id;
}

/// @brief The shared tail of `note` / `command` / `file`: append the
/// composed body, then write the leaf's payload.
auto append_and_report(context& ctx, const cliapp::parsed_args& args, std::string_view what, std::int64_t session_id,
                       std::expected<void, cap::capture_error> appended) -> handler_result {
  if (!appended) {
    return std::unexpected(
        error_from_body(kind_t::generic_failure, generic_body(std::format("capture {}", what), appended.error())));
  }
  if (wants_json(args)) {
    ctx.out() << cap::render_append_json(session_id);
  } else {
    ctx.out() << cap::render_append_text(what, session_id);
  }
  return {};
}

} // namespace

auto resolve_vendor_tuple(const env_lookup& env) -> vendor_tuple {
  vendor_tuple result{.vendor = "cli", .vendor_session_id = std::nullopt};
  if (auto const raw = env("PLANAR_VENDOR"); raw && !raw->empty()) {
    result.vendor = *raw;
  }
  if (auto const raw = env("PLANAR_VENDOR_SESSION_ID"); raw && !raw->empty()) {
    result.vendor_session_id = *raw;
  }
  return result;
}

auto capture_session(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // `--vendor` wins over `$PLANAR_VENDOR`, which wins over `"cli"`; and
  // `--vendor-session-id` wins over `$PLANAR_VENDOR_SESSION_ID`. That
  // precedence is this leaf's alone — the other five have no such flags.
  auto tuple = resolve_vendor_tuple(ctx.env());
  if (auto const flag = flag_string(args, "--vendor")) {
    tuple.vendor = *flag;
  }
  if (auto const flag = flag_string(args, "--vendor-session-id")) {
    tuple.vendor_session_id = *flag;
  }
  auto const model = flag_string(args, "--model");

  // The git start-context (task 6128). `cap::open_session`'s third
  // argument defaults to `std::nullopt`, and taking that default is what
  // made this verb SILENTLY write `repo_root = NULL` and
  // `head_sha_at_start = NULL` on every invocation while exiting 0 with
  // byte-identical stdout to the oracle. Downstream, `capture commits` is
  // documented as a no-op whenever either column is NULL, so those sessions
  // looked captured and reconciled to nothing.
  //
  // Best-effort by design, exactly as zig's `openSession` is: `probe`
  // reports unset outside a repository or before the first commit, and the
  // stamping step is then skipped rather than half-applied. That is NOT the
  // silent degradation above — the oracle behaves identically in the same
  // situation, and the row it writes there is the same row this writes.
  auto const                            git_probe = git::probe_start_context(ctx.cwd());
  std::optional<cap::start_git_context> git_ctx;
  if (git_probe.has_value()) {
    git_ctx = cap::start_git_context{
        .repo_root         = git_probe->repo_root,
        .head_sha_at_start = git_probe->head_sha_at_start,
    };
  }

  auto opened = cap::open_session(**conn,
                                  cap::open_args{
                                      .vendor            = tuple.vendor,
                                      .vendor_session_id = as_view(tuple.vendor_session_id),
                                      .task_id           = flag_int(args, "--task"),
                                      .model             = as_view(model),
                                  },
                                  git_ctx);
  if (!opened) {
    if (opened.error() == cap::capture_error::task_conflict) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "session already bound to a different task"));
    }
    return std::unexpected(error_from_body(kind_t::generic_failure, generic_body("capture session", opened.error())));
  }

  if (wants_json(args)) {
    ctx.out() << cap::render_session_json(*opened);
  } else {
    ctx.out() << cap::render_session_text(*opened);
  }
  return {};
}

auto capture_end(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // Resolution order, from end.zig: the `<session-id>` positional, then
  // `--session`, then the vendor tuple's active session. The positional is
  // a STRING so the oracle's own exit-2 wording survives; see the header.
  std::int64_t session_id = 0;
  if (auto const raw = positional_string(args, "session-id")) {
    auto const parsed = cliapp::parse_int64_zig(*raw);
    if (!parsed) {
      return std::unexpected(
          error_from_body(kind_t::invalid_input, std::format("session id must be an integer, got '{}'", *raw)));
    }
    session_id = *parsed;
  } else if (auto const flag = flag_int(args, "--session")) {
    session_id = *flag;
  } else {
    auto const tuple = resolve_vendor_tuple(ctx.env());
    auto const found = sess::active_for_vendor(**conn, tuple.vendor, as_view(tuple.vendor_session_id));
    if (!found) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "finding active session: QueryFailed"));
    }
    if (!found->has_value()) {
      return std::unexpected(error_from_body(kind_t::not_found, "no active session"));
    }
    session_id = (*found)->id;
  }

  auto const summary = flag_string(args, "--summary");
  auto       closed  = cap::close_session(**conn, session_id, as_view(summary));
  if (!closed) {
    switch (closed.error()) {
    case cap::capture_error::not_found:
      return std::unexpected(error_from_body(kind_t::not_found, std::format("session {} not found", session_id)));
    case cap::capture_error::already_ended:
      return std::unexpected(error_from_body(kind_t::generic_failure, std::format("session {} is already ended", session_id)));
    default:
      return std::unexpected(error_from_body(kind_t::generic_failure, generic_body("capture end", closed.error())));
    }
  }

  if (wants_json(args)) {
    ctx.out() << cap::render_end_json(session_id);
  } else {
    ctx.out() << cap::render_end_text(session_id);
  }
  return {};
}

auto capture_note(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const session_id = resolve_or_create_session(ctx, **conn, args);
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto const body = positional_string(args, "body").value_or("");
  return append_and_report(ctx, args, "note", *session_id, cap::append_note(**conn, *session_id, body));
}

auto capture_command(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const session_id = resolve_or_create_session(ctx, **conn, args);
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto const command = positional_string(args, "command").value_or("");
  auto const outcome = flag_string(args, "--outcome");
  auto const body    = cap::compose_command_body(command, as_view(outcome));
  return append_and_report(ctx, args, "command", *session_id, cap::append_command(**conn, *session_id, body));
}

auto capture_file(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const session_id = resolve_or_create_session(ctx, **conn, args);
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto const path = positional_string(args, "path").value_or("");
  auto const role = flag_string(args, "--role");
  auto const body = cap::compose_file_body(path, as_view(role));
  return append_and_report(ctx, args, "file", *session_id, cap::append_file(**conn, *session_id, body));
}

auto capture_snapshot(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const session_id = resolve_or_create_session(ctx, **conn, args);
  if (!session_id) {
    return std::unexpected(session_id.error());
  }

  // `--task` wins; otherwise the session's own bound task. A session
  // lookup failure is fatal here, matching snapshot.zig's `exit.die`.
  auto task_id = flag_int(args, "--task");
  if (!task_id) {
    auto const found = sess::get_by_id(**conn, *session_id);
    if (!found) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "session lookup: NotFound"));
    }
    task_id = found->task_id;
  }

  // `--note` takes precedence over the `<body>` positional. Both absent
  // stores SQL NULL, which the engine's `create_args` already expresses.
  auto body = flag_string(args, "--note");
  if (!body) {
    body = positional_string(args, "body");
  }

  // Bound to a named local FIRST: `as_view` returns a view INTO the
  // optional it is handed, so passing the `flag_string` temporary
  // directly would hand the engine a view of a destroyed buffer.
  auto const explicit_next = flag_string(args, "--next-action");
  auto const next_action   = cap::resolve_next_action(**conn, as_view(explicit_next), task_id);
  if (!next_action) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "task lookup: QueryFailed"));
  }

  auto const tuple = resolve_vendor_tuple(ctx.env());
  auto       taken = cap::take_snapshot(**conn, cap::snapshot_args{
                                                    .session_id        = *session_id,
                                                    .task_id           = task_id,
                                                    .vendor            = tuple.vendor,
                                                    .vendor_session_id = as_view(tuple.vendor_session_id),
                                                    .body              = as_view(body),
                                                    .next_action       = as_view(*next_action),
                                                });
  if (!taken) {
    return std::unexpected(error_from_body(kind_t::generic_failure, generic_body("capture snapshot", taken.error())));
  }

  if (wants_json(args)) {
    ctx.out() << cap::render_snapshot_json(*taken);
  } else {
    ctx.out() << cap::render_snapshot_text(*taken);
  }
  return {};
}

auto capture_commits(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const since = flag_string(args, "--since");
  auto const shas  = positional_strings(args, "shas");

  // The two validation checks the oracle's own handler runs BEFORE
  // touching the engine (commits.zig:15-20) -- `record_commits` does not
  // re-validate them; see capture.cppm's header.
  if (since.has_value() && !shas.empty()) {
    return std::unexpected(error_from_body(kind_t::invalid_input, "cannot combine --since with explicit commit SHAs"));
  }
  if (!since.has_value() && shas.empty()) {
    return std::unexpected(error_from_body(kind_t::invalid_input, "provide --since <ref> or one or more commit SHAs"));
  }

  // Resolution order, from the Zig handler's `resolveTargetSessionId`:
  // explicit `--session` wins; otherwise the vendor tuple's ACTIVE
  // session -- NOT created on demand, unlike the four append-style
  // leaves' `resolve_or_create_session`. See this module's header.
  std::int64_t session_id = 0;
  if (auto const explicit_id = flag_int(args, "--session")) {
    session_id = *explicit_id;
  } else {
    auto const tuple = resolve_vendor_tuple(ctx.env());
    auto const found = sess::active_for_vendor(**conn, tuple.vendor, as_view(tuple.vendor_session_id));
    if (!found) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "finding active session: QueryFailed"));
    }
    if (!found->has_value()) {
      return std::unexpected(error_from_body(kind_t::invalid_input, "no active session (run `planar capture session` first)"));
    }
    session_id = (*found)->id;
  }

  auto const repo_dir = flag_string(args, "--repo").value_or(".");

  // `sha_views` must outlive `record_commits`: `record_commits_args::shas`
  // is a non-owning span of `string_view`s viewing INTO `shas`' buffers.
  std::vector<std::string_view> sha_views;
  sha_views.reserve(shas.size());
  for (auto const& sha : shas) {
    sha_views.push_back(sha);
  }

  auto result = cap::record_commits(**conn, cap::record_commits_args{
                                                .session_id = session_id,
                                                .repo_dir   = repo_dir,
                                                .since      = as_view(since),
                                                .shas       = sha_views,
                                            });
  if (!result) {
    // Special-cased exactly the way commits.zig's own `switch` special-cases
    // them; everything else falls through to the generic body, matching the
    // oracle's default `exit.die(ctx, e, "capture commits: {s}", ...)` arm.
    switch (result.error()) {
    case cap::capture_error::not_found:
      return std::unexpected(error_from_body(kind_t::not_found, std::format("session {} not found", session_id)));
    case cap::capture_error::not_git:
      return std::unexpected(error_from_body(kind_t::generic_failure, std::format("repo is not a git repository: {}", repo_dir)));
    case cap::capture_error::git_failed:
      if (since.has_value()) {
        return std::unexpected(error_from_body(kind_t::generic_failure, std::format("cannot resolve ref '{}'", *since)));
      }
      return std::unexpected(error_from_body(kind_t::generic_failure, "one or more commit SHAs could not be resolved"));
    default:
      return std::unexpected(error_from_body(kind_t::generic_failure, generic_body("capture commits", result.error())));
    }
  }

  if (wants_json(args)) {
    ctx.out() << cap::render_commits_json(*result);
  } else {
    ctx.out() << cap::render_commits_text(*result);
  }
  return {};
}

} // namespace planar::cmd::handlers
