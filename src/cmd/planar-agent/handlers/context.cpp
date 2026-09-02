module planar.cmd.planar_agent.handlers.context;
import std;
import planar.cliapp.args;
import planar.db;
import planar.engine.runtime;
import planar.json_text;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
namespace planar::cmd::agent::handlers {
namespace {
auto fail(std::string body, domain_error_kind k = domain_error_kind::invalid_input) -> handler_result {
  return std::unexpected(error_from_body(k, std::move(body)));
}
auto text(const cliapp::parsed_args& a, std::string_view n) -> std::string {
  return cliapp::flag_string(a, n).value_or("");
}
auto integer(const cliapp::parsed_args& a, std::string_view n) -> std::optional<std::int64_t> {
  return cliapp::flag_int(a, n);
}
auto dbfail(db::db_error const& e) -> handler_result {
  return fail(e.message_, domain_error_kind::generic_failure);
}
auto valid_kind(std::string_view s) -> bool {
  return s == "finding" || s == "risk" || s == "artifact" || s == "followup" || s == "summary" || s == "capsule";
}
} // namespace
auto context_add(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.ensure_db();
  if (!c)
    return std::unexpected(c.error());
  auto kind = text(a, "--kind"), token = text(a, "--claim"), cf = text(a, "--compiled-from");
  if (!valid_kind(kind))
    return fail(std::format("invalid --kind '{}': must be one of finding|risk|artifact|followup|summary|capsule", kind));
  auto rec = engine::runtime::contextrecords::add_from_claim(
      **c, {.token         = token,
            .kind          = kind,
            .body          = text(a, "--body"),
            .compiled_from = cf.empty() ? std::nullopt : std::optional<std::string_view>{cf}});
  if (!rec) {
    if (rec.error().message_ == "claim not found")
      return fail("claim lookup: NotFound", domain_error_kind::not_found);
    if (rec.error().message_ == "claim has no run_id")
      return fail(
          std::format("claim '{}' has no run_id: context records are run-scoped; acquire the claim with --run <id>", token));
    return dbfail(rec.error());
  }
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"id\":{},\"record\":{{\"id\":{},\"run_id\":{},\"stage\":{},\"session_id\":{},"
                             "\"claim_id\":{},\"kind\":{},\"status\":\"active\"}}}}\n",
                             rec->id, rec->id, rec->run_id, json_text::json_string(rec->stage), rec->session_id, *rec->claim_id,
                             json_text::json_string(rec->kind));
  else
    ctx.out() << std::format("context:{} run:{} stage:{} kind:{} status:active\n", rec->id, rec->run_id, rec->stage, rec->kind);
  return {};
}
auto context_capsule(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.ensure_db();
  if (!c)
    return std::unexpected(c.error());
  auto run = integer(a, "--run");
  if (!run)
    return fail(std::format("invalid --run '{}': expected integer", text(a, "--run")));
  auto sid = integer(a, "--session");
  auto cf  = text(a, "--compiled-from");
  auto rec = engine::runtime::contextrecords::add_capsule(
      **c, {.run_id        = *run,
            .stage         = text(a, "--stage"),
            .body          = text(a, "--body"),
            .session_id    = sid,
            .compiled_from = cf.empty() ? std::nullopt : std::optional<std::string_view>{cf}});
  if (!rec) {
    if (rec.error().message_ == "workflow run not found")
      return fail(std::format("workflow_run {} not found", *run), domain_error_kind::not_found);
    return dbfail(rec.error());
  }
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"id\":{},\"record\":{{\"id\":{},\"run_id\":{},\"stage\":{},\"session_id\":{},"
                             "\"claim_id\":null,\"kind\":\"capsule\",\"status\":\"active\"}}}}\n",
                             rec->id, rec->id, rec->run_id, json_text::json_string(rec->stage), rec->session_id);
  else
    ctx.out() << std::format("capsule:{} run:{} stage:{} kind:capsule status:active\n", rec->id, rec->run_id, rec->stage);
  return {};
}
auto context_list(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.ensure_db();
  if (!c)
    return std::unexpected(c.error());
  auto run = integer(a, "--run");
  if (!run)
    return fail(std::format("invalid --run '{}': expected integer run id", text(a, "--run")));
  auto stage = text(a, "--stage"), status = text(a, "--status"), kind = text(a, "--kind");
  auto records =
      engine::runtime::contextrecords::list(**c, *run, stage.empty() ? std::nullopt : std::optional<std::string_view>{stage},
                                            status.empty() ? std::nullopt : std::optional<std::string_view>{status},
                                            kind.empty() ? std::nullopt : std::optional<std::string_view>{kind});
  if (!records)
    return dbfail(records.error());
  ctx.out() << "{\"ok\":true,\"records\":[";
  bool first = true;
  for (auto const& r : *records) {
    if (!first)
      ctx.out() << ",";
    first = false;
    ctx.out() << std::format(
        "{{\"id\":{},\"run_id\":{},\"stage\":{},\"session_id\":{},\"claim_id\":{},\"kind\":{},\"body\":{},\"status\":{},"
        "\"compiled_from\":{},\"created_at\":{}}}",
        r.id, r.run_id, json_text::json_string(r.stage), r.session_id, r.claim_id ? std::to_string(*r.claim_id) : "null",
        json_text::json_string(r.kind), json_text::json_string(r.body), json_text::json_string(r.status),
        r.compiled_from ? json_text::json_string(*r.compiled_from) : "null", json_text::json_string(r.created_at));
  }
  ctx.out() << "]}\n";
  return {};
}
auto context_resolve(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto c = ctx.ensure_db();
  if (!c)
    return std::unexpected(c.error());
  auto target = text(a, "--status");
  if (target != "consumed" && target != "superseded")
    return fail(std::format("invalid --status '{}': must be consumed|superseded", target));
  auto id = integer(a, "--id"), run = integer(a, "--run");
  if (id && run)
    return fail("provide either --id or --run, not both");
  if (!id && !run)
    return fail("provide either --id (single record) or --run (bulk stage sweep)");
  std::int64_t updated = 0;
  if (id) {
    auto rec = engine::runtime::contextrecords::get(**c, *id);
    if (!rec) {
      if (rec.error().message_ == "context record not found")
        return fail(std::format("context_records row {} not found", *id), domain_error_kind::not_found);
      return dbfail(rec.error());
    }
    if (rec->status != "active")
      return fail(std::format("record {} is already in status '{}'; can only resolve active records", *id, rec->status));
    auto result = engine::runtime::contextrecords::resolve_one(**c, *id, target);
    if (!result)
      return dbfail(result.error());
    updated = *result;
  } else {
    auto stage = text(a, "--stage");
    if (stage.empty())
      return fail("--stage is required when using --run for a bulk sweep");
    auto result = engine::runtime::contextrecords::resolve_stage(**c, *run, stage, target);
    if (!result)
      return dbfail(result.error());
    updated = *result;
  }
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"updated\":{},\"status\":{}}}\n", updated, json_text::json_string(target));
  else
    ctx.out() << std::format("updated:{} status:{}\n", updated, target);
  return {};
}
} // namespace planar::cmd::agent::handlers
