/// @file propagate.cpp
/// @brief Implementation of `planar.cmd.planar_ext.handlers.propagate`. See
/// propagate.cppm for scope: the `github-parent-issue` arm, plus the
/// `--restrategize`/`--verify-counterparts`/`--scope` surface (task 6428).

module;

module planar.cmd.planar_ext.handlers.propagate;

import std;
import cli11;
import planar.adapter;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.external;
import planar.engine.extsync.github;
import planar.engine.planning.descendants;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;
import planar.cmd.planar_ext.handler;
import planar.cmd.planar_ext.handlers.ext;
import planar.cmd.planar_ext.handlers.ext_adapter_factory;
import planar.cmd.planar_ext.handlers.ext_strategy;

namespace planar::cmd::ext::handlers {

namespace system_ns    = engine::external::system;
namespace link_ns      = engine::external::link;
namespace parent_issue = engine::external::parent_issue;
namespace descendants  = engine::planning::descendants;

namespace {

/// @brief Resolve `raw` to a plan id: an integer id, or a plan slug.
///
/// Mirrors the oracle's `resolvePlanId`. An integer that names no row and a
/// slug that names no row both report `not_found`; anything else is a query
/// failure.
/// @param conn An open, migrated connection.
/// @param raw The CLI positional value.
/// @return The resolved id, or the failure.
auto resolve_plan_id(db::connection& conn, std::string_view raw) -> std::expected<std::int64_t, std::string_view> {
  std::int64_t parsed_id = 0;
  auto const   from_int  = std::from_chars(raw.data(), raw.data() + raw.size(), parsed_id);
  if (from_int.ec == std::errc{} && from_int.ptr == raw.data() + raw.size()) {
    auto stmt = conn.prepare("select count(*) from plans where id = ?");
    if (!stmt) {
      return std::unexpected("QueryFailed");
    }
    if (!stmt->bind_int64(1, parsed_id)) {
      return std::unexpected("QueryFailed");
    }
    auto const step = stmt->step();
    if (!step) {
      return std::unexpected("QueryFailed");
    }
    if (*step != db::step_result::row || stmt->column_int64(0) == 0) {
      return std::unexpected("NotFound");
    }
    return parsed_id;
  }

  auto stmt = conn.prepare("select id from plans where slug = ? limit 1");
  if (!stmt) {
    return std::unexpected("QueryFailed");
  }
  if (!stmt->bind_text(1, raw)) {
    return std::unexpected("QueryFailed");
  }
  auto const step = stmt->step();
  if (!step) {
    return std::unexpected("QueryFailed");
  }
  if (*step != db::step_result::row) {
    return std::unexpected("NotFound");
  }
  return stmt->column_int64(0);
}

/// @brief Resolve the target system: the named slug, or the first
/// registered system when `--system` is omitted.
/// @param conn An open, migrated connection.
/// @param slug_opt The `--system` value, when given.
/// @return The resolved row, or why it could not be resolved.
auto resolve_system(db::connection& conn, std::optional<std::string_view> slug_opt)
    -> std::expected<system_ns::external_system, std::string_view> {
  if (slug_opt.has_value() && !slug_opt->empty()) {
    auto sys = system_ns::show_by_slug(conn, *slug_opt);
    if (!sys) {
      return std::unexpected("NotFound");
    }
    return *sys;
  }
  auto systems = system_ns::list(conn);
  if (!systems) {
    return std::unexpected("QueryFailed");
  }
  if (systems->empty()) {
    return std::unexpected("NoSystemsRegistered");
  }
  return systems->front();
}

/// @brief The GitHub `gh_client` bound to a production `adapter_handle`.
///
/// Same seam shape as the oracle's `ParentIssueBridge`: every callback
/// dispatches to `adapter_handle`'s GitHub-only methods and maps
/// `adapter::adapter_error` down to the two-member `gh_client_error` the
/// engine distinguishes on (`not_found` vs `other`) — see
/// `parent_issue.cppm`'s `gh_client_error` header on why that collapse is
/// the oracle's own shape, not a simplification this port introduces.
class adapter_gh_client final : public parent_issue::gh_client {
public:
  explicit adapter_gh_client(const adapter_handle& handle) : _handle(&handle) {
  }

