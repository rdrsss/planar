// @file readpin.t.cpp
// @brief Byte-level pins on what the six `planar-watch` READ verbs — feed,
// ps, claims, actions, plans, log — actually print over a seeded database
// (plan 996, task 6545; decision 1034).
//
// ## What this replaces, and why it could not be a port
//
// Decision 1034 deleted `planar-watch parity: the six read verbs agree with
// the oracle over a seeded database` in the same commit that deleted `zig/`.
// That case's SUBJECT was cross-implementation agreement: one arena, one
// seeded database, two readers. It tolerated a random `claim_token` and
// wall-clock row timestamps only because both readers read the SAME rows, so
// the volatility cancelled on both sides of the diff. With one
// implementation there is no second reader and nothing cancels.
//
// What went with it was the only byte-level pin on the six verbs' RENDERED
// OUTPUT. `format.t.cpp` tests the column helpers in isolation and asserts
// no rendered verb output; `handlers.t.cpp` covers behaviour — flags,
// refusals, JSON field presence, exit codes — with one exact-byte assertion
// across thirty-four cases; `parity.t.cpp` pins help pages and the `schema`
// catalog. None of them pins a printed row. This file does.
//
// ## The obstacle, and the option taken
//
// Task 6540 measured it: re-running the fixture twice showed every one of 47
// read-verb outputs differing between runs. `claim_token` is 32 fresh random
// hex characters per `pull`; every row timestamp is the real wall clock at
// millisecond precision; `repo_root` and `worktree_path` carry the scratch
// arena's own path, which is unique per run.
//
// Three ways to pin that against a single binary were on the table. This
// file takes the FIRST and cheapest — substitute the fixture's own known
// values, pin every other byte exactly — but performs the substitution AT
// THE FIXTURE rather than in the expectation. After seeding through the CLI
// exactly as an operator would, `freeze_fixture` below rewrites the volatile
// COLUMNS to fixed, distinct, deterministic values of the same shape. From
// there every byte the six verbs derive from a row is a literal below.
//
// The alternatives were rejected on what they cover, not on cost:
//
//   * RENDERER-LEVEL, with rows constructed in memory (what
//     `src/lib/engine/runtime/agentrender.t.cpp` does). The six verbs expose
//     no such seam: `ledger.cppm` and `live.cppm` export handlers taking a
//     `context&` (a live SQLite handle) and `parsed_args`, and the only
//     separable render functions are the column helpers `format.t.cpp`
//     already covers. Taking this route would mean EXTRACTING a new seam
//     from production code and then pinning the seam — pinning less than
//     the deleted case did (no SQL, no filter, no ordering, no envelope)
//     while changing the code under test to make the test possible.
//
//   * DETERMINISM AT THE SOURCE — a seeded RNG for `claim_token` and an
//     injectable clock. It reaches strictly one byte more than this file
//     does (`generated_at`, below) and changes three binaries' production
//     code to get it.
//
// WHAT WAS FORBIDDEN, AND IS NOT DONE HERE: a normalizer that blanks every
// token and timestamp. Task 6540 refused to write one for a stated reason —
// it is permissive enough to mask the rendering regressions the case exists
// to catch. Nothing below blanks a rendered value. The fixture's values are
// KNOWN because the fixture set them, and they appear in the expectations as
// literals.
//
// ## The one byte that is not pinned, and why nothing here can pin it
//
// `generated_at`. Four of the sixteen outputs open with
// `{"generated_at":"<host clock at emit>"...`, read from
// `format::now_iso()` — NOT from the database. `format.cppm`'s own header
// says so and says why no test asserts a literal value for it. It is
// substituted from the observed output into the expected literal at exactly
// one marked position (`with_generated_at`), so its POSITION and every
// surrounding byte stay pinned; the timestamp itself does not. Reaching it
// requires the injectable clock above.
//
// ## Why the fixture's clock runs in the year 2999
//
// One clock-derived value DOES appear inside a rendered column: `ps`'s
// `last_hb:`, which ages `last_heartbeat_at` against the host clock. Any
// past heartbeat renders an age that changes as the test sits, which is the
// flake task 6426 warns trains dismissal. `relative_time`'s comparison is
// SIGNED and its first bucket takes any future timestamp, so a fixture
// heartbeat in 2999 renders `just now` today and in a hundred years. That
// makes the column exactly pinnable at the cost of reaching only that one
// bucket here; the other four are pinned exactly by `format.t.cpp`'s own
// bucket cases, which is where bucket boundaries belong.
//
// Claim 6's `lease_expires_at` is frozen in the PAST instead, so
// `ps --stale`'s stale bucket has a row to render rather than a zero count,
// and `plans`' in-flight arithmetic has an expired lease to exclude.
//
// ## Why the timestamps are distinct rather than one constant
//
// `feed` sorts its events with `std::ranges::sort`, which is NOT stable. Ties
// on `at` therefore order arbitrarily — and at millisecond wall-clock
// precision the unfrozen fixture PRODUCED ties (a claim and its own first
// action land in the same millisecond). Every frozen timestamp below is
// distinct, so the sort has nothing to be unstable about.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;

#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::capture;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;

/// @brief Path to the built `planar-watch` binary — the subject.
/// @return The path.
auto watch_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the built `planar` operator binary — the seed's author.
/// @return The path.
auto operator_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_OPERATOR_CPP_BIN};
}

/// @brief Path to the built `planar-agent` binary — the claim ritual's.
/// @return The path.
auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_AGENT_CPP_BIN};
}

/// @brief Run one seed step, failing the case if it does not exit 0.
///
/// FAIL-LOUD, NOT BEST-EFFORT: a seed step that silently failed would leave
/// a short fixture, and every pin below would then diff against a plausible
/// but wrong expectation.
/// @param bin The binary to run.
/// @param args The argument tail.
/// @param root The arena root.
/// @param tag A discriminator so each step gets its own capture files.
/// @return The captured result.
auto seed_step(const std::filesystem::path& bin, std::vector<std::string> args, const std::filesystem::path& root,
               std::string_view tag) -> capture {
  auto const got = run_pinned(bin, args, root, tag);
  INFO("seed step " << tag << " stderr: " << got.err);
  REQUIRE(got.code == 0);
  return got;
}

