/// @file models.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.models`.

module planar.cmd.planar.handlers.models;

import std;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.ingest.packet;
import planar.engine.models.legacy;
import planar.engine.models.profile;
import planar.engine.models.ranking;
import planar.engine.models.registry;
import planar.engine.models.render;
import planar.engine.models.roles;
import planar.engine.models.views;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace reg    = engine::models::registry;
namespace rank_  = engine::models::ranking;
namespace views_ = engine::models::views;
namespace rend   = engine::models::render;
namespace legacy = engine::models::legacy;
namespace prof   = engine::models::profile;
namespace mroles = engine::models::roles;
namespace pk     = engine::ingest::packet;

namespace {

using kind_t = domain_error_kind;

/// @brief `--json` was passed.
/// @param args The parsed arguments.
/// @return Whether the flag is set.
auto wants_json(const cliapp::parsed_args& args) -> bool {
  return flag_bool(args, "--json");
}

/// @brief The zig error NAME the oracle interpolates for a `registry_error`.
/// @param err The engine failure.
/// @return The CamelCase name.
auto error_name(reg::registry_error err) -> std::string_view {
  switch (err) {
  case reg::registry_error::not_found:
    return "NotFound";
  case reg::registry_error::conflict:
    return "Conflict";
  case reg::registry_error::invalid_value:
    return "InvalidValue";
  case reg::registry_error::query_failed:
    break;
  }
  return "QueryFailed";
}

/// @brief The exit-code bucket for a `registry_error`.
///
/// `conflict` lands in `sync_conflict` (exit 3) and `invalid_value` in
/// `invalid_input` (exit 2) — both oracle-captured. Note this differs from
/// the `bench`/`run` family, where a duplicate is exit 6: the two families
/// classify a UNIQUE violation into DIFFERENT buckets and neither is
/// normalized to match the other.
/// @param err The engine failure.
/// @return The bucket.
auto error_kind(reg::registry_error err) -> domain_error_kind {
  switch (err) {
  case reg::registry_error::not_found:
    return kind_t::not_found;
  case reg::registry_error::conflict:
    return kind_t::sync_conflict;
  case reg::registry_error::invalid_value:
    return kind_t::invalid_input;
  case reg::registry_error::query_failed:
    break;
  }
  return kind_t::generic_failure;
}

/// @brief Compose the family's `<gerund phrase>: <ErrorName>` refusal.
/// @param what The gerund phrase, e.g. `"registering opaque candidate"`.
/// @param err The engine failure.
/// @return The domain error.
auto registry_failure(std::string_view what, reg::registry_error err) -> domain_error {
  return error_from_body(error_kind(err), std::format("{}: {}", what, error_name(err)));
}

/// @brief Read `--candidate`. Declared `(int) required` on the tree, so
/// CLI11 has already enforced both.
/// @param args The parsed arguments.
/// @return The candidate id.
auto candidate_of(const cliapp::parsed_args& args) -> std::int64_t {
  return cliapp::flag_int(args, "--candidate").value_or(0);
}

/// @brief Read and validate `--tier`, refusing at exit 2 in the oracle's
/// words.
/// @param args The parsed arguments.
/// @return The tier, or the refusal.
auto tier_of(const cliapp::parsed_args& args) -> std::expected<reg::tier, domain_error> {
  auto const raw    = cliapp::flag_string(args, "--tier").value_or("");
  auto const parsed = reg::tier_from_text(raw);
  if (!parsed) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid tier: {}", raw)));
  }
  return *parsed;
}

/// @brief One cohort flag's required-then-valid check, in the oracle's
/// wording. The two messages are DIFFERENT sentences and both are reachable.
/// @param args The parsed arguments.
/// @param flag The flag name including dashes.
/// @return The raw value, or the refusal.
auto required_cohort_flag(const cliapp::parsed_args& args, std::string_view flag) -> std::expected<std::string, domain_error> {
  auto const raw = cliapp::flag_string(args, flag);
  if (!raw || raw->empty()) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("{} is required when ranking a cohort", flag)));
  }
  return *raw;
}