  auto probe(std::string_view owner, std::string_view repo) -> std::expected<void, parent_issue::gh_client_error> override {
    return to_gh_result(_handle->link_sub_issue_probe(owner, repo));
  }

  auto create_issue(std::string_view owner, std::string_view repo, std::string_view title, std::string_view body,
                    std::span<const std::string> labels)
      -> std::expected<parent_issue::created_issue, parent_issue::gh_client_error> override {
    auto created = _handle->create_issue(owner, repo, title, body, labels);
    if (!created) {
      return std::unexpected(map_error(created.error()));
    }
    return parent_issue::created_issue{.number = created->number, .node_id = created->node_id};
  }

  auto link_sub_issue(std::string_view owner, std::string_view repo, std::int64_t parent_number, std::int64_t child_number)
      -> std::expected<void, parent_issue::gh_client_error> override {
    return to_gh_result(_handle->link_sub_issue(owner, repo, parent_number, child_number));
  }

  auto post_comment(std::string_view external_id, std::string_view body)
      -> std::expected<void, parent_issue::gh_client_error> override {
    return to_gh_result(_handle->post_comment(external_id, body));
  }

private:
  static auto map_error(adapter::adapter_error err) -> parent_issue::gh_client_error {
    return err == adapter::adapter_error::not_found ? parent_issue::gh_client_error::not_found
                                                    : parent_issue::gh_client_error::other;
  }

  static auto to_gh_result(std::expected<void, adapter::adapter_error> result)
      -> std::expected<void, parent_issue::gh_client_error> {
    if (!result) {
      return std::unexpected(map_error(result.error()));
    }
    return {};
  }