/// @brief The `claim_token` out of a `--json` claim payload.
///
/// A deliberately crude scan rather than a JSON parse: the key appears once
/// in every payload it is used on, and the token is 32 hex characters with
/// nothing to escape.
/// @param payload The `--json` stdout.
/// @return The token.
auto token_from(std::string_view payload) -> std::string {
  constexpr std::string_view key = "\"claim_token\":\"";
  auto const                 at  = payload.find(key);
  REQUIRE(at != std::string_view::npos);
  auto const start = at + key.size();
  auto const end   = payload.find('"', start);
  REQUIRE(end != std::string_view::npos);
  return std::string{payload.substr(start, end - start)};
}

/// @brief Rewrite the fixture's volatile columns to fixed, distinct,
/// deterministic values of the same shape.
///
/// THIS IS NOT A NORMALIZER AND IT TOUCHES NO RENDERED TEXT. It runs against
/// the fixture BEFORE any read verb sees it, replaces values the fixture's
/// own creation minted at random or from the clock, and leaves every column
/// that carries meaning (status, vendor, role, entity, outcome, summary,
/// failure category, parent links, counts, ordering) exactly as the seed
/// steps wrote it. What the verbs render afterwards is pinned byte for byte,
/// nothing is blanked, and a rendering regression has nowhere to hide.
///
/// The seed itself still goes through the CLI, as the repository's test
/// conventions require, so the ROWS are correct by construction rather than
/// by assertion.
///
/// Values are derived from each row's `id`, which makes them distinct, keeps
/// the original insertion order intact (ids are monotonic), and gives `feed`
/// a total order to sort by. Claims land on the minute, their actions 100ms
/// later, so the merged timeline interleaves the way the real one did.
/// @param db_path The seeded database.
auto freeze_fixture(const std::filesystem::path& db_path) -> void {
  auto conn = planar::db::connection::open(db_path.string());
  REQUIRE(conn.has_value());
  auto const applied = conn->execute(R"SQL(
update agent_work_claims set
  claim_token       = printf('%032x', id),
  claimed_at        = printf('2999-03-14T09:%02d:00.100Z', id),
  last_heartbeat_at = printf('2999-03-14T09:%02d:00.100Z', id),
  lease_expires_at  = case when id = 6 then '2020-01-01T00:00:00.000Z'
                           else printf('2999-03-14T19:%02d:00.100Z', id) end,
  released_at       = case when released_at is null then null
                           else printf('2999-03-14T09:%02d:00.150Z', id) end,
  worktree_path     = case when worktree_path is null then null
                           else '/pinned/proj/a-very-long-worktree-path-well-over-forty-characters/agent-42' end,
  repo_root         = case when repo_root is null then null else '/pinned/proj' end;
update agent_actions set
  started_at = printf('2999-03-14T09:%02d:00.200Z', id),
  ended_at   = case when ended_at is null then null else printf('2999-03-14T09:%02d:00.300Z', id) end;
update plans set
  created_at = printf('2999-03-14T08:%02d:00.000Z', id),
  updated_at = printf('2999-03-14T08:%02d:00.500Z', id);
)SQL");
  INFO("freeze failed: " << (applied.has_value() ? std::string{} : applied.error().message_));
  REQUIRE(applied.has_value());
}

/// @brief The marker standing in for `generated_at` in the four expectations
/// that carry one.
constexpr std::string_view k_generated_at_marker = "@@GENERATED_AT@@";

/// @brief Splice the OBSERVED `generated_at` into `expected` at the single
/// marked position.
///
/// The one value in this file's subject that comes from the host clock at
/// emit time rather than from a row (see the file header). Everything around
/// it stays a literal: the opening `{"generated_at":"` is required exactly,
/// the value runs to the next quote, and the marker occurs once.
/// @param expected The expectation carrying the marker.
/// @param observed The verb's actual stdout.
/// @return `expected` with the marker replaced by the observed timestamp.
auto with_generated_at(std::string_view expected, std::string_view observed) -> std::string {
  constexpr std::string_view opening = "{\"generated_at\":\"";
  REQUIRE(observed.starts_with(opening));
  auto const end = observed.find('"', opening.size());
  REQUIRE(end != std::string_view::npos);
  auto const value = observed.substr(opening.size(), end - opening.size());

  auto const at = expected.find(k_generated_at_marker);
  REQUIRE(at != std::string_view::npos);
  REQUIRE(expected.find(k_generated_at_marker, at + 1) == std::string_view::npos);

  std::string out{expected};
  out.replace(at, k_generated_at_marker.size(), value);
  return out;
}

/// @brief One pinned invocation.
struct pin {
  std::string_view         tag;      ///< Discriminator, and the capture file's name.
  std::vector<std::string> args;     ///< The argv tail.
  std::string_view         expected; ///< The exact stdout, marker included.
};

/// @brief Pinned stdout for `planar-watch feed`.
constexpr std::string_view k_feed = R"PIN(  2999-03-14T09:01:00.100Z  claim_acquired
  2999-03-14T09:01:00.200Z  action_started
  2999-03-14T09:02:00.100Z  claim_acquired
  2999-03-14T09:02:00.200Z  action_started
  2999-03-14T09:03:00.100Z  claim_acquired
  2999-03-14T09:03:00.200Z  action_started
  2999-03-14T09:04:00.100Z  claim_acquired
  2999-03-14T09:04:00.200Z  action_started
  2999-03-14T09:05:00.100Z  claim_acquired
  2999-03-14T09:05:00.200Z  action_started
  2999-03-14T09:06:00.100Z  claim_acquired
  2999-03-14T09:06:00.200Z  action_started
  2999-03-14T09:07:00.100Z  claim_acquired
  2999-03-14T09:07:00.150Z  aborted
  2999-03-14T09:07:00.200Z  action_started
  2999-03-14T09:08:00.200Z  action_started
  2999-03-14T09:08:00.300Z  action_ended
  2999-03-14T09:09:00.200Z  action_started
  2999-03-14T09:09:00.300Z  action_ended
  2999-03-14T09:10:00.200Z  action_started
  2999-03-14T09:10:00.300Z  action_ended
)PIN";

