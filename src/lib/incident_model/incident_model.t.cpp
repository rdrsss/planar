// @file incident_model.t.cpp
// @brief Unit tests for `planar.incident_model` (plan 1132, task 7372).
//
// What is pinned:
//
//   * Severities compare by ordinal. `critical` is text-smaller than `high`
//     and `low`, so a text comparison fails here and an ordinal one passes.
//   * The diagnostic-to-incident mapping is warning -> medium, error -> high,
//     and only warning and error are recorded.
//   * A fingerprint is sorted and de-duplicated and holds no timestamp; an
//     evidence digest changes with the fingerprint and with each timestamp.
//   * The status set is closed, and `not-recorded` is hyphenated.
//   * The finding order is severity, check id, primary entity (numeric id),
//     earliest evidence time.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.incident_model;

namespace {

namespace im = planar::incident_model;

auto ref(std::string kind, std::int64_t id) -> im::entity_ref {
  return im::entity_ref{.kind = std::move(kind), .id = id};
}

auto make(std::string check, im::diagnostic_severity sev, im::entity_ref primary, std::vector<std::string> times) -> im::finding {
  im::finding f;
  f.check_id       = std::move(check);
  f.severity       = sev;
  f.primary        = primary;
  f.evidence       = {primary};
  f.evidence_times = std::move(times);
  return f;
}

} // namespace

TEST_CASE("severities compare by ordinal and never by text", "[incident_model]") {
  CHECK(im::severity_ordinal(im::severity::info) == 0);
  CHECK(im::severity_ordinal(im::severity::low) == 1);
  CHECK(im::severity_ordinal(im::severity::medium) == 2);
  CHECK(im::severity_ordinal(im::severity::high) == 3);
  CHECK(im::severity_ordinal(im::severity::critical) == 4);
  // "critical" < "high" as text; the ordinal says otherwise.
  CHECK(im::severity_name(im::severity::critical) < im::severity_name(im::severity::high));
  CHECK(im::compare_severity(im::severity::critical, im::severity::high) == std::strong_ordering::greater);
  CHECK(im::compare_severity(im::severity::high, im::severity::medium) == std::strong_ordering::greater);
  CHECK(im::compare_severity(im::severity::medium, im::severity::medium) == std::strong_ordering::equal);
  CHECK(im::max_severity(im::severity::medium, im::severity::high) == im::severity::high);
  CHECK(im::max_severity(im::severity::critical, im::severity::high) == im::severity::critical);
}

TEST_CASE("diagnostic severities map onto the incident scale and only warning and error are recorded", "[incident_model]") {
  CHECK(im::incident_severity(im::diagnostic_severity::info) == im::severity::info);
  CHECK(im::incident_severity(im::diagnostic_severity::warning) == im::severity::medium);
  CHECK(im::incident_severity(im::diagnostic_severity::error) == im::severity::high);
  CHECK_FALSE(im::is_recorded(im::diagnostic_severity::info));
  CHECK(im::is_recorded(im::diagnostic_severity::warning));
  CHECK(im::is_recorded(im::diagnostic_severity::error));
}

TEST_CASE("closed enums round-trip through their names and reject anything else", "[incident_model]") {
  for (auto s : {im::severity::info, im::severity::low, im::severity::medium, im::severity::high, im::severity::critical}) {
    CHECK(im::parse_severity(im::severity_name(s)) == s);
  }
  CHECK_FALSE(im::parse_severity("High").has_value());
  CHECK(im::parse_diagnostic_severity("warning") == im::diagnostic_severity::warning);
  CHECK_FALSE(im::parse_diagnostic_severity("medium").has_value());
  CHECK(im::parse_check_kind("state") == im::check_kind::state);
  CHECK(im::parse_check_kind("event") == im::check_kind::event);
  CHECK_FALSE(im::parse_check_kind("other").has_value());
  for (auto s : {im::incident_state::open, im::incident_state::acknowledged, im::incident_state::reopened,
                 im::incident_state::resolved, im::incident_state::dismissed}) {
    CHECK(im::parse_incident_state(im::incident_state_name(s)) == s);
  }
  for (auto s : {im::incident_source::diagnose_check, im::incident_source::claim_failure, im::incident_source::cli_failure,
                 im::incident_source::conversational}) {
    CHECK(im::parse_incident_source(im::incident_source_name(s)) == s);
  }
  CHECK(im::coverage_state_name(im::coverage_state::not_applicable) == "not_applicable");
}

TEST_CASE("the incident status set is closed at seven values", "[incident_model]") {
  auto all = im::all_incident_statuses();
  REQUIRE(all.size() == 7);
  std::vector<std::string> names;
  for (auto s : all) {
    names.emplace_back(im::incident_status_name(s));
    CHECK(im::parse_incident_status(im::incident_status_name(s)) == s);
  }
  CHECK(names == std::vector<std::string>{"new", "recurring", "escalated", "reopened", "dismissed", "resolved", "not-recorded"});
  CHECK_FALSE(im::parse_incident_status("not_recorded").has_value());
  CHECK_FALSE(im::parse_incident_status("acknowledged").has_value());
}