  const adapter_handle* _handle;
};

/// @brief The one production `gh_client` used when `--dry-run` builds no
/// adapter. Every method is unreachable: the engine never calls a
/// `gh_client` method under `opts.dry_run` (see `parent_issue.cpp`'s dry-run
/// branch in `create_or_skip_github_issue`), so this exists only to satisfy
/// the interface when there is no handle to bind to.
class unreachable_gh_client final : public parent_issue::gh_client {
public:
  auto probe(std::string_view, std::string_view) -> std::expected<void, parent_issue::gh_client_error> override {
    return std::unexpected(parent_issue::gh_client_error::other);
  }
  auto create_issue(std::string_view, std::string_view, std::string_view, std::string_view, std::span<const std::string>)
      -> std::expected<parent_issue::created_issue, parent_issue::gh_client_error> override {
    return std::unexpected(parent_issue::gh_client_error::other);
  }
  auto link_sub_issue(std::string_view, std::string_view, std::int64_t, std::int64_t)
      -> std::expected<void, parent_issue::gh_client_error> override {
    return std::unexpected(parent_issue::gh_client_error::other);
  }
  auto post_comment(std::string_view, std::string_view) -> std::expected<void, parent_issue::gh_client_error> override {
    return std::unexpected(parent_issue::gh_client_error::other);
  }
};

/// @brief The refusal for a `parent_issue_error`, verbatim from the oracle's
/// `exit.die` call sites in `runParentIssueStrategy`.
auto parent_issue_error_message(parent_issue::parent_issue_error err) -> domain_error {
  switch (err) {
  case parent_issue::parent_issue_error::no_touched_repos:
    return error_from_body(
        domain_error_kind::invalid_input,
        "parent-issue strategy needs a touched repo (add a `tasks.scope_kind='repo'` row or a `task touches add` link)");
  case parent_issue::parent_issue_error::cannot_resolve_repo:
    return error_from_body(domain_error_kind::invalid_input,
                           "parent-issue strategy could not resolve a GitHub owner/repo from the touched project (set "
                           "`projects.git_remote` or use an `owner/repo` slug)");
  case parent_issue::parent_issue_error::sub_issue_unsupported:
    return error_from_body(domain_error_kind::invalid_input,
                           "parent-issue strategy: GitHub account does not expose the sub-issue REST endpoint for this "
                           "repo; use `--github-strategy tracking-issue` instead");
  case parent_issue::parent_issue_error::bad_config:
    return error_from_body(domain_error_kind::invalid_input, "parent-issue strategy: bad configuration");
  case parent_issue::parent_issue_error::query_failed:
  default:
    return error_from_body(domain_error_kind::generic_failure, "ext propagate: parent-issue: QueryFailed");
  }
}

auto op_text(parent_issue::op operation) -> std::string_view {
  switch (operation) {
  case parent_issue::op::created:
    return "created";
  case parent_issue::op::skipped:
    return "skipped";
  case parent_issue::op::failed:
  default:
    return "failed";
  }
}

/// @brief One `--verify-counterparts` outcome row, rendered alongside the
/// propagation results. Mirrors the oracle's `verified`/`missing`
/// `LineResult` rows (propagate.zig lines 384-412) — `title` is always
/// empty, matching the oracle, which never re-reads the local title for a
/// verify pass.
struct verify_row {
  std::string  entity_kind;
  std::int64_t entity_id = 0;
  std::string  external_id;
  bool         missing = false; ///< false == "verified" (still present).
};

/// @brief Prompt-and-read a single-line y/N confirmation for
/// `--restrategize` without `--yes`, verbatim from the oracle's
/// `confirmRestrategize` (propagate.zig lines 490-514).
///
/// Reads directly from `std::cin` rather than through `context` — no other
/// `planar-ext` handler needs an interactive prompt, so no seam exists yet
/// to inject one. That also means this path is untestable in-process; the
/// leaf tests drive it only via `--yes`, which never reaches this function.
/// @param ctx The invocation context, for the prompt text.
/// @param old_strategy The cached strategy kind.
/// @param new_strategy The freshly-selected strategy kind.
/// @return Whether the operator confirmed.
auto confirm_restrategize(context& ctx, std::string_view old_strategy, std::string_view new_strategy) -> bool {
  ctx.out() << std::format("Restrategize: cached strategy is \"{}\", new strategy would be \"{}\".\n", old_strategy,
                           new_strategy);
  ctx.out() << "Abandoning old counterparts will not delete them on the remote; Planar will stop tracking them.\n";
  ctx.out() << "Confirm? [y/N] ";
  ctx.out().flush();

  std::string line;
  if (!std::getline(std::cin, line)) {
    return false;
  }
  auto const first = line.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return false;
  }
  auto const  last = line.find_last_not_of(" \t\r\n");
  std::string trimmed{line.substr(first, last - first + 1)};
  for (auto& c : trimmed) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return trimmed == "y" || trimmed == "yes";
}

/// @brief What probing one counterpart against the remote found. Mirrors
/// the oracle's `ProbeResult` union (propagate.zig lines 518-522).
enum class probe_outcome : std::uint8_t {
  present,     ///< The remote item still exists.
  missing,     ///< The remote returned 404 (`adapter_error::not_found`).
  probe_error, ///< Any other failure — non-fatal, the caller warns and skips.
};

/// @brief What `probe_counterpart` returns: the outcome, plus the
/// underlying error when it was inconclusive (`outcome == probe_error`).
struct probe_result {
  probe_outcome                         outcome = probe_outcome::present;
  std::optional<adapter::adapter_error> error;
};

/// @brief Dispatch a single GET against the remote for `external_id`
/// through the generic four-operation adapter interface (works for both
/// Jira and GitHub — `adapter_handle::instance()` erases the concrete
/// type). Mirrors the oracle's `probeCounterpart`.
/// @param handle The built adapter handle.
/// @param external_id The provider-side id to probe.
/// @return The outcome; `error` is set only when `outcome == probe_error`.
auto probe_counterpart(const adapter_handle& handle, std::string_view external_id) -> probe_result {
  auto const result = handle.instance().pull(external_id);
  if (result) {
    return {.outcome = probe_outcome::present};
  }
  if (result.error() == adapter::adapter_error::not_found) {
    return {.outcome = probe_outcome::missing};
  }
  return {.outcome = probe_outcome::probe_error, .error = result.error()};
}

/// @brief Render the propagate report, matching the oracle's `runForPlan`
/// output shape — see propagate.zig lines 426-478 (results) and 375-420
/// (the verify-counterparts additions, task 6428). `abandoned_count` and
/// the `verify_rows` are only ever non-empty when `--restrategize` /
/// `--verify-counterparts` were passed and the run reached that far.
auto render(context& ctx, std::int64_t plan_id, std::string_view system_slug, const parent_issue::report& rpt, bool dry_run,
            bool as_json, std::size_t abandoned_count, std::span<const verify_row> verify_rows, bool unlink_missing,
            bool recreate_missing) -> void {
  std::size_t verified_count = 0;
  std::size_t missing_count  = 0;
  for (auto const& v : verify_rows) {
    if (v.missing) {
      ++missing_count;
    } else {
      ++verified_count;
    }
  }
  bool const ok = rpt.failed == 0 && missing_count == 0;

  if (as_json) {
    std::string out;
    out += R"({"ok":)";
    out += ok ? "true" : "false";
    out += std::format(R"(,"plan_id":{},"system":)", plan_id);
    json_text::append_json_string(out, system_slug);
    out += R"(,"strategy":)";
    json_text::append_json_string(out, rpt.strategy);
    out += std::format(R"(,"created":{},"skipped":{},"failed":{})", rpt.created, rpt.skipped, rpt.failed);
    if (verified_count > 0) {
      out += std::format(R"(,"verified":{})", verified_count);
    }
    if (abandoned_count > 0) {
      out += std::format(R"(,"abandoned":{})", abandoned_count);
    }
    if (missing_count > 0) {
      out += std::format(R"(,"missing":{})", missing_count);
    }
    out += R"(,"results":[)";
    for (std::size_t i = 0; i < rpt.results.size(); ++i) {
      auto const& r = rpt.results[i];
      if (i != 0) {
        out += ",";
      }
      out += R"({"entity_kind":)";
      json_text::append_json_string(out, r.entity_kind);
      out += std::format(R"(,"entity_id":{},"title":)", r.entity_id);
      json_text::append_json_string(out, r.title);
      out += R"(,"op":)";
      json_text::append_json_string(out, op_text(r.operation));
      out += R"(,"external_id":)";
      json_text::append_json_string(out, r.external_id);
      if (!r.error_name.empty()) {
        out += R"(,"error":)";
        json_text::append_json_string(out, r.error_name);
      }
      out += "}";
    }
    for (auto const& v : verify_rows) {
      out += ",{\"entity_kind\":";
      json_text::append_json_string(out, v.entity_kind);
      out += std::format(R"(,"entity_id":{},"title":"",)", v.entity_id);
      out += R"("op":)";
      json_text::append_json_string(out, v.missing ? "missing" : "verified");
      out += R"(,"external_id":)";
      json_text::append_json_string(out, v.external_id);
      out += "}";
    }
    out += "]}";
    ctx.out() << out << '\n';
    return;
  }

  std::string_view const prefix = dry_run ? "(dry-run) " : "";
  ctx.out() << std::format("{}propagated plan {} to {} via strategy {} (created {}, skipped {}, failed {}", prefix, plan_id,
                           system_slug, rpt.strategy, rpt.created, rpt.skipped, rpt.failed);
  if (verified_count > 0) {
    ctx.out() << std::format(", verified {}", verified_count);
  }
  if (abandoned_count > 0) {
    ctx.out() << std::format(", abandoned {}", abandoned_count);
  }
  if (missing_count > 0) {
    ctx.out() << std::format(", missing {}", missing_count);
  }
  ctx.out() << ")\n";
  for (auto const& r : rpt.results) {
    if (r.operation == parent_issue::op::created) {
      ctx.out() << std::format("  {}    {}:{} {} -> {}\n", op_text(r.operation), r.entity_kind, r.entity_id, r.title,
                               r.external_id);
    } else if (r.operation == parent_issue::op::skipped) {
      ctx.out() << std::format("  skipped   {}:{} {} (already linked: {})\n", r.entity_kind, r.entity_id, r.title, r.external_id);
    } else {
      ctx.out() << std::format("  FAILED    {}:{} {} ({})\n", r.entity_kind, r.entity_id, r.title, r.error_name);
    }
  }
  for (auto const& v : verify_rows) {
    if (v.missing) {
      ctx.out() << std::format("  MISSING   {}:{}  (counterpart {} not found on remote)\n", v.entity_kind, v.entity_id,
                               v.external_id);
    } else {
      ctx.out() << std::format("  verified  {}:{}  ({} still present)\n", v.entity_kind, v.entity_id, v.external_id);
    }
  }
  if (missing_count > 0 && !unlink_missing && !recreate_missing) {
    ctx.out() << std::format("  {} counterpart(s) missing; use --unlink or --recreate to remediate\n", missing_count);
  }
}

} // namespace