/// @brief Pinned stdout for `planar-watch feed --json`.
constexpr std::string_view k_feedj =
    R"PIN({"event":"claim_acquired","at":"2999-03-14T09:01:00.100Z","claim":{"id":1,"claim_token":"00000000000000000000000000000001","session_id":1,"entity_kind":"task","entity_id":1,"claim_scope":"exclusive","status":"active","vendor":"claude","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":"/pinned/proj/a-very-long-worktree-path-well-over-forty-characters/agent-42","repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:01:00.100Z","last_heartbeat_at":"2999-03-14T09:01:00.100Z","lease_expires_at":"2999-03-14T19:01:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null}}
{"event":"action_started","at":"2999-03-14T09:01:00.200Z","action":{"id":1,"session_id":1,"session_entry_id":null,"parent_action_id":null,"claim_id":1,"action_kind":"coder","entity_kind":"task","entity_id":1,"vendor":"claude","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:01:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"claim_acquired","at":"2999-03-14T09:02:00.100Z","claim":{"id":2,"claim_token":"00000000000000000000000000000002","session_id":2,"entity_kind":"task","entity_id":2,"claim_scope":"exclusive","status":"active","vendor":"codex","vendor_session_id":null,"role":"reviewer","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:02:00.100Z","last_heartbeat_at":"2999-03-14T09:02:00.100Z","lease_expires_at":"2999-03-14T19:02:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null}}
{"event":"action_started","at":"2999-03-14T09:02:00.200Z","action":{"id":2,"session_id":2,"session_entry_id":null,"parent_action_id":1,"claim_id":2,"action_kind":"reviewer","entity_kind":"task","entity_id":2,"vendor":"codex","vendor_role":"reviewer","model":null,"started_at":"2999-03-14T09:02:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"claim_acquired","at":"2999-03-14T09:03:00.100Z","claim":{"id":3,"claim_token":"00000000000000000000000000000003","session_id":3,"entity_kind":"task","entity_id":3,"claim_scope":"exclusive","status":"active","vendor":"copilot","vendor_session_id":null,"role":"test_coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:03:00.100Z","last_heartbeat_at":"2999-03-14T09:03:00.100Z","lease_expires_at":"2999-03-14T19:03:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null}}
{"event":"action_started","at":"2999-03-14T09:03:00.200Z","action":{"id":3,"session_id":3,"session_entry_id":null,"parent_action_id":2,"claim_id":3,"action_kind":"test_coder","entity_kind":"task","entity_id":3,"vendor":"copilot","vendor_role":"test_coder","model":null,"started_at":"2999-03-14T09:03:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"claim_acquired","at":"2999-03-14T09:04:00.100Z","claim":{"id":4,"claim_token":"00000000000000000000000000000004","session_id":1,"entity_kind":"task","entity_id":4,"claim_scope":"exclusive","status":"active","vendor":"claude","vendor_session_id":null,"role":null,"model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:04:00.100Z","last_heartbeat_at":"2999-03-14T09:04:00.100Z","lease_expires_at":"2999-03-14T19:04:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null}}
{"event":"action_started","at":"2999-03-14T09:04:00.200Z","action":{"id":4,"session_id":1,"session_entry_id":null,"parent_action_id":1,"claim_id":4,"action_kind":"coder","entity_kind":"task","entity_id":4,"vendor":"claude","vendor_role":null,"model":null,"started_at":"2999-03-14T09:04:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"claim_acquired","at":"2999-03-14T09:05:00.100Z","claim":{"id":5,"claim_token":"00000000000000000000000000000005","session_id":2,"entity_kind":"task","entity_id":5,"claim_scope":"exclusive","status":"active","vendor":"codex","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:05:00.100Z","last_heartbeat_at":"2999-03-14T09:05:00.100Z","lease_expires_at":"2999-03-14T19:05:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null}}
{"event":"action_started","at":"2999-03-14T09:05:00.200Z","action":{"id":5,"session_id":2,"session_entry_id":null,"parent_action_id":3,"claim_id":5,"action_kind":"coder","entity_kind":"task","entity_id":5,"vendor":"codex","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:05:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"claim_acquired","at":"2999-03-14T09:06:00.100Z","claim":{"id":6,"claim_token":"00000000000000000000000000000006","session_id":3,"entity_kind":"task","entity_id":6,"claim_scope":"exclusive","status":"active","vendor":"copilot","vendor_session_id":null,"role":"reviewer","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:06:00.100Z","last_heartbeat_at":"2999-03-14T09:06:00.100Z","lease_expires_at":"2020-01-01T00:00:00.000Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null}}
{"event":"action_started","at":"2999-03-14T09:06:00.200Z","action":{"id":6,"session_id":3,"session_entry_id":null,"parent_action_id":2,"claim_id":6,"action_kind":"reviewer","entity_kind":"task","entity_id":6,"vendor":"copilot","vendor_role":"reviewer","model":null,"started_at":"2999-03-14T09:06:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"claim_acquired","at":"2999-03-14T09:07:00.100Z","claim":{"id":7,"claim_token":"00000000000000000000000000000007","session_id":2,"entity_kind":"task","entity_id":7,"claim_scope":"exclusive","status":"aborted","vendor":"codex","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:07:00.100Z","last_heartbeat_at":"2999-03-14T09:07:00.100Z","lease_expires_at":"2999-03-14T19:07:00.100Z","released_at":"2999-03-14T09:07:00.150Z","release_reason":"gate failed","failure_category":"tool_failure","run_id":null,"stage":null}}
{"event":"aborted","at":"2999-03-14T09:07:00.150Z","claim":{"id":7,"claim_token":"00000000000000000000000000000007","session_id":2,"entity_kind":"task","entity_id":7,"claim_scope":"exclusive","status":"aborted","vendor":"codex","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:07:00.100Z","last_heartbeat_at":"2999-03-14T09:07:00.100Z","lease_expires_at":"2999-03-14T19:07:00.100Z","released_at":"2999-03-14T09:07:00.150Z","release_reason":"gate failed","failure_category":"tool_failure","run_id":null,"stage":null}}
{"event":"action_started","at":"2999-03-14T09:07:00.200Z","action":{"id":7,"session_id":3,"session_entry_id":null,"parent_action_id":6,"claim_id":6,"action_kind":"heartbeat","entity_kind":null,"entity_id":null,"vendor":"copilot","vendor_role":null,"model":null,"started_at":"2999-03-14T09:07:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"action_started","at":"2999-03-14T09:08:00.200Z","action":{"id":8,"session_id":2,"session_entry_id":null,"parent_action_id":null,"claim_id":7,"action_kind":"coder","entity_kind":"task","entity_id":7,"vendor":"codex","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:08:00.200Z","ended_at":"2999-03-14T09:08:00.300Z","outcome":"error","summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"action_ended","at":"2999-03-14T09:08:00.300Z","action":{"id":8,"session_id":2,"session_entry_id":null,"parent_action_id":null,"claim_id":7,"action_kind":"coder","entity_kind":"task","entity_id":7,"vendor":"codex","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:08:00.200Z","ended_at":"2999-03-14T09:08:00.300Z","outcome":"error","summary":null,"head_sha":null,"dirty":null,"metadata":null}}
{"event":"action_started","at":"2999-03-14T09:09:00.200Z","action":{"id":9,"session_id":1,"session_entry_id":null,"parent_action_id":1,"claim_id":1,"action_kind":"tool_call","entity_kind":"task","entity_id":1,"vendor":"claude","vendor_role":null,"model":null,"started_at":"2999-03-14T09:09:00.200Z","ended_at":"2999-03-14T09:09:00.300Z","outcome":"ok","summary":"the coordination layer rewired every caller and then some more words ——— tail padding to push past eighty bytes","head_sha":null,"dirty":null,"metadata":null}}
{"event":"action_ended","at":"2999-03-14T09:09:00.300Z","action":{"id":9,"session_id":1,"session_entry_id":null,"parent_action_id":1,"claim_id":1,"action_kind":"tool_call","entity_kind":"task","entity_id":1,"vendor":"claude","vendor_role":null,"model":null,"started_at":"2999-03-14T09:09:00.200Z","ended_at":"2999-03-14T09:09:00.300Z","outcome":"ok","summary":"the coordination layer rewired every caller and then some more words ——— tail padding to push past eighty bytes","head_sha":null,"dirty":null,"metadata":null}}
{"event":"action_started","at":"2999-03-14T09:10:00.200Z","action":{"id":10,"session_id":2,"session_entry_id":null,"parent_action_id":5,"claim_id":5,"action_kind":"tool_call","entity_kind":"task","entity_id":5,"vendor":"codex","vendor_role":null,"model":null,"started_at":"2999-03-14T09:10:00.200Z","ended_at":"2999-03-14T09:10:00.300Z","outcome":"ok","summary":"a plain ascii summary with no multibyte characters anywhere in it so the seventy-seventh byte is an ordinary letter and the cut width shows","head_sha":null,"dirty":null,"metadata":null}}
{"event":"action_ended","at":"2999-03-14T09:10:00.300Z","action":{"id":10,"session_id":2,"session_entry_id":null,"parent_action_id":5,"claim_id":5,"action_kind":"tool_call","entity_kind":"task","entity_id":5,"vendor":"codex","vendor_role":null,"model":null,"started_at":"2999-03-14T09:10:00.200Z","ended_at":"2999-03-14T09:10:00.300Z","outcome":"ok","summary":"a plain ascii summary with no multibyte characters anywhere in it so the seventy-seventh byte is an ordinary letter and the cut width shows","head_sha":null,"dirty":null,"metadata":null}}
)PIN";

/// @brief Pinned stdout for `planar-watch ps`.
constexpr std::string_view k_ps = R"PIN(active: 6
  task:6  scope:project:proj  activity:""  vendor:copilot  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000006
  task:5  scope:project:proj  activity:"a plain ascii summary with no multibyte characters anywhere in it so the seve…"  vendor:codex  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000005
  task:4  scope:project:proj  activity:""  vendor:claude  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000004
  task:3  scope:project:proj  activity:""  vendor:copilot  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000003
  task:2  scope:project:proj  activity:""  vendor:codex  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000002
  task:1  scope:project:proj  activity:"the coordination layer rewired every caller and then some more words ——…"  vendor:claude  branch:?  worktree:…agent-42  sha:?  last_hb:just now  token:00000000000000000000000000000001
)PIN";

/// @brief Pinned stdout for `planar-watch ps --json`.
constexpr std::string_view k_psj =
    R"PIN({"generated_at":"@@GENERATED_AT@@","active":[{"id":6,"claim_token":"00000000000000000000000000000006","session_id":3,"entity_kind":"task","entity_id":6,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"copilot","vendor_session_id":null,"role":"reviewer","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:06:00.100Z","last_heartbeat_at":"2999-03-14T09:06:00.100Z","lease_expires_at":"2020-01-01T00:00:00.000Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null,"latest_action":{"kind":"heartbeat","summary":null,"started_at":"2999-03-14T09:07:00.200Z"}},{"id":5,"claim_token":"00000000000000000000000000000005","session_id":2,"entity_kind":"task","entity_id":5,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"codex","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:05:00.100Z","last_heartbeat_at":"2999-03-14T09:05:00.100Z","lease_expires_at":"2999-03-14T19:05:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null,"latest_action":{"kind":"tool_call","summary":"a plain ascii summary with no multibyte characters anywhere in it so the seventy-seventh byte is an ordinary letter and the cut width shows","started_at":"2999-03-14T09:10:00.200Z"}},{"id":4,"claim_token":"00000000000000000000000000000004","session_id":1,"entity_kind":"task","entity_id":4,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"claude","vendor_session_id":null,"role":null,"model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:04:00.100Z","last_heartbeat_at":"2999-03-14T09:04:00.100Z","lease_expires_at":"2999-03-14T19:04:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null,"latest_action":{"kind":"coder","summary":null,"started_at":"2999-03-14T09:04:00.200Z"}},{"id":3,"claim_token":"00000000000000000000000000000003","session_id":3,"entity_kind":"task","entity_id":3,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"copilot","vendor_session_id":null,"role":"test_coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:03:00.100Z","last_heartbeat_at":"2999-03-14T09:03:00.100Z","lease_expires_at":"2999-03-14T19:03:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null,"latest_action":{"kind":"test_coder","summary":null,"started_at":"2999-03-14T09:03:00.200Z"}},{"id":2,"claim_token":"00000000000000000000000000000002","session_id":2,"entity_kind":"task","entity_id":2,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"codex","vendor_session_id":null,"role":"reviewer","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:02:00.100Z","last_heartbeat_at":"2999-03-14T09:02:00.100Z","lease_expires_at":"2999-03-14T19:02:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null,"latest_action":{"kind":"reviewer","summary":null,"started_at":"2999-03-14T09:02:00.200Z"}},{"id":1,"claim_token":"00000000000000000000000000000001","session_id":1,"entity_kind":"task","entity_id":1,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"claude","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":"/pinned/proj/a-very-long-worktree-path-well-over-forty-characters/agent-42","repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:01:00.100Z","last_heartbeat_at":"2999-03-14T09:01:00.100Z","lease_expires_at":"2999-03-14T19:01:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null,"latest_action":{"kind":"tool_call","summary":"the coordination layer rewired every caller and then some more words ——— tail padding to push past eighty bytes","started_at":"2999-03-14T09:09:00.200Z"}}],"stale":[]}
)PIN";

