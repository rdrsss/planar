// @file render.t.cpp
// @brief Byte-exact renderer tests for `planar.engine.models.render` (plan
// 996, task 6096).
//
// ORACLE PROVENANCE. Every expected string in this file is a VERBATIM
// TRANSCRIPTION of bytes the Zig binary wrote to a file, read back with
// `python3 -c "print(repr(open(f,'rb').read()))"` so no shell echo, no
// terminal, and no editor could alter them. Every probe ran with HOME,
// PLANAR_HOME and CODEX_HOME redirected under /tmp.
//
// The fixture, built through the CLI (never by raw SQL):
//
//   export PLANAR_DB=/tmp/op/rj.db PLANAR_CONFIG_PATH=/tmp/op/rj.toml
//   export HOME=/tmp/op/home PLANAR_HOME=/tmp/op/home/.planar
//   cd /tmp/op/rj && $Z init
//   $Z models registry add --vendor anthropic --id claude-opus-5 --order 1
//   $Z models registry add --vendor anthropic --id claude-sonnet-5 --order 2 --disabled
//   $Z models registry bind --candidate 1 --role coder --tier medium
//   $Z models registry observe --candidate 1 --host claude-code --version 1 \
//       --availability available --spawn-verification verified \
//       --evidence-ref ev-1 --captured-at 2026-08-01T00:00:00Z \
//       --expires-at 2026-09-01T00:00:00Z
//
// A note on how these captures were TAKEN, because the first attempt produced
// garbage: `$Z models evals $COHORT_FLAGS` with an unquoted variable does NOT
// word-split in zsh, and the oracle reported
// `error: unknown flag (got --vendor anthropic --project 1 ...)` -- one
// argument containing the whole string. Every capture below was re-taken with
// the flags written out literally.
//
// Field ORDER is part of what is pinned here. Zig's std.json.Stringify emits
// struct fields in declaration order; a reordering would still be valid JSON,
// would still parse, and would still be a parity break.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.models.render;
import planar.engine.models.registry;
import planar.engine.models.ranking;
import planar.engine.models.views;

namespace {

namespace rd  = planar::engine::models::render;
namespace reg = planar::engine::models::registry;
namespace rk  = planar::engine::models::ranking;
namespace vw  = planar::engine::models::views;

/// @brief The two-candidate registry the transcript above describes.
auto fixture_candidates() -> std::vector<reg::candidate> {
  reg::candidate opus{
      .registration_      = {.id                   = 1,
                             .vendor               = "anthropic",
                             .candidate_id         = "claude-opus-5",
                             .enabled              = true,
                             .fallback_order       = 1,
                             .registration_version = 1,
                             .compatibility_source = "native"},
      .bindings           = {reg::binding{.candidate_id = 1, .role = "coder", .tier_ = reg::tier::medium}},
      .latest_observation = reg::host_observation{.id                  = 1,
                                                  .candidate_id        = 1,
                                                  .host_id             = "claude-code",
                                                  .observation_version = 1,
                                                  .availability_       = reg::availability::available,
                                                  .spawn_verification_ = reg::spawn_verification::verified,
                                                  .evidence_ref        = "ev-1",
                                                  .captured_at         = "2026-08-01T00:00:00Z",
                                                  .expires_at          = "2026-09-01T00:00:00Z"},
  };
  reg::candidate sonnet{
      .registration_      = {.id                   = 2,
                             .vendor               = "anthropic",
                             .candidate_id         = "claude-sonnet-5",
                             .enabled              = false,
                             .fallback_order       = 2,
                             .registration_version = 1,
                             .compatibility_source = "native"},
      .bindings           = {},
      .latest_observation = std::nullopt,
  };
  return {std::move(opus), std::move(sonnet)};
}

/// @brief The four ranked rows the oracle produced for the seeded cohort.
///
/// Built by running the real `finalize` over the raw counts rather than by
/// hand-setting the derived fields, so this fixture cannot drift away from
/// the arithmetic it is meant to render.
auto fixture_ranking(const rk::gates& gate_config) -> rk::result {
  struct seed {
    std::int64_t  id;
    const char*   name;
    std::int64_t  order;
    std::uint64_t samples;
    std::uint64_t successes;
    std::uint64_t gate_failures;
  };
  const std::array<seed, 4> seeds{
      {{1, "cand-1", 1, 20, 16, 4}, {2, "cand-2", 2, 20, 12, 8}, {4, "cand-4", 4, 20, 4, 16}, {3, "cand-3", 3, 3, 3, 0}}};

  rk::result out;
  for (const auto& s : seeds) {
    rk::row value{.candidate_id   = s.id,
                  .candidate      = s.name,
                  .vendor         = "anthropic",
                  .fallback_order = s.order,
                  .samples        = s.samples,
                  .successes      = s.successes,
                  .gate_failures  = s.gate_failures};
    rk::finalize(value, gate_config);
    out.rows.push_back(std::move(value));
  }
  std::uint64_t next = 1;
  for (auto& value : out.rows) {
    if (value.insufficient_data || value.below_quality_floor) {
      continue;
    }
    value.rank = next;
    ++next;
  }
  if (next == 1) {
    out.no_recommendation_reason = "no candidate cleared both the minimum-sample and quality-floor gates";
  } else {
    out.recommended = out.rows.front().candidate;
  }
  return out;
}

auto fixture_experiment(std::int64_t samples, std::int64_t eligible) -> vw::experiment {
  return vw::experiment{.id                        = 1,
                        .experiment_key            = "exp-1",
                        .status                    = "running",
                        .vendor                    = "anthropic",
                        .role                      = "coder",
                        .tier                      = "medium",
                        .work_type                 = "engine",
                        .complexity                = "standard",
                        .validation_policy_version = "vp-1",
                        .routing_policy_version    = "rp-1",
                        .manifest_digest           = "digest-1",
                        .operator_approved_at      = "2026-08-01T00:00:00Z",
                        .population_size           = 63,
                        .candidate_count           = 4,
                        .samples                   = samples,
                        .eligible_samples          = eligible};
}

} // namespace

