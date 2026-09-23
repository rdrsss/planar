/// @file src/cmd/planar/handlers/handoff/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.handoff`.

module planar.cmd.planar.handlers.handoff;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.planning.transitions;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.handoff;
import planar.engine.runtime.resumecheck;
import planar.engine.runtime.session;
import planar.engine.runtime.snapshot;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.capture;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.handoff.create;
import planar.cmd.planar.handlers.handoff.validate;
import planar.cmd.planar.handlers.handoff.consume;
import planar.cmd.planar.handlers.handoff.abandon;
import planar.cmd.planar.handlers.handoff.list;
import planar.cmd.planar.handlers.handoff.show;

namespace planar::cmd::handlers {

namespace ho    = engine::runtime::handoff;
namespace agent = engine::runtime::agentactivity;
namespace rck   = engine::runtime::resumecheck;
namespace sess  = engine::runtime::session;
namespace snap  = engine::runtime::snapshot;

namespace {

using kind_t = domain_error_kind;

/// @brief `--json` was passed.
auto wants_json(const cliapp::parsed_args& args) -> bool {
  return flag_bool(args, "--json");
}

/// @brief The injected status-matrix guard `handoff`'s engine functions
/// take. Bound to `transition_kind::handoff`, force always false — every
/// handoff call site in the oracle passes false.
///
/// This binding is the whole reason the engine takes a callable: the
/// matrix lives in `engine_planning` and the handoff store lives in
/// `engine_runtime`, two layer-2 buckets that may not reach each other.
/// Layer 3 may reach both, so the join happens here. See
/// `planar.engine.runtime.handoff`'s header.
auto matrix_guard() -> ho::transition_check {
  return [](ho::status from, ho::status to) {
    return engine::planning::check_transition(engine::planning::transition_kind::handoff, ho::to_text(from), ho::to_text(to),
                                              false)
        .has_value();
  };
}

/// @brief A `std::optional<std::string>` as an optional `string_view`. The
/// referent must outlive the view.
auto as_view(const std::optional<std::string>& value) -> std::optional<std::string_view> {
  if (!value) {
    return std::nullopt;
  }
  return std::string_view{*value};
}

/// @brief Parse an `<id>` positional with the oracle's own message and
/// exit code. Declared as a string in the tree precisely so this runs.
auto parse_id(const cliapp::parsed_args& args, std::string_view name, std::string_view noun)
    -> std::expected<std::int64_t, domain_error> {
  auto const raw = positional_string(args, name).value_or("");
  auto const id  = cliapp::parse_int64_zig(raw);
  if (!id) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("{} must be an integer, got '{}'", noun, raw)));
  }
  return *id;
}

/// @brief Write one handoff in the requested form.
///
/// `render_json` is a FRAGMENT (no terminator) because `render_list_json`
/// composes many into NDJSON, so this caller appends the newline. The
/// text renderer is COMPLETE and gets written verbatim. Reading each
/// renderer's own `@return` is the rule.
auto emit_one(context& ctx, const cliapp::parsed_args& args, const ho::handoff& value) -> handler_result {
  if (wants_json(args)) {
    ctx.out() << ho::render_json(value) << '\n';
  } else {
    ctx.out() << ho::render_text(value);
  }
  return {};
}

/// @brief Map a `handoff_error` from a transition verb onto the oracle's
/// message and exit code. `terminal_wording` differs per verb: `validate`
/// says "cannot transition to validated", `consume` and `abandon` say
/// "is terminal; cannot <verb>".
auto transition_failure(ho::handoff_error err, std::int64_t id, std::string_view verb, std::string_view terminal_wording)
    -> domain_error {
  switch (err) {
  case ho::handoff_error::not_found:
    return error_from_body(kind_t::not_found, std::format("handoff {} not found", id));
  case ho::handoff_error::illegal_transition:
    return error_from_body(kind_t::generic_failure, std::format("handoff {} {}", id, terminal_wording));
  default:
    return error_from_body(kind_t::generic_failure, std::format("handoff {}: QueryFailed", verb));
  }
}

/// @brief Worktree context copied off the active claim on a task.
struct worktree_context {
  std::optional<std::string> worktree_path;
  std::optional<std::string> repo_root;
  std::optional<std::string> branch;
};

