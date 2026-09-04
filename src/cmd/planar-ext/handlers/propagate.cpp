/// @file propagate.cpp
/// @brief Implementation of `planar.cmd.planar_ext.handlers.propagate`. See
/// propagate.cppm for scope: this is the `github-parent-issue` arm only.

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
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;
import planar.cmd.planar_ext.handler;
import planar.cmd.planar_ext.handlers.ext_adapter_factory;
import planar.cmd.planar_ext.handlers.ext_strategy;

namespace planar::cmd::ext::handlers {

namespace system_ns    = engine::external::system;
namespace link_ns      = engine::external::link;
namespace parent_issue = engine::external::parent_issue;

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

  auto post_comment(std::string_view external_id, std::string_view body) -> std::expected<void, parent_issue::gh_client_error> override {
    return to_gh_result(_handle->post_comment(external_id, body));
  }

private:
  static auto map_error(adapter::adapter_error err) -> parent_issue::gh_client_error {
    return err == adapter::adapter_error::not_found ? parent_issue::gh_client_error::not_found : parent_issue::gh_client_error::other;
  }

  static auto to_gh_result(std::expected<void, adapter::adapter_error> result) -> std::expected<void, parent_issue::gh_client_error> {
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

/// @brief Render the propagate report exactly as the oracle's `runForPlan`
/// does — see propagate.zig lines 426-478. Only the fields this reduced
/// scope can ever populate are considered: `verified`/`abandoned`/`missing`
/// never appear because this arm never sets them.
auto render(context& ctx, std::int64_t plan_id, std::string_view system_slug, const parent_issue::report& rpt, bool dry_run,
           bool as_json) -> void {
  if (as_json) {
    bool const ok = rpt.failed == 0;
    std::string out;
    out += R"({"ok":)";
    out += ok ? "true" : "false";
    out += std::format(R"(,"plan_id":{},"system":)", plan_id);
    json_text::append_json_string(out, system_slug);
    out += R"(,"strategy":)";
    json_text::append_json_string(out, rpt.strategy);
    out += std::format(R"(,"created":{},"skipped":{},"failed":{},"results":[)", rpt.created, rpt.skipped, rpt.failed);
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
    out += "]}";
    ctx.out() << out << '\n';
    return;
  }

  std::string_view const prefix = dry_run ? "(dry-run) " : "";
  ctx.out() << std::format("{}propagated plan {} to {} via strategy {} (created {}, skipped {}, failed {})\n", prefix, plan_id,
                           system_slug, rpt.strategy, rpt.created, rpt.skipped, rpt.failed);
  for (auto const& r : rpt.results) {
    if (r.operation == parent_issue::op::created) {
      ctx.out() << std::format("  {}    {}:{} {} -> {}\n", op_text(r.operation), r.entity_kind, r.entity_id, r.title,
                               r.external_id);
    } else if (r.operation == parent_issue::op::skipped) {
      ctx.out() << std::format("  skipped   {}:{} {} (already linked: {})\n", r.entity_kind, r.entity_id, r.title,
                               r.external_id);
    } else {
      ctx.out() << std::format("  FAILED    {}:{} {} ({})\n", r.entity_kind, r.entity_id, r.title, r.error_name);
    }
  }
}

} // namespace

auto ext_propagate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const plan_id_raw = positional_string(args, "plan-id").value_or(std::string{});
  auto const plan_id     = resolve_plan_id(**conn, plan_id_raw);
  if (!plan_id) {
    if (plan_id.error() == "NotFound") {
      return std::unexpected(
          error_from_body(domain_error_kind::not_found, std::format("plan '{}' not found", plan_id_raw)));
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("ext propagate: lookup plan: {}", plan_id.error())));
  }

  auto const system_slug = flag_string(args, "--system");
  auto const sys          = resolve_system(**conn, system_slug.has_value() ? std::optional<std::string_view>{*system_slug}
                                                                          : std::nullopt);
  if (!sys) {
    if (sys.error() == "NotFound") {
      return std::unexpected(error_from_body(domain_error_kind::not_found,
                                             std::format("external system '{}' not found", system_slug.value_or(""))));
    }
    if (sys.error() == "NoSystemsRegistered") {
      return std::unexpected(error_from_body(domain_error_kind::not_found,
                                             "no external systems registered; run 'planar ext register' first"));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("ext propagate: lookup system: {}", sys.error())));
  }

  auto const sync_text = flag_string(args, "--sync").value_or(std::string{"read-only"});
  auto const direction  = link_ns::sync_direction_from_text(sync_text);
  if (!direction) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input,
                        std::format("invalid --sync '{}'; accepted: read-only, write-back, two-way", sync_text)));
  }

  auto const kind_text = system_ns::system_kind_to_text(sys->kind);
  auto       selected  = select_strategy(**conn, *plan_id, kind_text);
  if (!selected) {
    if (selected.error() == strategy_select_error::unsupported_system_kind) {
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input, std::format("system kind '{}' is not supported", kind_text)));
    }
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "ext propagate: pick strategy: QueryFailed"));
  }

  if (selected->kind != "github-parent-issue") {
    if (selected->kind == "github-projects-v2") {
      // Decision 1001: the multi-repo GitHub strategy is CUT from the C++
      // rewrite, not merely unported yet. `select_strategy` still reports
      // this bucket by name (see that module's header) so this refusal can
      // name the real reason instead of pretending the feature is
      // single-repo.
      return std::unexpected(error_from_body(
          domain_error_kind::invalid_input,
          "ext propagate: this feature touches multiple repos; multi-repo propagation (projects-v2) was cut from the "
          "C++ rewrite (decision 1001) and is not available"));
    }
    return std::unexpected(error_from_body(
        domain_error_kind::invalid_input,
        std::format("ext propagate: strategy '{}' is not yet implemented in planar-ext (only github-parent-issue this "
                    "cycle)",
                    selected->kind)));
  }

  bool const dry_run = flag_bool(args, "--dry-run");

  std::unique_ptr<adapter_handle> handle;
  if (!dry_run) {
    auto built = build_adapter(*sys, default_deps(ctx.env()));
    if (!built) {
      return std::unexpected(factory_error_message(built.error(), *sys));
    }
    handle = std::move(*built);
  }

  std::unique_ptr<parent_issue::gh_client> client_owner =
      handle ? std::unique_ptr<parent_issue::gh_client>(std::make_unique<adapter_gh_client>(*handle))
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

  bool const as_json = flag_bool(args, "--json");
  render(ctx, *plan_id, sys->slug, *rpt, dry_run, as_json);

  if (rpt->failed > 0) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("{} entity/entities failed during propagation", rpt->failed)));
  }
  return {};
}

} // namespace planar::cmd::ext::handlers