TEST_CASE("models.render: registry list --json matches the oracle byte for byte", "[models]") {
  const auto candidates = fixture_candidates();
  REQUIRE(rd::registry_json(candidates) ==
          "{\"registry_version\":1,\"candidates\":[{\"registration\":{\"id\":1,\"vendor\":\"anthropic\","
          "\"candidate_id\":\"claude-opus-5\",\"enabled\":true,\"fallback_order\":1,\"registration_version\":1,"
          "\"compatibility_source\":\"native\"},\"bindings\":[{\"candidate_id\":1,\"role\":\"coder\","
          "\"tier\":\"medium\"}],\"latest_observation\":{\"id\":1,\"candidate_id\":1,\"host_id\":\"claude-code\","
          "\"observation_version\":1,\"availability\":\"available\",\"spawn_verification\":\"verified\","
          "\"evidence_ref\":\"ev-1\",\"captured_at\":\"2026-08-01T00:00:00Z\","
          "\"expires_at\":\"2026-09-01T00:00:00Z\"}},{\"registration\":{\"id\":2,\"vendor\":\"anthropic\","
          "\"candidate_id\":\"claude-sonnet-5\",\"enabled\":false,\"fallback_order\":2,"
          "\"registration_version\":1,\"compatibility_source\":\"native\"},\"bindings\":[],"
          "\"latest_observation\":null}],\"migration_warning\":\"legacy catalog compatibility is one-window and "
          "non-authoritative\"}\n");
}

TEST_CASE("models.render: an empty registry still carries version and warning", "[models]") {
  // HAZARD 6, empty input. Not `[]` alone and not an error -- the full
  // envelope with an empty array inside it.
  REQUIRE(rd::registry_json({}) ==
          "{\"registry_version\":1,\"candidates\":[],\"migration_warning\":\"legacy catalog compatibility is "
          "one-window and non-authoritative\"}\n");
  // And the TEXT view of an empty registry is genuinely EMPTY -- no "none"
  // line, no header. Captured: exit 0, zero bytes on stdout.
  REQUIRE(rd::registry_text({}).empty());
}

TEST_CASE("models.render: registry list text matches the oracle byte for byte", "[models]") {
  const auto candidates = fixture_candidates();
  REQUIRE(rd::registry_text(candidates) == "1 anthropic claude-opus-5 enabled=true order=1 bindings=1 observation=present\n"
                                           "2 anthropic claude-sonnet-5 enabled=false order=2 bindings=0 observation=none\n");
}