/// @brief Assemble the cohort and gates from argv, applying the oracle's
/// exact check ORDER (see this module's header for the nine-step sequence
/// and the probes that isolated it).
/// @param args The parsed arguments.
/// @return The cohort plus its gates, or the first refusal.
auto build_cohort(const cliapp::parsed_args& args) -> std::expected<std::pair<rank_::cohort, rank_::gates>, domain_error> {
  rank_::cohort target;
  rank_::gates  gate_config;

  // 1. --project, ahead of everything else including the gate parses.
  auto const project_raw = cliapp::flag_string(args, "--project");
  if (!project_raw || project_raw->empty()) {
    return std::unexpected(error_from_body(kind_t::invalid_input, "--project is required when ranking a cohort"));
  }
  auto const project_id = cliapp::parse_int64_zig(*project_raw);
  if (!project_id) {
    return std::unexpected(
        error_from_body(kind_t::invalid_input, std::format("invalid --project '{}': expected integer", *project_raw)));
  }
  target.project_id = *project_id;

  // 2-3. The two gate parses, BEFORE the remaining required-flag checks.
  // `--min-samples` is UNSIGNED (the oracle refuses `-1`); `--quality-floor`
  // goes through Zig's float contract, hex literals and `inf` included.
  if (auto const raw = cliapp::flag_string(args, "--min-samples"); raw && !raw->empty()) {
    auto const parsed = cliapp::parse_uint64_zig(*raw);
    if (!parsed) {
      return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --min-samples '{}'", *raw)));
    }
    gate_config.minimum_samples = *parsed;
  }
  if (auto const raw = cliapp::flag_string(args, "--quality-floor"); raw && !raw->empty()) {
    auto const parsed = cliapp::parse_float_zig(*raw);
    if (!parsed) {
      return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --quality-floor '{}'", *raw)));
    }
    gate_config.quality_floor = *parsed;
  }

  // 4-6. Three opaque strings, required only.
  auto const validation_policy = required_cohort_flag(args, "--validation-policy");
  if (!validation_policy) {
    return std::unexpected(validation_policy.error());
  }
  target.validation_policy_version = *validation_policy;
  auto const routing_policy        = required_cohort_flag(args, "--routing-policy");
  if (!routing_policy) {
    return std::unexpected(routing_policy.error());
  }
  target.routing_policy_version = *routing_policy;
  auto const role               = required_cohort_flag(args, "--role");
  if (!role) {
    return std::unexpected(role.error());
  }
  target.role = *role;

  // 7-9. Three enums: required FIRST, then valid. Both messages are live.
  auto const tier_raw = required_cohort_flag(args, "--tier");
  if (!tier_raw) {
    return std::unexpected(tier_raw.error());
  }
  auto const tier_parsed = reg::tier_from_text(*tier_raw);
  if (!tier_parsed) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --tier '{}'", *tier_raw)));
  }
  target.tier_ = *tier_parsed;

  auto const work_type_raw = required_cohort_flag(args, "--work-type");
  if (!work_type_raw) {
    return std::unexpected(work_type_raw.error());
  }
  auto const work_type_parsed = rank_::work_type_from_text(*work_type_raw);
  if (!work_type_parsed) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --work-type '{}'", *work_type_raw)));
  }
  target.work_type_ = *work_type_parsed;

  auto const complexity_raw = required_cohort_flag(args, "--complexity");
  if (!complexity_raw) {
    return std::unexpected(complexity_raw.error());
  }
  auto const complexity_parsed = rank_::complexity_from_text(*complexity_raw);
  if (!complexity_parsed) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --complexity '{}'", *complexity_raw)));
  }
  target.complexity_ = *complexity_parsed;

  target.vendor = cliapp::flag_string(args, "--vendor").value_or("");
  return std::pair{std::move(target), gate_config};
}