/// @brief Read the ACTIVE claim on `task_id` and copy its worktree fields.
///
/// Best-effort by contract: any lookup failure, and the absence of an
/// active claim, both yield an empty context rather than an error.
/// Handoff creation must not fail because the coordination store
/// hiccupped — that is the Zig original's `catch return .{}`.
///
/// This uses `agentactivity::list_claims_by_entity`, which IS ported
/// (commit bedc18b) despite `engine/runtime/CMakeLists.txt` having listed
/// it as deferred until this task corrected the note.
auto resolve_worktree_for_task(db::connection& conn, std::int64_t task_id) -> worktree_context {
  auto const rows = agent::list_claims_by_entity(conn, "task", task_id);
  if (!rows) {
    return {};
  }
  for (auto const& row : *rows) {
    if (row.status != agent::claim_status::active) {
      continue;
    }
    // FIRST active claim wins, matching the Zig loop's early return.
    return worktree_context{.worktree_path = row.worktree_path, .repo_root = row.repo_root, .branch = row.branch};
  }
  return {};
}

/// @brief Render the composite's `--json` payload.
///
/// `"failures":[]` for a resumable task — NOT `null`. `resume validate`
/// emits `null` for the same state; see this module's header.
auto render_composite_json(std::int64_t snapshot_id, std::int64_t handoff_id, ho::status state, bool resumable,
                           std::span<const rck::validation_failure> failures) -> std::string {
  std::string out   = std::format("{{\"ok\":true,\"snapshot_id\":{},\"handoff_id\":{},\"status\":\"{}\",\"resumable\":{},"
                                  "\"failures\":[",
                                  snapshot_id, handoff_id, ho::to_text(state), resumable ? "true" : "false");
  bool        first = true;
  for (auto const& failure : failures) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += std::format("{{\"check\":\"{}\",\"message\":", failure.check);
    json_text::append_json_string(out, failure.message);
    out += ",\"remediation\":";
    json_text::append_json_string(out, failure.remediation);
    out += "}";
  }
  out += "]}\n";
  return out;
}

} // namespace