TEST_CASE("models.render: registry export adds a stderr warning, not a stdout one", "[models]") {
  const auto candidates = fixture_candidates();
  // `export` emits the SAME stdout envelope as `list --json` -- captured
  // identical -- which is why ONE function, registry_json, serves both leaves
  // and they cannot drift. There is no second renderer to compare it against,
  // so this test previously asserted `registry_json(x) == registry_json(x)`:
  // literally X == X, which holds for any implementation whatsoever (review
  // finding F7).
  //
  // What IS checkable, and what actually regresses, is the relationship
  // between the two channels: the same `migration_warning` text must appear
  // in the stdout envelope AND in the stderr line, and the stderr line must
  // stay JSON-free so a pipeline that forgot --json still gets parseable
  // stdout.
  const auto json    = rd::registry_json(candidates);
  const auto warning = rd::registry_export_stderr_warning();

  REQUIRE(warning == std::format("warning: {}\n", rd::migration_warning));
  REQUIRE(json.find(std::format(R"("migration_warning":"{}")", rd::migration_warning)) != std::string::npos);
  // Byte-pinned as well as related, so redefining `migration_warning` cannot
  // keep both sides "consistent" while silently changing the operator-visible
  // text.
  REQUIRE(warning == "warning: legacy catalog compatibility is one-window and non-authoritative\n");

  // stdout is JSON; stderr is emphatically not.
  REQUIRE(warning.find("{") == std::string::npos);
  REQUIRE(json.starts_with("{"));
  // And the warning goes to stderr ONLY -- no `warning: ` prefix leaks into
  // the parseable stream.
  REQUIRE(json.find("warning: ") == std::string::npos);
}

TEST_CASE("models.render: add and observe print a bare id line", "[models]") {
  REQUIRE(rd::id_line(1) == "1\n");
  REQUIRE(rd::id_line(42) == "42\n");
}

TEST_CASE("models.render: eligibility JSON matches the oracle byte for byte", "[models]") {
  const reg::eligibility partial{.cli_available                   = true,
                                 .exact_spawn_verified            = true,
                                 .role_tier_bound                 = true,
                                 .role_surface_override_supported = false,
                                 .host_policy_permits             = false,
                                 .observation_fresh               = true};
  REQUIRE(rd::eligibility_json(1, "claude-code", partial) ==
          "{\"candidate\":1,\"host\":\"claude-code\",\"eligible\":false,\"gates\":{\"cli_available\":true,"
          "\"exact_spawn_verified\":true,\"role_tier_bound\":true,\"role_surface_override_supported\":false,"
          "\"host_policy_permits\":false,\"observation_fresh\":true},"
          "\"reasons\":[\"role_surface_override_unsupported\",\"host_policy_denied\"]}\n");

  const reg::eligibility all{.cli_available                   = true,
                             .exact_spawn_verified            = true,
                             .role_tier_bound                 = true,
                             .role_surface_override_supported = true,
                             .host_policy_permits             = true,
                             .observation_fresh               = true};
  REQUIRE(rd::eligibility_json(1, "claude-code", all) ==
          "{\"candidate\":1,\"host\":\"claude-code\",\"eligible\":true,\"gates\":{\"cli_available\":true,"
          "\"exact_spawn_verified\":true,\"role_tier_bound\":true,\"role_surface_override_supported\":true,"
          "\"host_policy_permits\":true,\"observation_fresh\":true},\"reasons\":[]}\n");
}

TEST_CASE("models.render: verify-identity JSON matches the oracle byte for byte", "[models]") {
  // Three of the four outcomes below ARE oracle captures from
  // `models registry verify-identity`. The fourth is not, and the file used to
  // present all four as though they were (review finding F9). Re-probed
  // against the live binary:
  //
  //   --actual-vendor and --actual-id are BOTH `required` in the schema, and
  //   passing them EMPTY yields vendor_mismatch, not missing_actual_identity:
  //
  //     $Z models registry verify-identity --candidate 1 \
  //         --actual-vendor "" --actual-id ""
  //     -> exit 0, {"candidate":1,"identity":"vendor_mismatch"}
  //
  //   `missing_actual_identity` needs a NULL on one side, which this leaf
  //   cannot produce. It is reachable only through the routing-store path
  //   (zig/src/engine/routing/store.zig:590), where the columns are nullable.
  //
  // The pin stays: verify_identity_json is a pure function over the enum, the
  // envelope shape is the SAME for every arm, and the enum spelling is pinned
  // independently in registry.t.cpp. What is corrected is the claim about
  // where the bytes came from -- on a project whose method is that
  // expectations come from RUNNING the oracle, a false provenance note is what
  // makes the next reader stop checking.
  REQUIRE(rd::verify_identity_json(1, reg::identity_verification::matched) == "{\"candidate\":1,\"identity\":\"matched\"}\n");
  REQUIRE(rd::verify_identity_json(1, reg::identity_verification::candidate_mismatch) ==
          "{\"candidate\":1,\"identity\":\"candidate_mismatch\"}\n");
  REQUIRE(rd::verify_identity_json(1, reg::identity_verification::vendor_mismatch) ==
          "{\"candidate\":1,\"identity\":\"vendor_mismatch\"}\n");
  REQUIRE(rd::verify_identity_json(7, reg::identity_verification::missing_actual_identity) ==
          "{\"candidate\":7,\"identity\":\"missing_actual_identity\"}\n");
}