auto ext_propagate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // `--scope` is declared on the CLI tree and read here, then discarded —
  // verbatim `_ = args.scope;` from the oracle. `external_links` carries no
  // scope column and `ext propagate` is UNGUARDED BY DESIGN (see this
  // module's header and docs/concepts.md § cross-scope-guard).
  (void)flag_string(args, "--scope");

  bool const restrategize        = flag_bool(args, "--restrategize");
  bool const auto_yes            = flag_bool(args, "--yes");
  bool const verify_counterparts = flag_bool(args, "--verify-counterparts");
  bool const unlink_missing      = flag_bool(args, "--unlink");
  bool const recreate_missing    = flag_bool(args, "--recreate");

  // Up-front flag validation, mirroring the oracle's preflight in `handle`
  // (propagate.zig lines 67-72).
  if (unlink_missing && recreate_missing) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--unlink and --recreate are mutually exclusive"));
  }
  if ((unlink_missing || recreate_missing) && !verify_counterparts) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, "--unlink and --recreate require --verify-counterparts"));
  }

  auto const plan_id_raw = positional_string(args, "plan-id").value_or(std::string{});
  auto const plan_id     = resolve_plan_id(**conn, plan_id_raw);
  if (!plan_id) {
    if (plan_id.error() == "NotFound") {
      return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("plan '{}' not found", plan_id_raw)));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("ext propagate: lookup plan: {}", plan_id.error())));
  }

  auto const system_slug = flag_string(args, "--system");
  auto const sys = resolve_system(**conn, system_slug.has_value() ? std::optional<std::string_view>{*system_slug} : std::nullopt);
  if (!sys) {
    if (sys.error() == "NotFound") {
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, std::format("external system '{}' not found", system_slug.value_or(""))));
    }
    if (sys.error() == "NoSystemsRegistered") {
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, "no external systems registered; run 'planar ext register' first"));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("ext propagate: lookup system: {}", sys.error())));
  }

  auto const sync_text = flag_string(args, "--sync").value_or(std::string{"read-only"});
  auto const direction = link_ns::sync_direction_from_text(sync_text);
  if (!direction) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input,
                        std::format("invalid --sync '{}'; accepted: read-only, write-back, two-way", sync_text)));
  }

  auto const kind_text = system_ns::system_kind_to_text(sys->kind);

  // `--github-strategy` (task 6451): an explicit override that BYPASSES
  // `select_strategy`'s repo-count query entirely, exactly like the
  // oracle's `handle` (propagate.zig lines 76-83, 132-141) — an override
  // is a deliberate operator choice, not something the auto-detected
  // bucket should second-guess. Declared as mutually exclusive with
  // `--restrategize`, matching the oracle's up-front preflight.
  auto const github_strategy_flag = flag_string(args, "--github-strategy");
  if (github_strategy_flag.has_value() && restrategize) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, "--github-strategy and --restrategize are mutually exclusive"));
  }

  std::string selected_kind;
  if (github_strategy_flag.has_value()) {
    if (sys->kind != system_ns::system_kind::github_issues) {
      return std::unexpected(error_from_body(
          domain_error_kind::invalid_input,
          std::format("--github-strategy is only valid for github-issues systems; system '{}' has kind '{}'", sys->slug,
                      kind_text)));
    }
    if (*github_strategy_flag == "parent-issue") {
      selected_kind = "github-parent-issue";
    } else if (*github_strategy_flag == "projects-v2") {
      selected_kind = "github-projects-v2";
    } else if (*github_strategy_flag == "tracking-issue") {
      selected_kind = "github-tracking-issue";
    } else {
      return std::unexpected(error_from_body(
          domain_error_kind::invalid_input,
          std::format("invalid --github-strategy '{}'; accepted: parent-issue, projects-v2, tracking-issue",
                      *github_strategy_flag)));
    }
  } else {
    auto selected = select_strategy(**conn, *plan_id, kind_text);
    if (!selected) {
      if (selected.error() == strategy_select_error::unsupported_system_kind) {
        return std::unexpected(
            error_from_body(domain_error_kind::invalid_input, std::format("system kind '{}' is not supported", kind_text)));
      }
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "ext propagate: pick strategy: QueryFailed"));
    }
    selected_kind = std::string(selected->kind);
  }

  if (selected_kind == "github-projects-v2") {
    // Decision 1001 (accepted): the multi-repo GitHub strategy is CUT from
    // the C++ rewrite, not merely unported yet. `select_strategy` still
    // reports this bucket by name for an auto-detected >=2-repo feature
    // (see that module's header), and `--github-strategy projects-v2` is
    // still an ACCEPTED flag value (matching the oracle's accepted set) —
    // both paths land here, and this refusal names the real reason instead
    // of pretending the feature is single-repo or the flag is unknown.
    return std::unexpected(error_from_body(
        domain_error_kind::invalid_input,
        "ext propagate: this feature touches multiple repos; multi-repo propagation (projects-v2) was cut from the "
        "C++ rewrite (decision 1001) and is not available"));
  }

  bool const dry_run = flag_bool(args, "--dry-run");

  // --- restrategize -----------------------------------------------------
  //
  // Mirrors propagate.zig lines 158-197's `restrategize` arm. Generalized
  // at task 6451 to key off `selected_kind` (auto-detected OR
  // `--github-strategy`-overridden) rather than assuming
  // `github-parent-issue` — `link_ns::read_cached_strategy` /
  // `abandon_counterparts` are keyed by string, not by which strategy
  // produced it, so nothing here is parent-issue-specific.
  std::size_t abandoned_count = 0;
  if (restrategize) {
    auto cached = link_ns::read_cached_strategy(**conn, *plan_id, sys->id);
    if (!cached) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, "ext propagate: read cached strategy: QueryFailed"));
    }
    if (cached->has_value() && **cached != selected_kind) {
      if (!auto_yes) {
        if (!confirm_restrategize(ctx, **cached, selected_kind)) {
          return std::unexpected(error_from_body(domain_error_kind::invalid_input, "restrategize cancelled by user"));
        }
      }
      auto abandoned = link_ns::abandon_counterparts(**conn, *plan_id, sys->id, **cached, selected_kind);
      if (!abandoned) {
        return std::unexpected(
            error_from_body(domain_error_kind::generic_failure, "ext propagate: abandon counterparts: QueryFailed"));
      }
      abandoned_count = *abandoned;
    }
    // cached == selected_kind (or no cache yet): no-op restrategize.
  }

  if (selected_kind == "github-parent-issue") {
    // `need_adapter`: skipped under `--dry-run` unless `--verify-counterparts`
    // is also set — verify needs the adapter to probe. Mirrors propagate.zig
    // line 232.
    bool const need_adapter = !dry_run || verify_counterparts;

    std::unique_ptr<adapter_handle> handle;
    if (need_adapter) {
      auto built = build_adapter(*sys, default_deps(ctx.env()));
      if (!built) {
        return std::unexpected(factory_error_message(built.error(), *sys));
      }
      handle = std::move(*built);
    }

    std::unique_ptr<parent_issue::gh_client> client_owner =
        (handle && !dry_run) ? std::unique_ptr<parent_issue::gh_client>(std::make_unique<adapter_gh_client>(*handle))
                             : std::unique_ptr<parent_issue::gh_client>(std::make_unique<unreachable_gh_client>());

    auto rpt = parent_issue::propagate_parent_issue(**conn, *client_owner, *plan_id,
                                                    parent_issue::opts{
                                                        .sys_id          = sys->id,
                                                        .sys_slug        = sys->slug,
                                                        .dry_run         = dry_run,
                                                        .sync_direction_ = *direction,
                                                    });
    if (!rpt) {
      return std::unexpected(parent_issue_error_message(rpt.error()));
    }

    // --- verify-counterparts pass -------------------------------------------
    //
    // Mirrors propagate.zig lines 375-420. Only runs when NOT dry-run — a
    // preview should not mutate `external_links` or `sync_events`.
    std::vector<verify_row> verify_rows;
    if (verify_counterparts && !dry_run) {
      auto links = link_ns::list_mirror_links_in_tree(**conn, *plan_id, sys->id);
      if (!links) {
        return std::unexpected(
            error_from_body(domain_error_kind::generic_failure, "ext propagate: list links for verify: QueryFailed"));
      }
      for (auto const& row : *links) {
        auto const probed = probe_counterpart(*handle, row.external_id);
        if (probed.outcome == probe_outcome::present) {
          verify_rows.push_back(verify_row{
              .entity_kind = row.entity_kind, .entity_id = row.entity_id, .external_id = row.external_id, .missing = false});
        } else if (probed.outcome == probe_outcome::missing) {
          auto recorded = link_ns::record_counterpart_missing(**conn, row.id, row.entity_kind, row.entity_id, row.external_id,
                                                              unlink_missing || recreate_missing);
          if (!recorded) {
            return std::unexpected(
                error_from_body(domain_error_kind::generic_failure, "ext propagate: record counterpart-missing: QueryFailed"));
          }
          verify_rows.push_back(verify_row{
              .entity_kind = row.entity_kind, .entity_id = row.entity_id, .external_id = row.external_id, .missing = true});
        } else {
          // Non-fatal: warn and skip, matching the oracle's probe_error arm.
          ctx.err() << std::format("warning: counterpart probe failed for {} ({}); skipping\n", row.external_id,
                                   adapter::adapter_error_name(*probed.error));
        }
      }
    }

    std::size_t missing_count = 0;
    for (auto const& v : verify_rows) {
      if (v.missing) {
        ++missing_count;
      }
    }

    bool const as_json = flag_bool(args, "--json");
    render(ctx, *plan_id, sys->slug, *rpt, dry_run, as_json, abandoned_count, verify_rows, unlink_missing, recreate_missing);

    // Missing-counterpart refusal takes priority, matching the oracle's
    // ordering (propagate.zig lines 480-485: missing check precedes failed).
    if (missing_count > 0 && !unlink_missing && !recreate_missing) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                             std::format("{} counterpart(s) missing during --verify-counterparts", missing_count)));
    }
    if (rpt->failed > 0) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                             std::format("{} entity/entities failed during propagation", rpt->failed)));
    }
    return {};
  }

  // --- generic per-entity tree walk ---------------------------------------
  //
  // `jira-epic`, `github-zero-repo`, `github-tracking-issue`: every reachable
  // strategy that is NOT `github-parent-issue` (and not the cut
  // `github-projects-v2`, refused above). Ported at task 6451 as the
  // trivial loop the oracle itself uses (propagate.zig's `else for (tree)
  // |entry|` arm): walk the feature tree, then call the SAME
  // `propagate_one_entity` core `ext propagate-one` calls for each entry —
  // this is what makes the two verbs genuinely equivalent rather than
  // similar (see `propagate_one_entity`'s header in ext.cppm and
  // `propagate_faithful_test.zig`, the test this closes).
  //
  // `--verify-counterparts` is intentionally NOT ported onto this arm this
  // cycle — refusing explicitly here is honest about the gap; silently
  // ignoring the flag would not be.
  if (verify_counterparts) {
    return std::unexpected(error_from_body(
        domain_error_kind::invalid_input,
        "ext propagate: --verify-counterparts is only supported for the github-parent-issue strategy this cycle"));
  }

  std::unique_ptr<adapter_handle> handle;
  if (!dry_run) {
    auto built = build_adapter(*sys, default_deps(ctx.env()));
    if (!built) {
      return std::unexpected(factory_error_message(built.error(), *sys));
    }
    handle = std::move(*built);
  }

  auto root = templates_root_for(ctx);
  if (!root) {
    return std::unexpected(root.error());
  }

  auto tree = descendants::walk_tree(**conn, *plan_id);
  if (!tree) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "ext propagate: walk tree: QueryFailed"));
  }

  parent_issue::report rpt;
  rpt.strategy = selected_kind;
  for (auto const& entry : *tree) {
    std::string_view const entity_kind_str = entry.kind == descendants::entry_kind::task ? "task" : "plan";
    auto const              role           = entry.kind == descendants::entry_kind::plan_anchor ? entity_role::plan_anchor
                                            : entry.kind == descendants::entry_kind::task        ? entity_role::task
                                                                                                 : entity_role::plan_child;
    auto const template_kind = template_kind_for_entity(kind_text, role);
    if (!template_kind) {
      rpt.results.push_back(parent_issue::entity_result{.entity_kind = std::string(entity_kind_str),
                                                        .entity_id   = entry.id,
                                                        .title       = entry.title,
                                                        .operation   = parent_issue::op::failed,
                                                        .error_name  = "UnsupportedSystemKind"});
      ++rpt.failed;
      continue;
    }
    auto outcome = propagate_one_entity(**conn, handle.get(), *sys, entity_kind_str, entry.id, role, selected_kind,
                                        *template_kind, *direction, dry_run, *root);
    if (!outcome) {
      rpt.results.push_back(parent_issue::entity_result{.entity_kind = std::string(entity_kind_str),
                                                        .entity_id   = entry.id,
                                                        .title       = entry.title,
                                                        .operation   = parent_issue::op::failed,
                                                        .error_name  = outcome.error().text});
      ++rpt.failed;
      continue;
    }
    parent_issue::op const op_val = outcome->op == "skipped"  ? parent_issue::op::skipped
                                   : outcome->op == "planned" ? parent_issue::op::planned
                                                              : parent_issue::op::created;
    rpt.results.push_back(parent_issue::entity_result{.entity_kind = std::string(entity_kind_str),
                                                      .entity_id   = entry.id,
                                                      .title       = entry.title,
                                                      .operation   = op_val,
                                                      .external_id = outcome->external_id});
    // A `planned` (dry-run) row counts as `created`, matching the oracle's
    // own `created_count` accounting (propagate.zig lines 352-360).
    if (op_val == parent_issue::op::skipped) {
      ++rpt.skipped;
    } else {
      ++rpt.created;
    }
  }

  bool const as_json = flag_bool(args, "--json");
  render(ctx, *plan_id, sys->slug, rpt, dry_run, as_json, abandoned_count, {}, unlink_missing, recreate_missing);

  if (rpt.failed > 0) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("{} entity/entities failed during propagation", rpt.failed)));
  }
  return {};
}

} // namespace planar::cmd::ext::handlers