auto handoff(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const tuple = resolve_vendor_tuple(ctx.env());

  // Step 0 — an ACTIVE session is a prerequisite, never auto-created.
  // See this module's header; plan 314 task 2296 locked this refusal in.
  auto const found = sess::active_for_vendor(**conn, tuple.vendor, as_view(tuple.vendor_session_id));
  if (!found) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "looking up active session: QueryFailed"));
  }
  if (!found->has_value()) {
    return std::unexpected(error_from_body(kind_t::invalid_input, "no active session (run `planar capture session` first)"));
  }
  auto const session = **found;

  // Task id: the positional, else the session's own bound task, else none.
  std::optional<std::int64_t> task_id;
  if (auto const raw = positional_string(args, "task-id")) {
    auto const parsed = cliapp::parse_int64_zig(*raw);
    if (!parsed) {
      return std::unexpected(error_from_body(kind_t::invalid_input, std::format("task id must be an integer, got '{}'", *raw)));
    }
    task_id = *parsed;
  } else {
    task_id = session.task_id;
  }

  // Snapshot the task's next_action. A named-but-absent task is fatal.
  std::optional<std::string> next_action;
  if (task_id.has_value()) {
    auto stmt = (*conn)->prepare("select coalesce(next_action, '') from tasks where id = ?");
    if (!stmt) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "task lookup prep: QueryFailed"));
    }
    if (auto bound = stmt->bind_int64(1, *task_id); !bound) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "task lookup bind: QueryFailed"));
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "task lookup step: QueryFailed"));
    }
    if (*stepped != db::step_result::row) {
      return std::unexpected(error_from_body(kind_t::not_found, std::format("task {} not found", *task_id)));
    }
    next_action = stmt->column_text(0);
  }

  // Step 1 — the snapshot.
  auto const note     = flag_string(args, "--note");
  auto       snapshot = snap::create(**conn, snap::create_args{
                                                 .session_id        = session.id,
                                                 .task_id           = task_id,
                                                 .vendor            = tuple.vendor,
                                                 .vendor_session_id = as_view(tuple.vendor_session_id),
                                                 .body              = as_view(note),
                                                 .next_action       = as_view(next_action),
                                             });
  if (!snapshot) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "snapshot create: QueryFailed"));
  }

  // Step 2 — the pending handoff, carrying worktree context off the claim.
  worktree_context worktree;
  if (task_id.has_value()) {
    worktree = resolve_worktree_for_task(**conn, *task_id);
  }
  auto const to_vendor = flag_string(args, "--vendor");
  auto       pending   = ho::create(**conn, ho::create_args{
                                                .from_snapshot_id = snapshot->id,
                                                .from_vendor      = tuple.vendor,
                                                .to_vendor        = as_view(to_vendor),
                                                .worktree_path    = as_view(worktree.worktree_path),
                                                .repo_root        = as_view(worktree.repo_root),
                                                .branch           = as_view(worktree.branch),
                                            });
  if (!pending) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "handoff create: QueryFailed"));
  }

  // Step 3 — validate it.
  auto validated = ho::validate(**conn, pending->id, matrix_guard());
  if (!validated) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "handoff validate: QueryFailed"));
  }

  // Step 4 — the session note. BEST-EFFORT: its failure is swallowed,
  // matching the Zig `catch {}`.
  static_cast<void>(sess::append_entry(**conn, session.id, "note",
                                       std::format("handoff captured: snapshot={} handoff={}", snapshot->id, validated->id)));

  // The resumability check is ADVISORY here — a non-resumable task still
  // produces a validated handoff and still exits 0.
  bool                                 resumable = false;
  std::vector<rck::validation_failure> failures;
  if (task_id.has_value()) {
    if (auto const checked = rck::validate(**conn, *task_id)) {
      resumable = checked->resumable;
      failures  = checked->failures;
    }
  }

  if (wants_json(args)) {
    ctx.out() << render_composite_json(snapshot->id, validated->id, validated->state, resumable, failures);
    return {};
  }

  if (task_id.has_value()) {
    ctx.out() << std::format("handoff captured for task:{}\n", *task_id);
  } else {
    ctx.out() << "handoff captured for current task\n";
  }
  ctx.out() << std::format("  snapshot: {}  vendor: {}  next_action: {}\n", snapshot->id, snapshot->vendor,
                           snapshot->next_action);
  ctx.out() << std::format("  handoff:  {}  status: {}\n", validated->id, ho::to_text(validated->state));
  if (task_id.has_value()) {
    ctx.out() << (resumable ? "  validate: PASS \xe2\x80\x94 task is resume-ready\n"
                            : "  validate: FAIL \xe2\x80\x94 task is not resume-ready\n");
  }
  return {};
}

auto handoff_create(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const snapshot_id = parse_id(args, "snapshot-id", "snapshot id");
  if (!snapshot_id) {
    return std::unexpected(snapshot_id.error());
  }

  // The snapshot supplies BOTH `from_vendor` and the task whose claim
  // carries the worktree context — this leaf takes neither from the
  // environment.
  auto const snapshot = snap::show(**conn, *snapshot_id);
  if (!snapshot) {
    if (snapshot.error() == snap::snapshot_error::not_found) {
      return std::unexpected(error_from_body(kind_t::not_found, std::format("snapshot {} not found", *snapshot_id)));
    }
    return std::unexpected(error_from_body(kind_t::generic_failure, "snapshot lookup failed: QueryFailed"));
  }

  worktree_context worktree;
  if (snapshot->task_id.has_value()) {
    worktree = resolve_worktree_for_task(**conn, *snapshot->task_id);
  }
  auto const to_vendor = flag_string(args, "--vendor");
  auto       created   = ho::create(**conn, ho::create_args{
                                                .from_snapshot_id = snapshot->id,
                                                .from_vendor      = snapshot->vendor,
                                                .to_vendor        = as_view(to_vendor),
                                                .worktree_path    = as_view(worktree.worktree_path),
                                                .repo_root        = as_view(worktree.repo_root),
                                                .branch           = as_view(worktree.branch),
                                            });
  if (!created) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "handoff create: QueryFailed"));
  }
  return emit_one(ctx, args, *created);
}