TEST_CASE("models.render: evals JSON BRANCH 1 matches the oracle byte for byte", "[models]") {
  const rk::gates defaults;
  const auto      outcome = fixture_ranking(defaults);
  REQUIRE(rd::evals_json(outcome, defaults) ==
          "{\"version\":\"routing-ranking-v1\",\"evidence\":\"declared_experiment\",\"gates\":{\"minimum_samples\""
          ":5,\"quality_floor\":0.5},\"rows\":[{\"candidate_id\":1,\"candidate\":\"cand-1\",\"vendor\":"
          "\"anthropic\",\"fallback_order\":1,\"samples\":20,\"successes\":16,\"gate_failures\":4,"
          "\"excess_attempts\":0,\"mean_latency_ms\":null,\"mean_cost_micros\":null,\"measured_samples\":0,"
          "\"raw_rate\":0.8,\"wilson_lower\":0.5839825677481064,\"gate_failure_rate\":0.2,"
          "\"expected_excess_iterations\":0,\"insufficient_data\":false,\"below_quality_floor\":false,\"rank\":1},"
          "{\"candidate_id\":2,\"candidate\":\"cand-2\",\"vendor\":\"anthropic\",\"fallback_order\":2,"
          "\"samples\":20,\"successes\":12,\"gate_failures\":8,\"excess_attempts\":0,\"mean_latency_ms\":null,"
          "\"mean_cost_micros\":null,\"measured_samples\":0,\"raw_rate\":0.6,"
          "\"wilson_lower\":0.38658150076225317,\"gate_failure_rate\":0.4,\"expected_excess_iterations\":0,"
          "\"insufficient_data\":false,\"below_quality_floor\":true,\"rank\":null},{\"candidate_id\":4,"
          "\"candidate\":\"cand-4\",\"vendor\":\"anthropic\",\"fallback_order\":4,\"samples\":20,"
          "\"successes\":4,\"gate_failures\":16,\"excess_attempts\":0,\"mean_latency_ms\":null,"
          "\"mean_cost_micros\":null,\"measured_samples\":0,\"raw_rate\":0.2,"
          "\"wilson_lower\":0.08065766257979808,\"gate_failure_rate\":0.8,\"expected_excess_iterations\":0,"
          "\"insufficient_data\":false,\"below_quality_floor\":true,\"rank\":null},{\"candidate_id\":3,"
          "\"candidate\":\"cand-3\",\"vendor\":\"anthropic\",\"fallback_order\":3,\"samples\":3,\"successes\":3,"
          "\"gate_failures\":0,\"excess_attempts\":0,\"mean_latency_ms\":null,\"mean_cost_micros\":null,"
          "\"measured_samples\":0,\"raw_rate\":1,\"wilson_lower\":0.4385029682449545,\"gate_failure_rate\":0,"
          "\"expected_excess_iterations\":0,\"insufficient_data\":true,\"below_quality_floor\":false,"
          "\"rank\":null}],\"recommended\":\"cand-1\",\"no_recommendation_reason\":null}\n");
}

TEST_CASE("models.render: an integral double prints WITHOUT a fractional part", "[models]") {
  // Pinned on its own because it is the single formatting rule most likely to
  // diverge. cand-3's raw_rate is exactly 1.0 and the oracle prints `1`, not
  // `1.0`; its gate_failure_rate is exactly 0.0 and prints `0`, not `0.0`.
  // A `{:.1f}`-style renderer would produce valid JSON that fails parity on
  // every single row.
  const rk::gates defaults;
  const auto      json = rd::evals_json(fixture_ranking(defaults), defaults);
  REQUIRE(json.find("\"raw_rate\":1,") != std::string::npos);
  REQUIRE(json.find("\"raw_rate\":1.0") == std::string::npos);
  REQUIRE(json.find("\"gate_failure_rate\":0,") != std::string::npos);
  REQUIRE(json.find("\"expected_excess_iterations\":0,") != std::string::npos);
  // ...while a genuinely fractional value keeps every significant digit.
  REQUIRE(json.find("\"wilson_lower\":0.38658150076225317,") != std::string::npos);
  // The gate echo obeys the same rule: 0.5, not 0.50.
  REQUIRE(json.find("\"quality_floor\":0.5}") != std::string::npos);
}