TEST_CASE("entity refs parse strictly and order by numeric id", "[incident_model]") {
  CHECK(im::parse_entity_ref("claim:4469") == ref("claim", 4469));
  CHECK(im::entity_ref_text(ref("task", 7312)) == "task:7312");
  CHECK_FALSE(im::parse_entity_ref("claim").has_value());
  CHECK_FALSE(im::parse_entity_ref(":4").has_value());
  CHECK_FALSE(im::parse_entity_ref("Claim:4").has_value());
  CHECK_FALSE(im::parse_entity_ref("claim:").has_value());
  CHECK_FALSE(im::parse_entity_ref("claim:4x").has_value());
  CHECK_FALSE(im::parse_entity_ref("claim:-4").has_value());
  CHECK(im::compare_entity_refs(ref("task", 9), ref("task", 10)) == std::strong_ordering::less);
  CHECK(im::compare_entity_refs(ref("claim", 99), ref("task", 1)) == std::strong_ordering::less);
}

TEST_CASE("a fingerprint is sorted, de-duplicated and holds no timestamp", "[incident_model]") {
  std::vector<im::entity_ref> refs{ref("task", 7312), ref("claim", 4469), ref("task", 7312)};
  CHECK(im::fingerprint("claim-superseded-active", refs) == "claim-superseded-active|claim:4469,task:7312");
  std::vector<im::entity_ref> reversed{ref("claim", 4469), ref("task", 7312)};
  CHECK(im::fingerprint("claim-superseded-active", reversed) == im::fingerprint("claim-superseded-active", refs));
  CHECK(im::fingerprint("a-check", {}) == "a-check|");
  // Numeric order inside a kind, not text order.
  std::vector<im::entity_ref> numeric{ref("task", 10), ref("task", 9)};
  CHECK(im::fingerprint("c", numeric) == "c|task:9,task:10");

  auto f1 = make("c", im::diagnostic_severity::warning, ref("task", 1), {"2026-06-01T00:00:00.000Z"});
  auto f2 = make("c", im::diagnostic_severity::warning, ref("task", 1), {"2026-07-01T00:00:00.000Z"});
  CHECK(im::finding_fingerprint(f1) == im::finding_fingerprint(f2));
}