auto handoff_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = parse_id(args, "handoff-id", "handoff id");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto updated = ho::validate(**conn, *id, matrix_guard());
  if (!updated) {
    return std::unexpected(transition_failure(updated.error(), *id, "validate", "cannot transition to validated"));
  }
  return emit_one(ctx, args, *updated);
}

auto handoff_consume(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = parse_id(args, "handoff-id", "handoff id");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto updated = ho::consume(**conn, *id, flag_int(args, "--session"), matrix_guard());
  if (!updated) {
    return std::unexpected(transition_failure(updated.error(), *id, "consume", "is terminal; cannot consume"));
  }
  return emit_one(ctx, args, *updated);
}

auto handoff_abandon(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = parse_id(args, "handoff-id", "handoff id");
  if (!id) {
    return std::unexpected(id.error());
  }
  // `--reason` is read and discarded: the Zig original puts it in the
  // audit summary only, and this tree has no audit table. No column stores
  // it in the oracle either. See `handoff::abandon`'s contract.
  auto updated = ho::abandon(**conn, *id, matrix_guard());
  if (!updated) {
    return std::unexpected(transition_failure(updated.error(), *id, "abandon", "is terminal; cannot abandon"));
  }
  return emit_one(ctx, args, *updated);
}

auto handoff_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // `--status` is a COMMA-SEPARATED list, each token trimmed of spaces,
  // empty tokens skipped. An unrecognized token is exit 2.
  ho::list_filter filter;
  if (auto const raw = flag_string(args, "--status")) {
    for (auto const part : std::views::split(std::string_view{*raw}, ',')) {
      std::string_view token{part.begin(), part.end()};
      while (!token.empty() && token.front() == ' ') {
        token.remove_prefix(1);
      }
      while (!token.empty() && token.back() == ' ') {
        token.remove_suffix(1);
      }
      if (token.empty()) {
        continue;
      }
      auto const parsed = ho::status_from_text(token);
      if (!parsed) {
        return std::unexpected(error_from_body(kind_t::invalid_input, std::format("unknown handoff status '{}'", token)));
      }
      filter.statuses.push_back(*parsed);
    }
  }

  auto const items = ho::list(**conn, filter);
  if (!items) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "handoff list: QueryFailed"));
  }
  if (wants_json(args)) {
    ctx.out() << ho::render_list_json(*items);
  } else {
    ctx.out() << ho::render_list_text(*items);
  }
  return {};
}

auto handoff_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = parse_id(args, "handoff-id", "handoff id");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto const found = ho::show(**conn, *id);
  if (!found) {
    if (found.error() == ho::handoff_error::not_found) {
      return std::unexpected(error_from_body(kind_t::not_found, std::format("handoff {} not found", *id)));
    }
    return std::unexpected(error_from_body(kind_t::generic_failure, "handoff show: QueryFailed"));
  }
  return emit_one(ctx, args, *found);
}

auto declare_handoff(CLI::App& root) -> void {
  CLI::App* handoff = root.add_subcommand(
      "handoff",
      "Capture a context snapshot for the current session and atomically:\n    1. Insert a context_snapshots row.\n    2. Insert "
      "a handoffs row with status='pending'.\n    3. Validate the handoff (pending → validated, validated_at set).\n\n  "
      "Subcommands manage the handoff lifecycle: create / validate /\n  consume / abandon / list / show.");
  handoff->require_subcommand(0);
  add_string(*handoff, "--vendor");
  add_string(*handoff, "--note");
  add_json(*handoff);
  add_positional_optional(*handoff, "task-id");

  // The two parent flags every child redeclares, plus --json -- see
  // `declare_handoff`'s header in handoff.cppm for why redeclaration
  // rather than CLI11's `fallthrough()`.

  handoff_cli::attach_create(handoff);

  handoff_cli::attach_validate(handoff);

  handoff_cli::attach_consume(handoff);

  handoff_cli::attach_abandon(handoff);

  handoff_cli::attach_list(handoff);

  handoff_cli::attach_show(handoff);
}

} // namespace planar::cmd::handlers