TEST_CASE("models.render: evals JSON BRANCH 2 names the gate that blocked everything", "[models]") {
  const rk::gates raised{.minimum_samples = 5, .quality_floor = 0.99};
  const auto      json = rd::evals_json(fixture_ranking(raised), raised);
  REQUIRE(json.starts_with("{\"version\":\"routing-ranking-v1\",\"evidence\":\"declared_experiment\","
                           "\"gates\":{\"minimum_samples\":5,\"quality_floor\":0.99},\"rows\":["));
  REQUIRE(json.ends_with("\"recommended\":null,\"no_recommendation_reason\":\"no candidate cleared both the "
                         "minimum-sample and quality-floor gates\"}\n"));
  // cand-3 keeps insufficient_data / below_quality_floor false even at 0.99:
  // the sample gate short-circuits. Oracle-verified at this exact floor.
  REQUIRE(json.find("\"candidate\":\"cand-3\"") != std::string::npos);
  REQUIRE(json.find("\"insufficient_data\":true,\"below_quality_floor\":false,\"rank\":null}") != std::string::npos);
  // No row carries a rank.
  REQUIRE(json.find("\"rank\":1") == std::string::npos);
}

TEST_CASE("models.render: evals JSON BRANCH 3 distinguishes no-evidence from all-gated", "[models]") {
  const rk::gates defaults;
  rk::result      empty;
  empty.no_recommendation_reason = "no cohort-eligible declared-experiment samples";
  REQUIRE(rd::evals_json(empty, defaults) ==
          "{\"version\":\"routing-ranking-v1\",\"evidence\":\"declared_experiment\",\"gates\":{\"minimum_samples\""
          ":5,\"quality_floor\":0.5},\"rows\":[],\"recommended\":null,\"no_recommendation_reason\":\"no "
          "cohort-eligible declared-experiment samples\"}\n");
}

TEST_CASE("models.render: the evals scorecard table matches the oracle byte for byte", "[models]") {
  // Column widths, the em-dash in the banner, the two-decimal gate echo, the
  // `--` / `n/a` rank placeholders, the gated-row suffixes, and the
  // `latency:unmeasured` tail are ALL part of this comparison.
  const rk::gates defaults;
  REQUIRE(rd::evals_text(fixture_ranking(defaults), defaults) ==
          "routing evidence ranking (routing-ranking-v1) \u2014 declared-experiment samples only, read-only:\n"
          "  gates: minimum_samples=5 quality_floor=0.50\n"
          "\n"
          "  rank candidate                    samples  success      raw   wilson  gatefail  excess\n"
          "  1    cand-1                            20       16    0.800    0.584     0.200    0.00  "
          "latency:unmeasured\n"
          "  --   cand-2                            20       12    0.600    0.387     0.400    0.00  "
          "below_quality_floor  latency:unmeasured\n"
          "  --   cand-4                            20        4    0.200    0.081     0.800    0.00  "
          "below_quality_floor  latency:unmeasured\n"
          "  n/a  cand-3                             3        3    1.000    0.439     0.000    0.00  "
          "insufficient_data  latency:unmeasured\n"
          "\n"
          "recommended: cand-1 (preview only; writes nothing)\n");
}

TEST_CASE("models.render: an empty scorecard prints NO table and NO footer", "[models]") {
  // The easy mistake is to print the header row and a "no recommendation"
  // footer around an empty table. The oracle prints neither -- the emptiness
  // line is the whole body.
  const rk::gates defaults;
  rk::result      empty;
  empty.no_recommendation_reason = "no cohort-eligible declared-experiment samples";
  const auto text                = rd::evals_text(empty, defaults);
  REQUIRE(text == "routing evidence ranking (routing-ranking-v1) \u2014 declared-experiment samples only, "
                  "read-only:\n"
                  "  gates: minimum_samples=5 quality_floor=0.50\n"
                  "\n"
                  "  (no cohort-eligible declared-experiment samples)\n");
  REQUIRE(text.find("rank candidate") == std::string::npos);
  REQUIRE(text.find("no recommendation:") == std::string::npos);
}

