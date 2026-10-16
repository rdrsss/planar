// brief_next_bucketing.t.cpp: `planar plan next` and `planar-agent brief` share one
// bucketing (plan 1142, task mo-brief-engine; test spec 695, scenario "plan next
// and the brief share bucketing").
//
// The brief reads its claimed, stale and available sets from the same claim-aware
// bucketing `plan next` uses. These cases drive the built `planar` and
// `planar-agent` binaries over one scratch arena holding a live claim, a lapsed
// claim, a blocked task and two free tasks, and hold each bucket the brief
// reports to the matching bucket of `plan next --json`. They also pin that
// `plan next` prints the same bytes before and after a brief ran, so the brief
// neither changed the bucketing nor wrote anything that `plan next` reads.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.json_dom;

#include "parity_harness.hpp"

namespace {

namespace parity = planar::cmd::parity;
namespace jd     = planar::json_dom;

struct world {
  parity::arena         arena = parity::make_arena("brief_next_bucketing");
  std::filesystem::path root  = arena.cpp_root;
  int                   seq   = 0;

  auto run(std::filesystem::path const& bin, std::vector<std::string> const& args) -> parity::capture {
    auto got = parity::run_pinned(bin, args, root, std::format("bnb{}", ++seq));
    INFO(bin.filename().string() << " " << args.front() << " -> " << got.code << ": " << got.err << got.out);
    return got;
  }

  auto planar(std::vector<std::string> const& args) -> parity::capture {
    return run(std::filesystem::path{PLANAR_CPP_BIN}, args);
  }

  auto planar_agent(std::vector<std::string> const& args) -> parity::capture {
    return run(std::filesystem::path{PLANAR_AGENT_CPP_BIN}, args);
  }
};

auto parse(std::string const& text) -> jd::json_value {
  auto parsed = jd::parse_json(text);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

/// The `id` of every task object in `array`, or of `.task` when the entries wrap a claim.
auto task_ids(jd::json_value const* array) -> std::vector<std::int64_t> {
  REQUIRE(array != nullptr);
  std::vector<std::int64_t> out;
  for (auto const& item : array->array) {
    auto const* task = item.find("task");
    out.push_back((task != nullptr ? task : &item)->find("id")->integer);
  }
  std::ranges::sort(out);
  return out;
}

/// The tasks the brief's claims of one state cover, sorted.
auto brief_claimed(jd::json_value const& brief, std::string_view state) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  for (auto const& claim : brief.find("claims")->array) {
    if (claim.find("state")->string == state) {
      for (auto const& id : claim.find("covered_tasks")->array) {
        out.push_back(id.integer);
      }
    }
  }
  std::ranges::sort(out);
  return out;
}

} // namespace

TEST_CASE("plan next and the brief put every task in the same bucket, and the brief leaves plan next unchanged",
          "[cmd][plan][brief][bucketing][1142]") {
  world w;
  REQUIRE(w.planar({"init", "--skip-project", "--allow-no-repo"}).code == 0);
  REQUIRE(w.planar({"plan", "create", "Bucketed"}).code == 0);
  for (auto const* title : {"live", "lapsed", "blocked", "free one", "free two"}) {
    REQUIRE(w.planar({"task", "add", title, "--plan", "1"}).code == 0);
  }
  REQUIRE(w.planar_agent(
               {"claim", "--entity", "task:1", "--vendor", "claude", "--role", "coder", "--ttl", "30m", "--no-locality-probe"})
              .code == 0);
  REQUIRE(w.planar_agent(
               {"claim", "--entity", "task:2", "--vendor", "claude", "--role", "coder", "--ttl", "1s", "--no-locality-probe"})
              .code == 0);
  REQUIRE(w.planar({"task", "update", "3", "--status", "blocked"}).code == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds{2200}); // the one-second lease lapses

  auto const before = w.planar({"plan", "next", "1", "--json"});
  REQUIRE(before.code == 0);
  auto const brief = w.planar_agent({"brief", "1", "--json"});
  REQUIRE(brief.code == 0);
  auto const after = w.planar({"plan", "next", "1", "--json"});
  REQUIRE(after.code == 0);

  // `plan next` keeps its shape and prints the same bytes with a brief in between.
  CHECK(after.out == before.out);
  auto const               next = parse(before.out);
  std::vector<std::string> keys;
  for (auto const& [name, value] : next.object) {
    keys.push_back(name);
  }
  CHECK(keys == std::vector<std::string>{"plan_id", "available", "claimed", "stale", "blocked", "summary"});

  auto const parsed = parse(brief.out);

  // The sets the two views share.
  CHECK(task_ids(next.find("claimed")) == std::vector<std::int64_t>{1});
  CHECK(task_ids(next.find("stale")) == std::vector<std::int64_t>{2});
  CHECK(task_ids(next.find("available")) == std::vector<std::int64_t>{4, 5});
  CHECK(task_ids(next.find("blocked")) == std::vector<std::int64_t>{3});

  CHECK(brief_claimed(parsed, "live") == task_ids(next.find("claimed")));
  CHECK(brief_claimed(parsed, "stale") == task_ids(next.find("stale")));
  std::vector<std::int64_t> readiness;
  for (auto const& item : parsed.find("readiness")->array) {
    readiness.push_back(item.find("id")->integer);
  }
  std::ranges::sort(readiness);
  CHECK(readiness == task_ids(next.find("available")));
  CHECK(parsed.find("counts")->find("blocked")->integer == next.find("summary")->find("blocked")->integer);
  CHECK(parsed.find("counts")->find("blocked")->integer == 1);
}