TEST_CASE("an evidence digest follows the fingerprint and every timestamp", "[incident_model]") {
  std::vector<std::string> t{"2026-06-01T00:00:00.000Z", "2026-06-01T00:05:00.000Z"};
  auto                     d = im::evidence_digest("c|task:1", t);
  REQUIRE(d.size() == 64);
  CHECK(std::ranges::all_of(d, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
  CHECK(im::evidence_digest("c|task:1", t) == d);
  CHECK(im::evidence_digest("c|task:2", t) != d);
  // Each violation-defining timestamp matters: first, last, order, and presence.
  CHECK(im::evidence_digest("c|task:1", std::vector<std::string>{"2026-06-01T00:00:01.000Z", t[1]}) != d);
  CHECK(im::evidence_digest("c|task:1", std::vector<std::string>{t[0], "2026-06-01T00:05:01.000Z"}) != d);
  CHECK(im::evidence_digest("c|task:1", std::vector<std::string>{t[1], t[0]}) != d);
  CHECK(im::evidence_digest("c|task:1", std::vector<std::string>{t[0]}) != d);
  CHECK(im::evidence_digest("c|task:1", {}) != d);

  auto f = make("c", im::diagnostic_severity::error, ref("task", 1), t);
  CHECK(im::finding_digest(f) == im::evidence_digest(im::finding_fingerprint(f), t));
}

TEST_CASE("findings sort by severity, check id, entity, then earliest evidence", "[incident_model]") {
  using ds = im::diagnostic_severity;
  std::vector<im::finding> v{
      make("zz-check", ds::info, ref("task", 1), {"2026-06-01T00:00:00.000Z"}),
      make("bb-check", ds::warning, ref("task", 10), {"2026-06-01T00:00:00.000Z"}),
      make("bb-check", ds::warning, ref("task", 9), {"2026-06-02T00:00:00.000Z"}),
      make("bb-check", ds::warning, ref("task", 9), {"2026-06-01T00:00:00.000Z", "2026-05-01T00:00:00.000Z"}),
      make("aa-check", ds::warning, ref("task", 5), {"2026-06-01T00:00:00.000Z"}),
      make("zz-check", ds::error, ref("task", 2), {"2026-06-01T00:00:00.000Z"}),
  };
  im::sort_findings(v);
  std::vector<std::string> got;
  for (const auto& f : v) {
    got.push_back(std::format("{} {} {} {}", im::diagnostic_severity_name(f.severity), f.check_id, im::entity_ref_text(f.primary),
                              im::earliest_evidence(f)));
  }
  CHECK(got == std::vector<std::string>{
                   "error zz-check task:2 2026-06-01T00:00:00.000Z",
                   "warning aa-check task:5 2026-06-01T00:00:00.000Z",
                   "warning bb-check task:9 2026-05-01T00:00:00.000Z",
                   "warning bb-check task:9 2026-06-02T00:00:00.000Z",
                   "warning bb-check task:10 2026-06-01T00:00:00.000Z",
                   "info zz-check task:1 2026-06-01T00:00:00.000Z",
               });
}

TEST_CASE("earliest evidence is the smallest time wherever it sits, and empty with none", "[incident_model]") {
  auto none = make("c", im::diagnostic_severity::info, ref("task", 1), {});
  CHECK(im::earliest_evidence(none).empty());
  auto first = make("c", im::diagnostic_severity::info, ref("task", 1), {"2026-05-01T00:00:00.000Z", "2026-06-01T00:00:00.000Z"});
  auto last  = make("c", im::diagnostic_severity::info, ref("task", 1), {"2026-06-01T00:00:00.000Z", "2026-05-01T00:00:00.000Z"});
  auto mid   = make("c", im::diagnostic_severity::info, ref("task", 1),
                    {"2026-06-01T00:00:00.000Z", "2026-05-01T00:00:00.000Z", "2026-07-01T00:00:00.000Z"});
  CHECK(im::earliest_evidence(first) == "2026-05-01T00:00:00.000Z");
  CHECK(im::earliest_evidence(last) == "2026-05-01T00:00:00.000Z");
  CHECK(im::earliest_evidence(mid) == "2026-05-01T00:00:00.000Z");
}

TEST_CASE("a cluster fingerprint is its grouping key and scope, never its members", "[incident_model]") {
  im::finding claims;
  claims.check_id = "claim-failure-cluster";
  claims.primary  = ref("claim", 1);
  claims.evidence = {ref("claim", 1), ref("claim", 2), ref("claim", 3)};
  claims.group    = im::grouping{.key_parts = {"failure_category=tool_failure"}, .scope = "repo:planar"};
  CHECK(im::finding_fingerprint(claims) == "claim-failure-cluster|failure_category=tool_failure|repo:planar");

  im::finding cli;
  cli.check_id = "cli-failure-cluster";
  cli.primary  = ref("cli_invocation", 9);
  cli.evidence = {ref("cli_invocation", 9)};
  cli.group    = im::grouping{.key_parts = {"task add", "usage"}, .scope = "global"};
  CHECK(im::finding_fingerprint(cli) == "cli-failure-cluster|task add|usage|global");

  // A narrower run over a subset of the members keeps the fingerprint.
  auto narrower     = claims;
  narrower.evidence = {ref("claim", 1)};
  CHECK(im::finding_fingerprint(narrower) == im::finding_fingerprint(claims));
  // A different scope or key is a different incident.
  auto other_scope         = claims;
  other_scope.group->scope = "global";
  CHECK(im::finding_fingerprint(other_scope) != im::finding_fingerprint(claims));
  auto other_key             = claims;
  other_key.group->key_parts = {"failure_category=timeout"};
  CHECK(im::finding_fingerprint(other_key) != im::finding_fingerprint(claims));
}

TEST_CASE("a member digest follows the fingerprint, the member id and the member time", "[incident_model]") {
  im::cluster_member m{.ref = ref("claim", 4469), .time = "2026-06-01T00:00:00.000Z"};
  auto               d = im::member_digest("claim-failure-cluster|failure_category=timeout|global", m);
  REQUIRE(d.size() == 64);
  CHECK(im::member_digest("claim-failure-cluster|failure_category=timeout|global", m) == d);
  CHECK(im::member_digest("claim-failure-cluster|failure_category=timeout|repo:x", m) != d);
  auto other_id = m;
  other_id.ref  = ref("claim", 4470);
  CHECK(im::member_digest("claim-failure-cluster|failure_category=timeout|global", other_id) != d);
  auto other_time = m;
  other_time.time = "2026-06-01T00:00:01.000Z";
  CHECK(im::member_digest("claim-failure-cluster|failure_category=timeout|global", other_time) != d);
}

TEST_CASE("earliest evidence of a cluster includes its member times", "[incident_model]") {
  im::finding f;
  f.check_id = "claim-failure-cluster";
  f.members  = {{ref("claim", 1), "2026-06-03T00:00:00.000Z"}, {ref("claim", 2), "2026-06-01T00:00:00.000Z"}};
  CHECK(im::earliest_evidence(f) == "2026-06-01T00:00:00.000Z");
}