TEST_CASE("models.render: a fully gated scorecard DOES print the footer", "[models]") {
  // Contrast with the empty case above: rows exist, so the table and the
  // no-recommendation footer both appear.
  const rk::gates raised{.minimum_samples = 5, .quality_floor = 0.99};
  const auto      text = rd::evals_text(fixture_ranking(raised), raised);
  REQUIRE(text.find("  gates: minimum_samples=5 quality_floor=0.99\n") != std::string::npos);
  REQUIRE(text.find("rank candidate") != std::string::npos);
  REQUIRE(text.ends_with("\nno recommendation: no candidate cleared both the minimum-sample and quality-floor "
                         "gates\n"));
  REQUIRE(text.find("recommended: ") == std::string::npos);
}

TEST_CASE("models.render: a measured latency replaces the unmeasured tail", "[models]") {
  const rk::gates defaults;
  auto            outcome         = fixture_ranking(defaults);
  outcome.rows[0].mean_latency_ms = 1234.6;
  const auto text                 = rd::evals_text(outcome, defaults);
  // Rounded to whole milliseconds, with the unit suffix.
  REQUIRE(text.find("  1235ms\n") != std::string::npos);
  // The other three rows still say unmeasured -- the flag is per row.
  REQUIRE(text.find("latency:unmeasured") != std::string::npos);
}

TEST_CASE("models.render: experiments JSON matches the oracle byte for byte", "[models]") {
  const std::array<vw::experiment, 1> one{fixture_experiment(64, 63)};
  REQUIRE(rd::experiments_json(one) ==
          "{\"views_version\":\"routing-views-v1\",\"experiments\":[{\"id\":1,\"experiment_key\":\"exp-1\","
          "\"status\":\"running\",\"vendor\":\"anthropic\",\"role\":\"coder\",\"tier\":\"medium\","
          "\"work_type\":\"engine\",\"complexity\":\"standard\",\"validation_policy_version\":\"vp-1\","
          "\"routing_policy_version\":\"rp-1\",\"manifest_digest\":\"digest-1\","
          "\"operator_approved_at\":\"2026-08-01T00:00:00Z\",\"population_size\":63,\"candidate_count\":4,"
          "\"samples\":64,\"eligible_samples\":63}]}\n");

  // HAZARD 6, empty input.
  REQUIRE(rd::experiments_json({}) == "{\"views_version\":\"routing-views-v1\",\"experiments\":[]}\n");
}

TEST_CASE("models.render: experiments text matches the oracle byte for byte", "[models]") {
  const std::array<vw::experiment, 1> one{fixture_experiment(64, 63)};
  // Note the trailing BLANK line after the last stanza -- present in the
  // capture, and easy to trim by accident.
  REQUIRE(rd::experiments_text(one) == "routing experiments (routing-views-v1) \u2014 read-only:\n"
                                       "\n"
                                       "  [1] exp-1  status=running\n"
                                       "       cohort: anthropic/coder/medium/engine/standard  policies: vp-1 + rp-1\n"
                                       "       manifest: digest-1 approved 2026-08-01T00:00:00Z  population=63 candidates=4\n"
                                       "       samples: 64 recorded, 63 counted (1 excluded)\n"
                                       "\n");

  // The excluded count is a DIFFERENCE, so an all-counted experiment reports
  // zero rather than omitting the parenthetical.
  const std::array<vw::experiment, 1> clean{fixture_experiment(63, 63)};
  REQUIRE(rd::experiments_text(clean).find("samples: 63 recorded, 63 counted (0 excluded)\n") != std::string::npos);

  // HAZARD 6, empty input: a sentence, not an empty string and not a header.
  REQUIRE(rd::experiments_text({}) == "no declared routing experiments\n");
}