/// @brief Pinned stdout for `planar-watch ps --stale`.
constexpr std::string_view k_psstale = R"PIN(active: 6
  task:6  scope:project:proj  activity:""  vendor:copilot  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000006
  task:5  scope:project:proj  activity:"a plain ascii summary with no multibyte characters anywhere in it so the seve…"  vendor:codex  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000005
  task:4  scope:project:proj  activity:""  vendor:claude  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000004
  task:3  scope:project:proj  activity:""  vendor:copilot  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000003
  task:2  scope:project:proj  activity:""  vendor:codex  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000002
  task:1  scope:project:proj  activity:"the coordination layer rewired every caller and then some more words ——…"  vendor:claude  branch:?  worktree:…agent-42  sha:?  last_hb:just now  token:00000000000000000000000000000001
stale: 1
  task:6  scope:project:proj  activity:""  vendor:copilot  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000006
)PIN";

/// @brief Pinned stdout for `planar-watch ps --group-by role`.
constexpr std::string_view k_psrole = R"PIN([group: reviewer]
  task:6  scope:project:proj  activity:""  vendor:copilot  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000006
  task:2  scope:project:proj  activity:""  vendor:codex  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000002
[group: coder]
  task:5  scope:project:proj  activity:"a plain ascii summary with no multibyte characters anywhere in it so the seve…"  vendor:codex  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000005
  task:1  scope:project:proj  activity:"the coordination layer rewired every caller and then some more words ——…"  vendor:claude  branch:?  worktree:…agent-42  sha:?  last_hb:just now  token:00000000000000000000000000000001