/// @brief `models resolve`'s `@tagName`-equivalent for `complexity`.
///
/// TRAP (plan 996, task 6343): the oracle's JSON envelope renders complexity
/// via Zig's `@tagName`, which emits the UNDERSCORED `high_risk`. That is a
/// DIFFERENT string from `rank_::complexity_to_text`, which is deliberately
/// HYPHENATED (`high-risk`) to match the schema CHECK constraint. Reusing
/// `complexity_to_text` here would look like harmless consolidation and
/// silently change this leaf's emitted contract.
auto complexity_tag(rank_::complexity value) -> std::string_view {
  switch (value) {
  case rank_::complexity::bounded:
    return "bounded";
  case rank_::complexity::standard:
    return "standard";
  case rank_::complexity::high_risk:
    return "high_risk";
  }
  return "standard";
}

/// @brief Map the nine-role `roles::role` onto `pk::planning_role`'s four
/// pre-task values.
///
/// Callable only when `mroles::class_of(value) == packet_class::planning`,
/// which is exactly the four cases below; the five task-bound cases are
/// unreachable from the caller's branch and fall through to `planner` only to
/// keep the switch exhaustive under `-Werror -Wswitch`.
auto to_planning_role(mroles::role value) -> pk::planning_role {
  switch (value) {
  case mroles::role::planner:
    return pk::planning_role::planner;
  case mroles::role::spec_reviewer:
    return pk::planning_role::spec_reviewer;
  case mroles::role::ingestor:
    return pk::planning_role::ingestor;
  case mroles::role::orchestrator:
    return pk::planning_role::orchestrator;
  case mroles::role::coder:
  case mroles::role::test_coder:
  case mroles::role::reviewer:
  case mroles::role::research:
  case mroles::role::janitor:
    break;
  }
  return pk::planning_role::planner;
}

/// @brief Adapt a task packet's `facts` evidence into `profile`'s narrower
/// input view.
///
/// The mapping is one-to-one by field name — `profile.cppm`'s header
/// documents why the classifier names its own `fact` type instead of
/// consuming `pk::evidence` directly (this handler is the seam the two
/// layer-2 buckets cannot share). Result strings are VIEWS into `facts`;
/// the caller must keep `facts`'s owner alive for as long as the result.
auto to_profile_facts(const std::vector<pk::evidence>& facts) -> std::vector<prof::fact> {
  std::vector<prof::fact> out;
  out.reserve(facts.size());
  for (const auto& f : facts) {
    out.push_back({.kind           = f.kind,
                   .locator        = f.locator,
                   .text           = f.text,
                   .provenance     = f.provenance,
                   .current_digest = f.current_digest,
                   .id             = f.id});
  }
  return out;
}

/// @brief Append `"key":value` with a bare `null` for an unset optional
/// string, matching `std.json.Stringify`'s handling of a Zig `?[]const u8`.
auto append_optional_string(std::string& out, std::string_view key, const std::optional<std::string>& value) -> void {
  out.push_back('"');
  out.append(key);
  out.append("\":");
  if (value.has_value()) {
    json_text::append_json_string(out, *value);
  } else {
    out.append("null");
  }
}