TEST_CASE("models.render: outcomes JSON matches the oracle byte for byte", "[models]") {
  const std::array<vw::outcome, 2> rows{vw::outcome{.id                           = 64,
                                                    .experiment_id                = 1,
                                                    .logical_work_item_id         = "w-1-0",
                                                    .role                         = "coder",
                                                    .vendor                       = "anthropic",
                                                    .candidate                    = "cand-1",
                                                    .tier                         = "medium",
                                                    .work_type                    = "engine",
                                                    .complexity                   = "standard",
                                                    .terminal_state               = "candidate_mismatch",
                                                    .quality_success              = false,
                                                    .counts_toward_recommendation = false,
                                                    .exclusion_reason             = "actual_candidate_differs",
                                                    .finalized_at                 = "2026-08-03T00:00:00Z"},
                                        vw::outcome{.id                           = 63,
                                                    .experiment_id                = 1,
                                                    .logical_work_item_id         = "w-4-19",
                                                    .role                         = "coder",
                                                    .vendor                       = "anthropic",
                                                    .candidate                    = "cand-4",
                                                    .tier                         = "medium",
                                                    .work_type                    = "engine",
                                                    .complexity                   = "standard",
                                                    .terminal_state               = "quality_failed",
                                                    .quality_success              = false,
                                                    .counts_toward_recommendation = true,
                                                    .exclusion_reason             = std::nullopt,
                                                    .finalized_at                 = "2026-08-03T00:00:00Z"}};

  REQUIRE(rd::outcomes_json(rows) ==
          "{\"views_version\":\"routing-views-v1\",\"outcomes\":[{\"id\":64,\"experiment_id\":1,"
          "\"logical_work_item_id\":\"w-1-0\",\"role\":\"coder\",\"vendor\":\"anthropic\","
          "\"candidate\":\"cand-1\",\"tier\":\"medium\",\"work_type\":\"engine\",\"complexity\":\"standard\","
          "\"terminal_state\":\"candidate_mismatch\",\"quality_success\":false,"
          "\"counts_toward_recommendation\":false,\"exclusion_reason\":\"actual_candidate_differs\","
          "\"finalized_at\":\"2026-08-03T00:00:00Z\"},{\"id\":63,\"experiment_id\":1,"
          "\"logical_work_item_id\":\"w-4-19\",\"role\":\"coder\",\"vendor\":\"anthropic\","
          "\"candidate\":\"cand-4\",\"tier\":\"medium\",\"work_type\":\"engine\",\"complexity\":\"standard\","
          "\"terminal_state\":\"quality_failed\",\"quality_success\":false,"
          "\"counts_toward_recommendation\":true,\"exclusion_reason\":null,"
          "\"finalized_at\":\"2026-08-03T00:00:00Z\"}]}\n");

  // HAZARD 6, empty input.
  REQUIRE(rd::outcomes_json({}) == "{\"views_version\":\"routing-views-v1\",\"outcomes\":[]}\n");
}

TEST_CASE("models.render: outcomes text always names an exclusion's reason", "[models]") {
  const std::array<vw::outcome, 2> rows{vw::outcome{.id                           = 64,
                                                    .experiment_id                = 1,
                                                    .logical_work_item_id         = "w-1-0",
                                                    .role                         = "coder",
                                                    .vendor                       = "anthropic",
                                                    .candidate                    = "cand-1",
                                                    .tier                         = "medium",
                                                    .work_type                    = "engine",
                                                    .complexity                   = "standard",
                                                    .terminal_state               = "candidate_mismatch",
                                                    .quality_success              = false,
                                                    .counts_toward_recommendation = false,
                                                    .exclusion_reason             = "actual_candidate_differs",
                                                    .finalized_at                 = "2026-08-03T00:00:00Z"},
                                        vw::outcome{.id                           = 63,
                                                    .experiment_id                = 1,
                                                    .logical_work_item_id         = "w-4-19",
                                                    .role                         = "coder",
                                                    .vendor                       = "anthropic",
                                                    .candidate                    = "cand-4",
                                                    .tier                         = "medium",
                                                    .work_type                    = "engine",
                                                    .complexity                   = "standard",
                                                    .terminal_state               = "quality_failed",
                                                    .quality_success              = false,
                                                    .counts_toward_recommendation = true,
                                                    .exclusion_reason             = std::nullopt,
                                                    .finalized_at                 = "2026-08-03T00:00:00Z"}};

  REQUIRE(rd::outcomes_text(rows) == "terminal outcomes (routing-views-v1) \u2014 read-only:\n"
                                     "\n"
                                     "  [64] cand-1  candidate_mismatch  w-1-0\n"
                                     "       EXCLUDED from recommendations: actual_candidate_differs\n"
                                     "\n"
                                     "  [63] cand-4  quality_failed  w-4-19\n"
                                     "       counts toward recommendation (quality_success=no)\n"
                                     "\n");

  // A passing sample reads `yes`; only the word changes.
  auto passing            = rows[1];
  passing.quality_success = true;
  const std::array<vw::outcome, 1> one_pass{passing};
  REQUIRE(rd::outcomes_text(one_pass).find("counts toward recommendation (quality_success=yes)\n") != std::string::npos);

  // An excluded row with NO recorded reason still says something rather than
  // printing a bare colon. The schema makes this unreachable through the CLI,
  // but a hand-edited database must not produce an unexplained exclusion.
  auto reasonless             = rows[0];
  reasonless.exclusion_reason = std::nullopt;
  const std::array<vw::outcome, 1> one_orphan{reasonless};
  REQUIRE(rd::outcomes_text(one_orphan).find("EXCLUDED from recommendations: unknown\n") != std::string::npos);

  // HAZARD 6, empty input.
  REQUIRE(rd::outcomes_text({}) == "no recorded terminal outcomes\n");
}