[group: unknown]
  task:4  scope:project:proj  activity:""  vendor:claude  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000004
[group: test_coder]
  task:3  scope:project:proj  activity:""  vendor:copilot  branch:?  worktree:""  sha:?  last_hb:just now  token:00000000000000000000000000000003
)PIN";

/// @brief Pinned stdout for `planar-watch claims`.
constexpr std::string_view k_claims = R"PIN(claims: 5
  task:5  scope:project:proj  status:active  vendor:codex  token:00000000000000000000000000000005
  task:4  scope:project:proj  status:active  vendor:claude  token:00000000000000000000000000000004
  task:3  scope:project:proj  status:active  vendor:copilot  token:00000000000000000000000000000003
  task:2  scope:project:proj  status:active  vendor:codex  token:00000000000000000000000000000002
  task:1  scope:project:proj  status:active  vendor:claude  token:00000000000000000000000000000001
)PIN";

/// @brief Pinned stdout for `planar-watch claims --json`.
constexpr std::string_view k_claimsj =
    R"PIN({"generated_at":"@@GENERATED_AT@@","claims":[{"id":5,"claim_token":"00000000000000000000000000000005","session_id":2,"entity_kind":"task","entity_id":5,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"codex","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:05:00.100Z","last_heartbeat_at":"2999-03-14T09:05:00.100Z","lease_expires_at":"2999-03-14T19:05:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null},{"id":4,"claim_token":"00000000000000000000000000000004","session_id":1,"entity_kind":"task","entity_id":4,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"claude","vendor_session_id":null,"role":null,"model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:04:00.100Z","last_heartbeat_at":"2999-03-14T09:04:00.100Z","lease_expires_at":"2999-03-14T19:04:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null},{"id":3,"claim_token":"00000000000000000000000000000003","session_id":3,"entity_kind":"task","entity_id":3,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"copilot","vendor_session_id":null,"role":"test_coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:03:00.100Z","last_heartbeat_at":"2999-03-14T09:03:00.100Z","lease_expires_at":"2999-03-14T19:03:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null},{"id":2,"claim_token":"00000000000000000000000000000002","session_id":2,"entity_kind":"task","entity_id":2,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"codex","vendor_session_id":null,"role":"reviewer","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:02:00.100Z","last_heartbeat_at":"2999-03-14T09:02:00.100Z","lease_expires_at":"2999-03-14T19:02:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null},{"id":1,"claim_token":"00000000000000000000000000000001","session_id":1,"entity_kind":"task","entity_id":1,"entity_scope":{"kind":"association","slug":"project:proj"},"claim_scope":"exclusive","status":"active","vendor":"claude","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":"/pinned/proj/a-very-long-worktree-path-well-over-forty-characters/agent-42","repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:01:00.100Z","last_heartbeat_at":"2999-03-14T09:01:00.100Z","lease_expires_at":"2999-03-14T19:01:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null}]}
)PIN";