/// @brief `models resolve --json`'s complete stdout envelope.
///
/// Field order and null handling mirror the oracle's `std.json.Stringify`
/// call in `zig/src/cmd/planar/handlers/models.zig:handleResolve` exactly —
/// see this file's header for the `complexity` trap this render must not
/// paper over.
auto resolve_json(const mroles::resolution& resolution, std::string_view readiness) -> std::string {
  std::string out = "{\"resolution_version\":";
  json_text::append_json_string(out, mroles::resolution_version);
  out.append(",\"role\":");
  json_text::append_json_string(out, mroles::role_to_text(resolution.role_));
  out.append(",\"packet_class\":");
  json_text::append_json_string(out, mroles::packet_class_to_text(resolution.class_));
  out.append(",\"source\":");
  json_text::append_json_string(out, mroles::source_to_text(resolution.source_));
  out.append(std::format(",\"packet_backed\":{}", mroles::packet_backed(resolution) ? "true" : "false"));
  out.append(",\"tier\":");
  json_text::append_json_string(out, reg::tier_to_text(resolution.tier_));
  out.append(",\"work_type\":");
  if (resolution.work_type_.has_value()) {
    json_text::append_json_string(out, rank_::work_type_to_text(*resolution.work_type_));
  } else {
    out.append("null");
  }
  out.append(",\"complexity\":");
  if (resolution.complexity_.has_value()) {
    json_text::append_json_string(out, complexity_tag(*resolution.complexity_));
  } else {
    out.append("null");
  }
  out.append(",\"fallback_reason\":");
  if (resolution.fallback_reason_.has_value()) {
    json_text::append_json_string(out, mroles::fallback_reason_to_text(*resolution.fallback_reason_));
  } else {
    out.append("null");
  }
  out.push_back(',');
  append_optional_string(out, "first_readiness_reason",
                         readiness.empty() ? std::optional<std::string>{} : std::string{readiness});
  out.push_back(',');
  append_optional_string(out, "rule_version",
                         resolution.rule_version.has_value() ? std::optional<std::string>{std::string{*resolution.rule_version}}
                                                             : std::optional<std::string>{});
  out.append("}\n");
  return out;
}

/// @brief `models resolve`'s operator-facing text form, matching the
/// oracle's line-by-line rendering.
auto resolve_text(const mroles::resolution& resolution, std::string_view readiness) -> std::string {
  std::string out = std::format("role   : {} ({} packet)\n", mroles::role_to_text(resolution.role_),
                                mroles::packet_class_to_text(resolution.class_));
  out.append(std::format("tier   : {}\n", reg::tier_to_text(resolution.tier_)));
  if (mroles::packet_backed(resolution)) {
    out.append(std::format("source : packet ({})\n", resolution.rule_version.value_or("?")));
    if (resolution.work_type_.has_value()) {
      out.append(std::format("work   : {}\n", rank_::work_type_to_text(*resolution.work_type_)));
    }
    if (resolution.complexity_.has_value()) {
      out.append(std::format("risk   : {}\n", complexity_tag(*resolution.complexity_)));
    }
  } else {
    out.append(std::format("source : STATIC FALLBACK — {}\n", resolution.fallback_reason_.has_value()
                                                                  ? mroles::fallback_reason_to_text(*resolution.fallback_reason_)
                                                                  : "unknown"));
    if (!readiness.empty()) {
      out.append(std::format("because: {}\n", readiness));
    }
    out.append("         (no work type or complexity: nothing was derived)\n");
  }
  return out;
}

} // namespace

auto models_registry_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const listed = reg::list(**conn);
  if (!listed) {
    return std::unexpected(registry_failure("listing registry", listed.error()));
  }
  // The text renderer returns ZERO BYTES for an empty registry while the
  // JSON one returns the full envelope with `"candidates":[]`. Both are the
  // oracle's; neither is normalized to the other.
  ctx.out() << (wants_json(args) ? rend::registry_json(*listed) : rend::registry_text(*listed));
  return {};
}

auto models_registry_export(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const listed = reg::list(**conn);
  if (!listed) {
    return std::unexpected(registry_failure("listing registry", listed.error()));
  }
  // `--json` MUTES THE STDERR WARNING and changes stdout not at all — the
  // inverse of every other leaf in the family. Captured both ways.
  if (!wants_json(args)) {
    ctx.err() << rend::registry_export_stderr_warning();
  }
  ctx.out() << rend::registry_json(*listed);
  return {};
}

auto models_registry_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const vendor  = cliapp::flag_string(args, "--vendor").value_or("");
  auto const id      = cliapp::flag_string(args, "--id").value_or("");
  auto const created = reg::create(**conn, reg::create_args{
                                               .vendor       = vendor,
                                               .candidate_id = id,
                                               // `--disabled` is the NEGATIVE form: the stored column is `enabled`.
                                               .enabled              = !flag_bool(args, "--disabled"),
                                               .fallback_order       = cliapp::flag_int(args, "--order").value_or(0),
                                               .compatibility_source = "native",
                                           });
  if (!created) {
    return std::unexpected(registry_failure("registering opaque candidate", created.error()));
  }
  ctx.out() << rend::id_line(*created);
  return {};
}