TEST_CASE("models.render: the registry envelope routes opaque fields through the shared escape table", "[models]") {
  // The escape TABLE itself is pinned once, in json_text.t.cpp. What this
  // bucket owes is proof that the envelope actually routes its opaque
  // operator-supplied fields through it rather than interpolating them raw --
  // candidate ids, vendors, roles, host ids and evidence refs are all stored
  // verbatim, so a raw `{}` on any one of them emits invalid JSON.
  //
  // Every metacharacter below is placed in a DIFFERENT field, so a single
  // unescaped interpolation is named by the assertion that fails rather than
  // masked by a neighbour.
  reg::candidate hostile{
      .registration_      = {.id                   = 1,
                             .vendor               = "ven\"dor",
                             .candidate_id         = std::string{"cand\bid\f!", 9},
                             .enabled              = true,
                             .fallback_order       = 1,
                             .registration_version = 1,
                             .compatibility_source = "back\\slash"},
      .bindings           = {reg::binding{.candidate_id = 1, .role = "ro\nle", .tier_ = reg::tier::medium}},
      .latest_observation = reg::host_observation{.id                  = 1,
                                                  .candidate_id        = 1,
                                                  .host_id             = std::string{"ho\x01st", 5},
                                                  .observation_version = 1,
                                                  .availability_       = reg::availability::available,
                                                  .spawn_verification_ = reg::spawn_verification::verified,
                                                  .evidence_ref        = "ev/1\tref",
                                                  .captured_at         = "2026-08-01T00:00:00Z",
                                                  .expires_at          = "2026-09-01T00:00:00Z"},
  };
  const std::array<reg::candidate, 1> one{hostile};
  const auto                          json = rd::registry_json(one);

  REQUIRE(json.find(R"("vendor":"ven\"dor")") != std::string::npos);
  // \b and \f are the SHORT forms, not `\u0008` / `\u000c`. Oracle-verified:
  // `annotate add --body <0x08><0x0c> --json` emits `\b` and `\f`.
  REQUIRE(json.find(R"("candidate_id":"cand\bid\f!")") != std::string::npos);
  REQUIRE(json.find(R"("compatibility_source":"back\\slash")") != std::string::npos);
  REQUIRE(json.find(R"("role":"ro\nle")") != std::string::npos);
  // A C0 byte with no short form takes LOWERCASE `\u00xx`.
  REQUIRE(json.find(R"("host_id":"ho\u0001st")") != std::string::npos);
  // `/` stays bare; the tab becomes `\t`.
  REQUIRE(json.find(R"("evidence_ref":"ev/1\tref")") != std::string::npos);
  // No raw control byte survived anywhere in the envelope.
  REQUIRE(json.find('\x08') == std::string::npos);
  REQUIRE(json.find('\x0c') == std::string::npos);
  REQUIRE(json.find('\x01') == std::string::npos);
}

TEST_CASE("models.render: an opaque identifier survives into the rendered envelope", "[models]") {
  // End-to-end: a candidate id full of characters that would break a naive
  // renderer still round-trips through the registry envelope.
  std::vector<reg::candidate> hostile{reg::candidate{
      .registration_      = {.id                   = 1,
                             .vendor               = "vend\"or",
                             .candidate_id         = "id\\with/slash",
                             .enabled              = true,
                             .fallback_order       = 0,
                             .registration_version = 1,
                             .compatibility_source = "native"},
      .bindings           = {},
      .latest_observation = std::nullopt,
  }};
  REQUIRE(rd::registry_json(hostile).find("\"vendor\":\"vend\\\"or\"") != std::string::npos);
  REQUIRE(rd::registry_json(hostile).find("\"candidate_id\":\"id\\\\with/slash\"") != std::string::npos);
}