/// @brief Pinned stdout for `planar-watch claims --status all`.
constexpr std::string_view k_claimsall = R"PIN(claims: 7
  task:7  scope:project:proj  status:aborted  vendor:codex  category:tool_failure  token:00000000000000000000000000000007
  task:6  scope:project:proj  status:active  vendor:copilot  token:00000000000000000000000000000006
  task:5  scope:project:proj  status:active  vendor:codex  token:00000000000000000000000000000005
  task:4  scope:project:proj  status:active  vendor:claude  token:00000000000000000000000000000004
  task:3  scope:project:proj  status:active  vendor:copilot  token:00000000000000000000000000000003
  task:2  scope:project:proj  status:active  vendor:codex  token:00000000000000000000000000000002
  task:1  scope:project:proj  status:active  vendor:claude  token:00000000000000000000000000000001
)PIN";

/// @brief Pinned stdout for `planar-watch actions`.
constexpr std::string_view k_actions = R"PIN(actions: 10
  action:10  kind:tool_call  vendor:codex  entity:task:5  started:2999-03-14T09:10:00.200Z
  action:9  kind:tool_call  vendor:claude  entity:task:1  started:2999-03-14T09:09:00.200Z
  action:8  kind:coder  vendor:codex  entity:task:7  started:2999-03-14T09:08:00.200Z
  action:7  kind:heartbeat  vendor:copilot  entity:-:0  started:2999-03-14T09:07:00.200Z
  action:6  kind:reviewer  vendor:copilot  entity:task:6  started:2999-03-14T09:06:00.200Z
  action:5  kind:coder  vendor:codex  entity:task:5  started:2999-03-14T09:05:00.200Z
  action:4  kind:coder  vendor:claude  entity:task:4  started:2999-03-14T09:04:00.200Z
  action:3  kind:test_coder  vendor:copilot  entity:task:3  started:2999-03-14T09:03:00.200Z
  action:2  kind:reviewer  vendor:codex  entity:task:2  started:2999-03-14T09:02:00.200Z
  action:1  kind:coder  vendor:claude  entity:task:1  started:2999-03-14T09:01:00.200Z
)PIN";

/// @brief Pinned stdout for `planar-watch actions --json`.
constexpr std::string_view k_actionsj =
    R"PIN({"generated_at":"@@GENERATED_AT@@","actions":[{"id":10,"session_id":2,"session_entry_id":null,"parent_action_id":5,"claim_id":5,"action_kind":"tool_call","entity_kind":"task","entity_id":5,"vendor":"codex","vendor_role":null,"model":null,"started_at":"2999-03-14T09:10:00.200Z","ended_at":"2999-03-14T09:10:00.300Z","outcome":"ok","summary":"a plain ascii summary with no multibyte characters anywhere in it so the seventy-seventh byte is an ordinary letter and the cut width shows","head_sha":null,"dirty":null,"metadata":null},{"id":9,"session_id":1,"session_entry_id":null,"parent_action_id":1,"claim_id":1,"action_kind":"tool_call","entity_kind":"task","entity_id":1,"vendor":"claude","vendor_role":null,"model":null,"started_at":"2999-03-14T09:09:00.200Z","ended_at":"2999-03-14T09:09:00.300Z","outcome":"ok","summary":"the coordination layer rewired every caller and then some more words ——— tail padding to push past eighty bytes","head_sha":null,"dirty":null,"metadata":null},{"id":8,"session_id":2,"session_entry_id":null,"parent_action_id":null,"claim_id":7,"action_kind":"coder","entity_kind":"task","entity_id":7,"vendor":"codex","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:08:00.200Z","ended_at":"2999-03-14T09:08:00.300Z","outcome":"error","summary":null,"head_sha":null,"dirty":null,"metadata":null},{"id":7,"session_id":3,"session_entry_id":null,"parent_action_id":6,"claim_id":6,"action_kind":"heartbeat","entity_kind":null,"entity_id":null,"vendor":"copilot","vendor_role":null,"model":null,"started_at":"2999-03-14T09:07:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null},{"id":6,"session_id":3,"session_entry_id":null,"parent_action_id":2,"claim_id":6,"action_kind":"reviewer","entity_kind":"task","entity_id":6,"vendor":"copilot","vendor_role":"reviewer","model":null,"started_at":"2999-03-14T09:06:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null},{"id":5,"session_id":2,"session_entry_id":null,"parent_action_id":3,"claim_id":5,"action_kind":"coder","entity_kind":"task","entity_id":5,"vendor":"codex","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:05:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null},{"id":4,"session_id":1,"session_entry_id":null,"parent_action_id":1,"claim_id":4,"action_kind":"coder","entity_kind":"task","entity_id":4,"vendor":"claude","vendor_role":null,"model":null,"started_at":"2999-03-14T09:04:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null},{"id":3,"session_id":3,"session_entry_id":null,"parent_action_id":2,"claim_id":3,"action_kind":"test_coder","entity_kind":"task","entity_id":3,"vendor":"copilot","vendor_role":"test_coder","model":null,"started_at":"2999-03-14T09:03:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null},{"id":2,"session_id":2,"session_entry_id":null,"parent_action_id":1,"claim_id":2,"action_kind":"reviewer","entity_kind":"task","entity_id":2,"vendor":"codex","vendor_role":"reviewer","model":null,"started_at":"2999-03-14T09:02:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null},{"id":1,"session_id":1,"session_entry_id":null,"parent_action_id":null,"claim_id":1,"action_kind":"coder","entity_kind":"task","entity_id":1,"vendor":"claude","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:01:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}]}
)PIN";

/// @brief Pinned stdout for `planar-watch plans`.
constexpr std::string_view k_plans = R"PIN(plans: 2
  plan:1  [active]  in_flight:yes  claims:5  actions:6  Demo plan
  plan:2  [draft]  in_flight:no  claims:0  actions:0  Idle plan
)PIN";

/// @brief Pinned stdout for `planar-watch plans --json`.
constexpr std::string_view k_plansj =
    R"PIN({"generated_at":"@@GENERATED_AT@@","plans":[{"plan":{"id":1,"scope_kind":"association","scope_id":1,"title":"Demo plan","slug":"demo-plan","summary":null,"status":"active","parent_plan_id":null,"created_at":"2999-03-14T08:01:00.000Z","updated_at":"2999-03-14T08:01:00.500Z"},"in_flight":true,"active_claims":5,"active_actions":6,"last_event_at":"2999-03-14T09:10:00.300Z"},{"plan":{"id":2,"scope_kind":"association","scope_id":1,"title":"Idle plan","slug":"idle-plan","summary":null,"status":"draft","parent_plan_id":null,"created_at":"2999-03-14T08:02:00.000Z","updated_at":"2999-03-14T08:02:00.500Z"},"in_flight":false,"active_claims":0,"active_actions":0,"last_event_at":null}]}
)PIN";