auto models_registry_update(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const updated =
      reg::update(**conn, candidate_of(args), !flag_bool(args, "--disabled"), cliapp::flag_int(args, "--order").value_or(0));
  if (!updated) {
    return std::unexpected(registry_failure("updating candidate", updated.error()));
  }
  return {};
}

auto models_registry_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const removed = reg::remove(**conn, candidate_of(args));
  if (!removed) {
    return std::unexpected(registry_failure("removing candidate", removed.error()));
  }
  return {};
}

auto models_registry_bind(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const tier_ = tier_of(args);
  if (!tier_) {
    return std::unexpected(tier_.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const bound = reg::bind(**conn, candidate_of(args), cliapp::flag_string(args, "--role").value_or(""), *tier_);
  if (!bound) {
    // A MISSING candidate reaches here as `query_failed`, not `not_found` —
    // the FK failure is not translated. `observe` classifies the same
    // missing row as `conflict` (exit 3). Both are pinned.
    return std::unexpected(registry_failure("binding candidate", bound.error()));
  }
  // Zero bytes on success; the oracle prints no confirmation.
  return {};
}

auto models_registry_unbind(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const tier_ = tier_of(args);
  if (!tier_) {
    return std::unexpected(tier_.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  // NOT existence-checked, deliberately: unbinding a binding that never
  // existed exits 0 and prints nothing on the oracle.
  auto const unbound = reg::unbind(**conn, candidate_of(args), cliapp::flag_string(args, "--role").value_or(""), *tier_);
  if (!unbound) {
    return std::unexpected(registry_failure("unbinding candidate", unbound.error()));
  }
  return {};
}

auto models_registry_observe(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const availability_raw = cliapp::flag_string(args, "--availability").value_or("");
  auto const availability     = reg::availability_from_text(availability_raw);
  if (!availability) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid availability: {}", availability_raw)));
  }
  auto const spawn_raw = cliapp::flag_string(args, "--spawn-verification").value_or("");
  auto const spawn     = reg::spawn_verification_from_text(spawn_raw);
  if (!spawn) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid spawn verification: {}", spawn_raw)));
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const evidence_ref = cliapp::flag_string(args, "--evidence-ref").value_or("");
  auto const captured_at  = cliapp::flag_string(args, "--captured-at").value_or("");
  auto const expires_at   = cliapp::flag_string(args, "--expires-at").value_or("");
  auto const written      = reg::observe(**conn, reg::observe_args{
                                                     .candidate_id        = candidate_of(args),
                                                     .host_id             = cliapp::flag_string(args, "--host").value_or(""),
                                                     .observation_version = cliapp::flag_int(args, "--version").value_or(1),
                                                     .availability_       = *availability,
                                                     .spawn_verification_ = *spawn,
                                                     .evidence_ref        = evidence_ref,
                                                     .captured_at         = captured_at,
                                                     .expires_at          = expires_at,
                                                 });
  if (!written) {
    return std::unexpected(registry_failure("recording host observation", written.error()));
  }
  ctx.out() << rend::id_line(*written);
  return {};
}

auto models_registry_eligibility(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const tier_ = tier_of(args);
  if (!tier_) {
    return std::unexpected(tier_.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const host = cliapp::flag_string(args, "--host").value_or("");
  // Host-SCOPED: an observation made by a different host is never
  // substituted, however new it is. That is what makes the `observation`
  // gate answer a question about THIS machine.
  auto const found = reg::get_for_host(**conn, candidate_of(args), host);
  if (!found) {
    return std::unexpected(registry_failure("reading candidate", found.error()));
  }
  auto const role       = cliapp::flag_string(args, "--role").value_or("");
  bool       bound_here = false;
  for (auto const& binding : found->bindings) {
    if (binding.role == role && binding.tier_ == *tier_) {
      bound_here = true;
      break;
    }
  }
  auto const now   = cliapp::flag_string(args, "--now").value_or("");
  auto const gates = reg::evaluate_eligibility(reg::eligibility_input{
      .enabled                         = found->registration_.enabled,
      .binding_present                 = bound_here,
      .role_surface_override_supported = flag_bool(args, "--override-supported"),
      .host_policy_permits             = flag_bool(args, "--policy-permits"),
      .observation                     = found->latest_observation,
      .now                             = now,
  });
  ctx.out() << rend::eligibility_json(candidate_of(args), host, gates);
  return {};
}

auto models_registry_verify_identity(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = candidate_of(args);
  // `get_registration`, NOT `get_for_host`: this leaf declares no `--host`
  // and needs the registered pair alone. `get_for_host("")` refuses the
  // empty host as `invalid_value`, which turned every verify-identity into
  // an exit-1 not-found. See `registry.cppm`'s note on this function.
  auto const found = reg::get_registration(**conn, id);
  if (!found) {
    // The family's ONE leaf that does not use the `<gerund>: <ErrorName>`
    // shape — it names the id instead. Oracle-captured.
    return std::unexpected(error_from_body(kind_t::not_found, std::format("candidate {} not found", id)));
  }
  auto const actual_vendor    = cliapp::flag_string(args, "--actual-vendor").value_or("");
  auto const actual_candidate = cliapp::flag_string(args, "--actual-id").value_or("");
  auto const outcome          = reg::verify_actual_identity(found->vendor, found->candidate_id, actual_vendor, actual_candidate);
  ctx.out() << rend::verify_identity_json(id, outcome);
  return {};
}

auto models_evals(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // THE BRANCH SELECTOR: a NON-EMPTY `--vendor`, and nothing else. Every
  // other cohort flag is inert on its own, and `--vendor ""` falls through
  // to legacy. See this module's header for the six probes.
  auto const vendor = cliapp::flag_string(args, "--vendor").value_or("");
  if (!vendor.empty()) {
    auto built = build_cohort(args);
    if (!built) {
      return std::unexpected(built.error());
    }
    auto conn = ctx.ensure_db();
    if (!conn) {
      return std::unexpected(conn.error());
    }
    auto const ranked = rank_::rank(**conn, built->first, built->second);
    if (!ranked) {
      return std::unexpected(error_from_body(kind_t::generic_failure, "models evals: QueryFailed"));
    }
    ctx.out() << (wants_json(args) ? rend::evals_json(*ranked, built->second) : rend::evals_text(*ranked, built->second));
    return {};
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const aggregated = legacy::aggregate(**conn);
  if (!aggregated) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "models evals: QueryFailed"));
  }
  ctx.out() << (wants_json(args) ? legacy::evals_json(*aggregated) : legacy::evals_text(*aggregated));
  return {};
}

auto models_experiments(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const listed = views_::list_experiments(**conn);
  if (!listed) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "models experiments: QueryFailed"));
  }
  ctx.out() << (wants_json(args) ? rend::experiments_json(*listed) : rend::experiments_text(*listed));
  return {};
}

