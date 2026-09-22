/// @file runs.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.runs`.

module planar.cmd.planar.handlers.runs;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.runs.lifecycle;
import planar.engine.runs.render;
import planar.engine.runs.harvest;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace life = engine::runs::lifecycle;
namespace rend = engine::runs::render;
namespace harv = engine::runs::harvest;

namespace {

using kind_t = domain_error_kind;

/// @brief `--json` was passed.
/// @param args The parsed arguments.
/// @return Whether the flag is set.
auto wants_json(const cliapp::parsed_args& args) -> bool {
  return flag_bool(args, "--json");
}

/// @brief The `<run-uid>` positional. Declared `required` on every leaf
/// that has one, so CLI11 refuses an invocation without it before dispatch
/// and the empty fallback is unreachable in practice.
/// @param args The parsed arguments.
/// @return The uid.
auto uid_of(const cliapp::parsed_args& args) -> std::string {
  return cliapp::positional_string(args, "run-uid").value_or("");
}

/// @brief Map a `runs_error` to this binary's exit-code bucket and stderr
/// body, given the leaf and the uid it was working on.
///
/// One mapper, but NOT one bucket: `duplicate_run_uid` and `duplicate_seq`
/// land in `already_exists` (exit 6) while `not_found` lands in `not_found`
/// (exit 1) and `query_failed` in `generic_failure` (exit 1). The two
/// duplicate arms carry their own oracle wording; the `query_failed` arm
/// carries the family's generic `<leaf>: QueryFailed` line — see this
/// module's header for the `std.log` companion line that is deliberately
/// NOT reproduced.
/// @param leaf The leaf name, e.g. `"bench touch"`.
/// @param run_uid The uid under operation, for the wording that names it.
/// @param seq The seq under operation, for the duplicate-seq wording.
/// @param err The engine failure.
/// @return The domain error to return.
auto error_for(std::string_view leaf, std::string_view run_uid, std::int64_t seq, life::runs_error err) -> domain_error {
  switch (err) {
  case life::runs_error::not_found:
    return error_from_body(kind_t::not_found, rend::render_run_not_found(leaf, run_uid));
  case life::runs_error::duplicate_run_uid:
    return error_from_body(kind_t::already_exists, rend::render_duplicate_run_uid(run_uid));
  case life::runs_error::duplicate_seq:
    return error_from_body(kind_t::already_exists, rend::render_duplicate_seq(seq, run_uid));
  case life::runs_error::query_failed:
    break;
  }
  return error_from_body(kind_t::generic_failure, std::format("{}: QueryFailed", leaf));
}

/// @brief Resolve a `<run-uid>` to its row, reporting the family's shared
/// not-found wording under the caller's leaf name.
/// @param conn An open connection.
/// @param leaf The leaf name, for the message.
/// @param run_uid The uid to resolve.
/// @return The run row, or the refusal.
auto resolve_run(db::connection& conn, std::string_view leaf, std::string_view run_uid)
    -> std::expected<life::run, domain_error> {
  auto found = life::show_by_uid(conn, run_uid);
  if (!found) {
    return std::unexpected(error_for(leaf, run_uid, 0, found.error()));
  }
  return *found;
}

/// @brief Validate an optional raw-JSON flag, refusing at exit 2 with the
/// blob echoed.
///
/// `--payload` and `--config-json` are embedded VERBATIM into the enclosing
/// object by the renderers rather than escaped as strings, so an invalid
/// blob would produce invalid JSON downstream. The oracle refuses up front
/// and so does this.
/// @param args The parsed arguments.
/// @param leaf The leaf name, for the message.
/// @param flag The flag name including dashes.
/// @return The blob when present and valid, unset when absent, or the
/// refusal when present and malformed.
auto checked_json_flag(const cliapp::parsed_args& args, std::string_view leaf, std::string_view flag)
    -> std::expected<std::optional<std::string>, domain_error> {
  auto const raw = cliapp::flag_string(args, flag);
  if (!raw) {
    return std::optional<std::string>{};
  }
  if (!rend::is_valid_json_payload(*raw)) {
    return std::unexpected(error_from_body(kind_t::invalid_input, rend::render_invalid_json(leaf, flag, *raw)));
  }
  return raw;
}

/// @brief The shared body of `bench finish` and `run finish`: validate the
/// status, resolve the uid, write, and report through the leaf's own
/// renderer.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param leaf The leaf name, for every message.
/// @param report Called with the resolved uid and status to produce stdout.
/// @return Success, or the refusal.
auto finish_common(context& ctx, const cliapp::parsed_args& args, std::string_view leaf,
                   const std::function<std::string(std::string_view, std::string_view)>& report) -> handler_result {
  auto const status = cliapp::flag_string(args, "--status").value_or("");
  // BEFORE opening the database: the oracle refuses a bad `--status`
  // without touching SQLite, and `db_open` is asserted in the tests.
  if (!rend::is_valid_terminal_status(status)) {
    return std::unexpected(error_from_body(kind_t::invalid_input, rend::render_invalid_status(leaf, status)));
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const uid   = uid_of(args);
  auto const found = resolve_run(**conn, leaf, uid);
  if (!found) {
    return std::unexpected(found.error());
  }
  auto const done = life::finish(**conn, found->id, status);
  if (!done) {
    return std::unexpected(error_for(leaf, uid, 0, done.error()));
  }
  ctx.out() << report(uid, status);
  return {};
}

} // namespace

auto bench_start(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const arm = cliapp::flag_string(args, "--arm").value_or("");

  // ORDER IS OBSERVABLE, and this is the oracle's. The arm warning comes
  // FIRST — before either refusal — so an invocation that is both
  // unrecognized-arm and malformed prints the warn line and THEN the
  // error. Then `--config-json`, then `--task`. Captured three ways
  // because a single probe cannot distinguish the orderings:
  //
  //   --arm s   --config-json notjson                 -> warn, then config-json error
  //   --arm s   --config-json notjson --task notanint -> warn, then config-json error
  //   --arm strict --config-json notjson --task notanint -> config-json error alone
  //
  // The middle case is the one that pins `--config-json` ahead of
  // `--task`; the first pins the warning ahead of both. An earlier
  // revision of this handler validated `--task` first and diverged on
  // exactly the middle case — the exit code and stdout were identical, so
  // only a stderr byte diff saw it.
  if (!rend::is_known_arm(arm)) {
    ctx.err() << rend::render_unknown_arm_warning(arm) << '\n';
  }

  auto config_json = checked_json_flag(args, "bench start", "--config-json");
  if (!config_json) {
    return std::unexpected(config_json.error());
  }

  // The REPEATABLE `--task` filter. Declared `(string)` + list on the tree
  // (CLI11 rejects a repeated single-valued option outright), so the
  // integer conversion is this leaf's job and its refusal wording is the
  // oracle's, not the parser's. `flag_strings` reads the WHOLE vector —
  // `flag_string` would keep only the last id and silently narrow the
  // snapshot with identical stdout.
  std::optional<std::vector<std::int64_t>> task_filter;
  if (auto const raw = cliapp::flag_strings(args, "--task"); !raw.empty()) {
    std::vector<std::int64_t> ids;
    ids.reserve(raw.size());
    for (auto const& one : raw) {
      auto const parsed = cliapp::parse_int64_zig(one);
      if (!parsed) {
        return std::unexpected(error_from_body(kind_t::invalid_input, rend::render_invalid_task_id(one)));
      }
      ids.push_back(*parsed);
    }
    task_filter = std::move(ids);
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const uid         = uid_of(args);
  auto const corpus_repo = cliapp::flag_string(args, "--corpus-repo");
  auto const started =
      life::start(**conn, life::start_args{
                              .run_uid     = uid,
                              .plan_id     = cliapp::flag_int(args, "--plan").value_or(0),
                              .arm         = arm,
                              .base_sha    = cliapp::flag_string(args, "--base-sha").value_or(""),
                              .config_hash = cliapp::flag_string(args, "--config-hash").value_or(""),
                              .config_json = *config_json ? std::optional<std::string_view>{**config_json} : std::nullopt,
                              .corpus_repo = corpus_repo ? std::optional<std::string_view>{*corpus_repo} : std::nullopt,
                              .status      = std::nullopt,
                              .task_filter = std::move(task_filter),
                          });
  if (!started) {
    return std::unexpected(error_for("bench start", uid, 0, started.error()));
  }
  ctx.out() << rend::render_bench_start(started->run_uid);
  return {};
}

auto bench_event(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto payload = checked_json_flag(args, "bench event", "--payload");
  if (!payload) {
    return std::unexpected(payload.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const uid   = uid_of(args);
  auto const found = resolve_run(**conn, "bench event", uid);
  if (!found) {
    return std::unexpected(found.error());
  }
  // The seq is the CALLER's here — that is the whole difference from
  // `run event`, which auto-increments. A duplicate is a named exit-6
  // refusal, not a silent overwrite.
  auto const seq      = cliapp::flag_int(args, "--seq").value_or(0);
  auto const appended = life::event(**conn, found->id, seq, cliapp::flag_string(args, "--kind").value_or(""),
                                    *payload ? std::optional<std::string_view>{**payload} : std::nullopt);
  if (!appended) {
    return std::unexpected(error_for("bench event", uid, seq, appended.error()));
  }
  ctx.out() << rend::render_bench_ok();
  return {};
}

auto bench_touch(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const raw_kind = cliapp::flag_string(args, "--kind").value_or("");
  auto const kind     = life::touch_kind_from_text(raw_kind);
  if (!kind) {
    return std::unexpected(error_from_body(kind_t::invalid_input, rend::render_invalid_touch_kind(raw_kind)));
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const uid   = uid_of(args);
  auto const found = resolve_run(**conn, "bench touch", uid);
  if (!found) {
    return std::unexpected(found.error());
  }
  // `touch`, NOT `touch_idempotent`: a repeat tuple must trip the UNIQUE
  // constraint and surface as an exit-1 `QueryFailed`, which is what the
  // oracle does. The idempotent variant is `bench harvest`'s write
  // primitive, where a re-harvest must be a silent success rather than a
  // reported failure.
  auto const written = life::touch(**conn, found->id, cliapp::flag_int(args, "--task").value_or(0),
                                   cliapp::flag_string(args, "--path").value_or(""), *kind);
  if (!written) {
    return std::unexpected(error_for("bench touch", uid, 0, written.error()));
  }
  ctx.out() << rend::render_bench_ok();
  return {};
}

auto bench_harvest(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const base = cliapp::flag_string(args, "--base");
  auto const head = cliapp::flag_string(args, "--head");
  // Checked BEFORE the database is opened, mirroring the oracle: this is
  // the one refusal on this leaf the reference binary raises without
  // touching SQLite at all.
  if (base.has_value() != head.has_value()) {
    return std::unexpected(error_from_body(kind_t::invalid_input, rend::render_base_head_mismatch()));
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const uid   = uid_of(args);
  auto const found = resolve_run(**conn, "bench harvest", uid);
  if (!found) {
    return std::unexpected(found.error());
  }

  auto const            worktree = cliapp::flag_string(args, "--worktree").value_or("");
  harv::diff_spec const spec =
      base.has_value() ? harv::diff_spec{harv::diff_range{.base = *base, .head = *head}} : harv::diff_spec{std::nullopt};

  auto const n = harv::harvest(**conn, harv::harvest_args{
                                           .worktree = worktree,
                                           .run_id   = found->id,
                                           .task_id  = cliapp::flag_int(args, "--task").value_or(0),
                                           .spec     = spec,
                                       });
  if (!n) {
    // `query_failed` carries the family's generic wording — this leaf's
    // write goes through `touch_idempotent`, so in practice only a schema-
    // level failure (not a duplicate tuple, which that primitive silently
    // ignores) can reach it.
    auto const body = n.error() == harv::harvest_error::git_failed ? rend::render_harvest_git_failed(worktree)
                                                                   : std::string{"bench harvest: QueryFailed"};
    return std::unexpected(error_from_body(kind_t::generic_failure, body));
  }
  ctx.out() << std::format("{}\n", *n);
  return {};
}

auto bench_finish(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return finish_common(ctx, args, "bench finish", [](std::string_view, std::string_view) { return rend::render_bench_ok(); });
}

auto bench_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const uid   = uid_of(args);
  auto const found = resolve_run(**conn, "bench show", uid);
  if (!found) {
    return std::unexpected(found.error());
  }
  auto const events = life::events(**conn, found->id);
  if (!events) {
    return std::unexpected(error_for("bench show", uid, 0, events.error()));
  }
  // Unfiltered: `bench show` reports BOTH declared and actual touches. The
  // `kind` filter exists on the engine call for `bench harvest`'s benefit.
  auto const touches = life::touches(**conn, found->id, std::nullopt);
  if (!touches) {
    return std::unexpected(error_for("bench show", uid, 0, touches.error()));
  }
  ctx.out() << (wants_json(args) ? rend::render_bench_show_json(*found, *events, *touches)
                                 : rend::render_bench_show_text(*found, *events, *touches));
  return {};
}

auto run_start(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  // The arm is `--workflow`'s value or the literal `"op"`. An operational
  // run carries no corpus identity, so `base_sha` and `config_hash` are
  // EMPTY STRINGS rather than NULL — the columns are `not null` and the
  // distinction is visible in the rows even though `run show` never prints
  // them.
  auto const arm = cliapp::flag_string(args, "--workflow").value_or("op");
  auto const uid = life::generate_run_uid(**conn);
  if (!uid) {
    return std::unexpected(error_for("run start", "", 0, uid.error()));
  }
  auto const plan_id = cliapp::flag_int(args, "--plan").value_or(0);
  auto const started = life::start(**conn, life::start_args{
                                               .run_uid     = *uid,
                                               .plan_id     = plan_id,
                                               .arm         = arm,
                                               .base_sha    = "",
                                               .config_hash = "",
                                               .config_json = std::nullopt,
                                               .corpus_repo = std::nullopt,
                                               .status      = std::nullopt,
                                               .task_filter = std::nullopt,
                                           });
  if (!started) {
    return std::unexpected(error_for("run start", *uid, 0, started.error()));
  }
  // Unconditionally JSON — `--json` is declared on the leaf and changes
  // nothing. Oracle-captured both ways.
  ctx.out() << rend::render_run_start_json(started->run_uid, plan_id, arm);
  return {};
}

auto run_event(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto payload = checked_json_flag(args, "run event", "--payload");
  if (!payload) {
    return std::unexpected(payload.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const uid   = uid_of(args);
  auto const found = resolve_run(**conn, "run event", uid);
  if (!found) {
    return std::unexpected(found.error());
  }
  // AUTO-INCREMENTED, unlike `bench event`: `coalesce(max(seq),0)+1`.
  auto const seq = life::next_seq(**conn, found->id);
  if (!seq) {
    return std::unexpected(error_for("run event", uid, 0, seq.error()));
  }
  auto const kind = cliapp::flag_string(args, "--kind").value_or("");
  auto const appended =
      life::event(**conn, found->id, *seq, kind, *payload ? std::optional<std::string_view>{**payload} : std::nullopt);
  if (!appended) {
    return std::unexpected(error_for("run event", uid, *seq, appended.error()));
  }
  ctx.out() << rend::render_run_event_json(uid, *seq, kind);
  return {};
}

auto run_finish(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return finish_common(ctx, args, "run finish",
                       [](std::string_view uid, std::string_view status) { return rend::render_run_finish_json(uid, status); });
}

auto run_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const uid   = uid_of(args);
  auto const found = resolve_run(**conn, "run show", uid);
  if (!found) {
    return std::unexpected(found.error());
  }
  auto const events = life::events(**conn, found->id);
  if (!events) {
    return std::unexpected(error_for("run show", uid, 0, events.error()));
  }
  // NO touches read at all — they are measurement-only and the operational
  // renderer omits them. Reading them here would be dead work whose absence
  // from the output looks like a rendering bug.
  ctx.out() << (wants_json(args) ? rend::render_run_show_json(*found, *events) : rend::render_run_show_text(*found, *events));
  return {};
}

} // namespace planar::cmd::handlers