/// @brief Pinned stdout for `planar-watch log --task 1`.
constexpr std::string_view k_log1 = R"PIN(entries: 2 actions + 1 claim rows
  claim:00000000000000000000000000000001  2999-03-14T09:01:00.100Z  vendor:claude  status:active
  action:1  2999-03-14T09:01:00.200Z  kind:coder  vendor:claude
  action:9  2999-03-14T09:09:00.200Z  kind:tool_call  vendor:claude
)PIN";

/// @brief Pinned stdout for `planar-watch log --task 1 --json`.
constexpr std::string_view k_log1j =
    R"PIN({"entity":{"kind":"task","id":1},"entries":[{"kind":"claim_acquired","at":"2999-03-14T09:01:00.100Z","claim":{"id":1,"claim_token":"00000000000000000000000000000001","session_id":1,"entity_kind":"task","entity_id":1,"claim_scope":"exclusive","status":"active","vendor":"claude","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":"/pinned/proj/a-very-long-worktree-path-well-over-forty-characters/agent-42","repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:01:00.100Z","last_heartbeat_at":"2999-03-14T09:01:00.100Z","lease_expires_at":"2999-03-14T19:01:00.100Z","released_at":null,"release_reason":null,"failure_category":null,"run_id":null,"stage":null}},{"kind":"action","at":"2999-03-14T09:01:00.200Z","action":{"id":1,"session_id":1,"session_entry_id":null,"parent_action_id":null,"claim_id":1,"action_kind":"coder","entity_kind":"task","entity_id":1,"vendor":"claude","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:01:00.200Z","ended_at":null,"outcome":null,"summary":null,"head_sha":null,"dirty":null,"metadata":null}},{"kind":"action","at":"2999-03-14T09:09:00.200Z","action":{"id":9,"session_id":1,"session_entry_id":null,"parent_action_id":1,"claim_id":1,"action_kind":"tool_call","entity_kind":"task","entity_id":1,"vendor":"claude","vendor_role":null,"model":null,"started_at":"2999-03-14T09:09:00.200Z","ended_at":"2999-03-14T09:09:00.300Z","outcome":"ok","summary":"the coordination layer rewired every caller and then some more words ——— tail padding to push past eighty bytes","head_sha":null,"dirty":null,"metadata":null}}]}
)PIN";

/// @brief Pinned stdout for `planar-watch log --task 7 --json`.
constexpr std::string_view k_log7j =
    R"PIN({"entity":{"kind":"task","id":7},"entries":[{"kind":"claim_acquired","at":"2999-03-14T09:07:00.100Z","claim":{"id":7,"claim_token":"00000000000000000000000000000007","session_id":2,"entity_kind":"task","entity_id":7,"claim_scope":"exclusive","status":"aborted","vendor":"codex","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:07:00.100Z","last_heartbeat_at":"2999-03-14T09:07:00.100Z","lease_expires_at":"2999-03-14T19:07:00.100Z","released_at":"2999-03-14T09:07:00.150Z","release_reason":"gate failed","failure_category":"tool_failure","run_id":null,"stage":null}},{"kind":"claim_aborted","at":"2999-03-14T09:07:00.150Z","claim":{"id":7,"claim_token":"00000000000000000000000000000007","session_id":2,"entity_kind":"task","entity_id":7,"claim_scope":"exclusive","status":"aborted","vendor":"codex","vendor_session_id":null,"role":"coder","model":null,"worktree_id":null,"worktree_path":null,"repo_root":"/pinned/proj","branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown","purpose":null,"base_ref":null,"claimed_at":"2999-03-14T09:07:00.100Z","last_heartbeat_at":"2999-03-14T09:07:00.100Z","lease_expires_at":"2999-03-14T19:07:00.100Z","released_at":"2999-03-14T09:07:00.150Z","release_reason":"gate failed","failure_category":"tool_failure","run_id":null,"stage":null}},{"kind":"action","at":"2999-03-14T09:08:00.200Z","action":{"id":8,"session_id":2,"session_entry_id":null,"parent_action_id":null,"claim_id":7,"action_kind":"coder","entity_kind":"task","entity_id":7,"vendor":"codex","vendor_role":"coder","model":null,"started_at":"2999-03-14T09:08:00.200Z","ended_at":"2999-03-14T09:08:00.300Z","outcome":"error","summary":null,"head_sha":null,"dirty":null,"metadata":null}}]}
)PIN";

/// @brief Every pinned invocation, in the order they run.
const std::vector<pin> k_pins{
    pin{.tag = "feed", .args = {"feed"}, .expected = k_feed},
    pin{.tag = "feedj", .args = {"feed", "--json"}, .expected = k_feedj},
    pin{.tag = "ps", .args = {"ps"}, .expected = k_ps},
    pin{.tag = "psj", .args = {"ps", "--json"}, .expected = k_psj},
    pin{.tag = "psstale", .args = {"ps", "--stale"}, .expected = k_psstale},
    pin{.tag = "psrole", .args = {"ps", "--group-by", "role"}, .expected = k_psrole},
    pin{.tag = "claims", .args = {"claims"}, .expected = k_claims},
    pin{.tag = "claimsj", .args = {"claims", "--json"}, .expected = k_claimsj},
    pin{.tag = "claimsall", .args = {"claims", "--status", "all"}, .expected = k_claimsall},
    pin{.tag = "actions", .args = {"actions"}, .expected = k_actions},
    pin{.tag = "actionsj", .args = {"actions", "--json"}, .expected = k_actionsj},
    pin{.tag = "plans", .args = {"plans"}, .expected = k_plans},
    pin{.tag = "plansj", .args = {"plans", "--json"}, .expected = k_plansj},
    pin{.tag = "log1", .args = {"log", "--task", "1"}, .expected = k_log1},
    pin{.tag = "log1j", .args = {"log", "--task", "1", "--json"}, .expected = k_log1j},
    pin{.tag = "log7j", .args = {"log", "--task", "7", "--json"}, .expected = k_log7j},
};

} // namespace