auto models_outcomes(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `--limit` is declared `(string)` on the tree and carries TWO distinct
  // refusals — an unparseable value and a non-positive one — with different
  // wording. Default 50. Both refusals happen BEFORE the database opens.
  std::int64_t limit = 50;
  if (auto const raw = cliapp::flag_string(args, "--limit"); raw && !raw->empty()) {
    auto const parsed = cliapp::parse_int64_zig(*raw);
    if (!parsed) {
      return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --limit '{}': expected integer", *raw)));
    }
    if (*parsed <= 0) {
      return std::unexpected(error_from_body(kind_t::invalid_input, "--limit must be positive"));
    }
    limit = *parsed;
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const listed = views_::list_outcomes(**conn, limit);
  if (!listed) {
    return std::unexpected(error_from_body(kind_t::generic_failure, "models outcomes: QueryFailed"));
  }
  ctx.out() << (wants_json(args) ? rend::outcomes_json(*listed) : rend::outcomes_text(*listed));
  return {};
}

auto models_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // The database opens BEFORE role parsing, matching the oracle's
  // `ensureDb()` then role-buffer decode — observable only when both would
  // fail, but observable.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const role_raw = cliapp::flag_string(args, "--role").value_or("");
  auto const role_    = mroles::role_from_wire(role_raw);
  if (!role_) {
    return std::unexpected(error_from_body(kind_t::invalid_input, std::format("unknown role '{}'", role_raw)));
  }

  mroles::static_fallback fallback;
  if (auto const raw = cliapp::flag_string(args, "--fallback-tier"); raw && !raw->empty()) {
    auto const parsed = reg::tier_from_text(*raw);
    if (!parsed) {
      return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --fallback-tier '{}'", *raw)));
    }
    fallback.tier_ = *parsed;
  }

  mroles::resolution resolution;
  std::string        readiness;

  if (mroles::class_of(*role_) == mroles::packet_class::task) {
    auto const raw = cliapp::flag_string(args, "--task");
    if (!raw || raw->empty()) {
      return std::unexpected(
          error_from_body(kind_t::invalid_input, std::format("--task is required for task-bound role '{}'", role_raw)));
    }
    auto const task_id = cliapp::parse_int64_zig(*raw);
    if (!task_id) {
      return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --task '{}'", *raw)));
    }

    auto assembled = pk::assemble_task(**conn, *task_id);
    if (!assembled) {
      if (assembled.error() == pk::packet_error::task_not_found) {
        return std::unexpected(error_from_body(kind_t::not_found, std::format("no task with id {}", *task_id)));
      }
      return std::unexpected(error_from_body(kind_t::generic_failure, "assembling packet: QueryFailed"));
    }

    if (assembled->ready()) {
      // `facts` owns the strings `to_profile_facts`'s result views into; it
      // must outlive `outcome` and the `resolve_task_packet` call below.
      auto const facts   = to_profile_facts(assembled->input.facts);
      auto       outcome = prof::compile(facts);
      resolution         = mroles::resolve_task_packet(*role_, true, std::optional<prof::outcome>{std::move(outcome)}, fallback);
    } else {
      resolution = mroles::resolve_task_packet(*role_, false, std::nullopt, fallback);
      if (!assembled->reasons.empty()) {
        readiness = std::string{pk::reason_name(assembled->reasons[0])};
      }
    }
  } else {
    if (auto const raw = cliapp::flag_string(args, "--plan"); raw && !raw->empty()) {
      auto const plan_id = cliapp::parse_int64_zig(*raw);
      if (!plan_id) {
        return std::unexpected(error_from_body(kind_t::invalid_input, std::format("invalid --plan '{}'", *raw)));
      }

      auto assembled = pk::assemble_planning(**conn, to_planning_role(*role_), *plan_id);
      if (!assembled) {
        if (assembled.error() == pk::packet_error::plan_not_found) {
          return std::unexpected(error_from_body(kind_t::not_found, "assembling planning packet: PlanNotFound"));
        }
        return std::unexpected(error_from_body(kind_t::generic_failure, "assembling planning packet: QueryFailed"));
      }
      resolution = mroles::resolve_planning(*role_, assembled->ready(), fallback, pk::policy_version);
      if (!assembled->reasons.empty()) {
        readiness = std::string{pk::planning_reason_name(assembled->reasons[0])};
      }
    } else {
      resolution = mroles::resolve_planning(*role_, std::nullopt, fallback, pk::policy_version);
    }
  }

  ctx.out() << (wants_json(args) ? resolve_json(resolution, readiness) : resolve_text(resolution, readiness));
  return {};
}

} // namespace planar::cmd::handlers