TEST_CASE("planar-watch: the six read verbs render a frozen fixture byte for byte", "[cmd][watch][readpin]") {
  auto const space = make_arena("watchpin");
  auto const root  = space.cpp_root;

  // --- seed, through the CLI -----------------------------------------------
  //
  // The shape is the deleted case's, kept deliberately: every row below was
  // chosen to reach a rendering path rather than to pad the fixture.
  seed_step(operator_bin(), {"init"}, root, "s00");
  seed_step(operator_bin(), {"assoc", "create", "project:proj", "--kind", "project"}, root, "s01");
  seed_step(operator_bin(), {"assoc", "add", "project:proj", (root / "proj").string()}, root, "s02");
  seed_step(operator_bin(), {"plan", "create", "Demo plan"}, root, "s03");
  for (int i = 1; i <= 8; ++i) {
    seed_step(operator_bin(), {"task", "add", std::format("Task {}", i), "--plan", "1"}, root, std::format("s1{}", i));
  }
  seed_step(operator_bin(), {"plan", "update", "1", "--status", "active"}, root, "s20");
  // A second plan with NO agent activity, so `plans` renders both the
  // in-flight and the idle line.
  seed_step(operator_bin(), {"plan", "create", "Idle plan"}, root, "s21");

  // A worktree path well over forty characters, so `ps`' worktree column
  // renders the ELIDED form (`…agent-42`) rather than a bare basename.
  auto const      worktree = root / "proj" / "a-very-long-worktree-path-well-over-forty-characters" / "agent-42";
  std::error_code ec;
  std::filesystem::create_directories(worktree, ec);

  auto const first =
      seed_step(agent_bin(), {"pull", "1", "--vendor", "claude", "--role", "coder", "--worktree", worktree.string(), "--json"},
                root, "c01");
  auto const first_token = token_from(first.out);
  seed_step(agent_bin(), {"pull", "1", "--vendor", "codex", "--role", "reviewer", "--parent-action", "1", "--json"}, root, "c02");
  seed_step(agent_bin(), {"pull", "1", "--vendor", "copilot", "--role", "test_coder", "--parent-action", "2", "--json"}, root,
            "c03");
  // No `--role` at all, so `ps --group-by role` gets its `unknown` bucket.
  seed_step(agent_bin(), {"pull", "1", "--vendor", "claude", "--parent-action", "1", "--json"}, root, "c04");
  auto const ascii = seed_step(
      agent_bin(), {"pull", "1", "--vendor", "codex", "--role", "coder", "--parent-action", "3", "--json"}, root, "c05");
  auto const ascii_token = token_from(ascii.out);
  auto const sibling     = seed_step(
      agent_bin(), {"pull", "1", "--vendor", "copilot", "--role", "reviewer", "--parent-action", "2", "--json"}, root, "c06");
  auto const sibling_token = token_from(sibling.out);
  // An action with NO `--entity`, so `actions`' text arm renders its null
  // entity — `entity:-:0`, a literal dash and a zero standing in for columns
  // that do not exist.
  seed_step(agent_bin(), {"action", "start", "--claim", sibling_token, "--kind", "heartbeat", "--json"}, root, "c07");
  // A terminal claim carrying a closed failure category, which is the only
  // way the `category:` column and `log`'s `claim_aborted` entry render.
  auto const failed       = seed_step(agent_bin(), {"pull", "1", "--vendor", "codex", "--role", "coder", "--json"}, root, "c08");
  auto const failed_token = token_from(failed.out);
  seed_step(agent_bin(), {"fail", "--claim", failed_token, "--reason", "gate failed", "--category", "tool_failure"}, root, "c09");
  // A summary well over the 80-byte activity limit whose 77TH BYTE LANDS
  // INSIDE A MULTI-BYTE CHARACTER (0x94, the third byte of the third em
  // dash), so `ps`' activity column truncates on a real UTF-8 boundary.
  seed_step(agent_bin(), {"action", "start", "--claim", first_token, "--kind", "tool_call", "--entity", "task:1", "--json"}, root,
            "c10");
  seed_step(agent_bin(),
            // Action 9: six pulls (1-6), the entity-less `action start` (7),
            // the failed pull (8), and the `action start` immediately above.
            {"action", "end", "--action", "9", "--outcome", "ok", "--summary",
             "the coordination layer rewired every caller and then some more words "
             "\xe2\x80\x94\xe2\x80\x94\xe2\x80\x94 tail padding to push past eighty bytes"},
            root, "c11");
  // A SECOND over-length summary, this one PURE ASCII, so the activity
  // column's 77-byte cut is pinned on its own. Without it the cut width is
  // NOT independently observable: the multi-byte summary above has an em
  // dash straddling byte 77, and the UTF-8 walk-back lands a 76-byte and a
  // 77-byte cut on the SAME character boundary — measured, as an INERT
  // break-probe, before this row existed.
  seed_step(agent_bin(), {"action", "start", "--claim", ascii_token, "--kind", "tool_call", "--entity", "task:5", "--json"}, root,
            "c12");
  seed_step(agent_bin(),
            // Action 10: the nine above plus this one.
            {"action", "end", "--action", "10", "--outcome", "ok", "--summary",
             "a plain ascii summary with no multibyte characters anywhere in it so the "
             "seventy-seventh byte is an ordinary letter and the cut width shows"},
            root, "c13");

  freeze_fixture(root / "planar.db");

  // --- pin -----------------------------------------------------------------
  for (auto const& item : k_pins) {
    auto const got = run_pinned(watch_bin(), item.args, root, item.tag);
    INFO("verb: " << item.tag << " stderr: " << got.err);
    CHECK(got.code == 0);
    CHECK(got.err.empty());
    if (item.expected.find(k_generated_at_marker) == std::string_view::npos) {
      CHECK(got.out == item.expected);
    } else {
      CHECK(got.out == with_generated_at(item.expected, got.out));
    }
  }
}
