// @file parity.t.cpp
// @brief Characterization pins on `planar-watch`'s user-visible surface,
// captured from the Zig reference before the M10 cutover deleted it
// (plan 996, tasks 6107/6065; decisions 963/982).
//
// Harness in `../parity_harness.hpp`; see that file for the database-safety
// rules it enforces and why it is a header rather than a target.
//
// ## What is pinned
//
// Leaf help pages, both `completion` failure paths (the exit-1/exit-2 pair
// that discriminates this binary's policy), the unknown-verb path, and —
// the case that matters most for this particular binary — that no ported
// invocation creates a database FILE. That last one is the read-only
// invariant observed from OUTSIDE the process, complementing
// `context.t.cpp`'s inside-the-process write-refusal proof.
//
// TASK 6065 added the `schema` CATALOG to the pinned set, and it is now
// the strongest case in the file: the transcribed bytes below were the
// oracle's own, and this binary still reproduces them exactly.
//
// ## What the M10 cutover removed
//
// Every assertion here runs against the built C++ binary alone; nothing in
// this file shells a second implementation any more. The transcribed
// expectations ARE the former oracle's output, frozen at the commit that
// deleted it, so they keep grading the port without needing it present.
//
// DECISION 1034 removed the one case that could not be frozen this way —
// "the six read verbs agree with the oracle over a seeded database". Its
// subject was cross-IMPLEMENTATION agreement over a fixture whose bytes
// (32 random hex `claim_token` characters, wall-clock row timestamps) are
// only comparable when two readers share one database. With one
// implementation there is no second reader; a normalizer permissive enough
// to pin it against a fresh run would mask the rendering regressions the
// case existed to catch. `readpin.t.cpp` beside this file carries the
// replacement (task 6545): it freezes the fixture's volatile COLUMNS after
// seeding through the CLI, so the six verbs' rendered bytes become literals
// with nothing blanked.

#include <catch2/catch_test_macros.hpp>

import std;

#include "catalog_parity.hpp"
#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

} // namespace

TEST_CASE("planar-watch: leaf help pages are exact, and still cost nothing to render", "[cmd][watch][parity]") {
  // TASK 6123 RE-BASELINE. This case used to diff each leaf's `--help`
  // page against the Zig oracle byte for byte. `src/lib/cli`'s help
  // renderer — the thing that produced those bytes — is deleted; CLI11
  // renders help now, and the operator sanctioned the re-baseline. So the
  // page is no longer oracle-comparable and this case no longer claims it
  // is. What it still does is PIN THE EXACT BYTES, captured from the built
  // binary, rather than loosening to a `contains` check: a leaf's page is
  // derived entirely from its own node, so a dropped flag or a mistyped
  // description changes it, and that is the regression worth catching.
  //
  // It also no longer needs the oracle at all, so it runs on a checkout
  // with no zig/ build — strictly more coverage than the SKIP it replaces.
  auto const arena = make_arena("leafhelp");

  auto const version = run_pinned(cpp_bin(), std::vector<std::string>{"version", "--help"}, arena.cpp_root, "version");
  CHECK(version.code == 0);
  CHECK(version.err.empty());
  CHECK(version.out == "Print the planar-watch version, commit, and C++ toolchain.\n"
                       "\n"
                       "\n"
                       "version [OPTIONS]\n"
                       "\n"
                       "\n"
                       "OPTIONS:\n"
                       "  -h,     --help              Print this help message and exit\n"
                       "\n"
                       "Exit codes:\n"
                       "  0  Success.\n"
                       "  1  Failure: entity not found, an unmapped error, or a usage error such as an unknown flag.\n");

  auto const schema = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--help"}, arena.cpp_root, "schema");
  CHECK(schema.code == 0);
  CHECK(schema.err.empty());
  CHECK(schema.out == "Print the full command tree as a JSON catalog (flags, aliases, positionals).\n"
                      "\n"
                      "\n"
                      "schema [OPTIONS]\n"
                      "\n"
                      "\n"
                      "OPTIONS:\n"
                      "  -h,     --help              Print this help message and exit\n"
                      "          --command           Emit only this command's catalog object, by full path (\"planar\n"
                      "                              task update\") or relative to the root (\"task update\"); an unknown\n"
                      "                              path exits 2 with nothing on stdout\n"
                      "          --compact           Emit one {command, summary} row per command instead of the full\n"
                      "                              catalog; with --command, only that command's row\n"
                      "\n"
                      "Exit codes:\n"
                      "  0  Success.\n"
                      "  1  Failure: entity not found, an unmapped error, or a usage error such as an unknown flag.\n"
                      "  2  Bad input: an invalid value or entity ref.\n");

  // The only ported leaf with a POSITIONAL, so the only one whose page can
  // regress by losing the POSITIONALS section.
  auto const completion = run_pinned(cpp_bin(), std::vector<std::string>{"completion", "--help"}, arena.cpp_root, "completion");
  CHECK(completion.code == 0);
  CHECK(completion.err.empty());
  CHECK(completion.out == "Generate the autocompletion script for the specified shell.\n"
                          "\n"
                          "\n"
                          "completion [OPTIONS] shell\n"
                          "\n"
                          "\n"
                          "POSITIONALS:\n"
                          "  shell REQUIRED              Shell: bash, zsh, or fish\n"
                          "\n"
                          "OPTIONS:\n"
                          "  -h,     --help              Print this help message and exit\n"
                          "\n"
                          "Exit codes:\n"
                          "  0  Success.\n"
                          "  1  Failure: entity not found, an unmapped error, or a usage error such as an unknown flag.\n"
                          "  2  Bad input: an invalid value or entity ref.\n");
}

TEST_CASE("planar-watch parity: completion's two failure paths keep their distinct exit codes",
          "[cmd][watch][parity][exitcode]") {
  // TASK 6540 RE-BASELINE. Both arms here used to be a live diff against
  // the Zig oracle. Transcribed at this commit — the two binaries agreed
  // byte for byte on both arms at transcription time — and pinned against
  // the built binary alone from here on, following the same pattern task
  // 6123 established in `the CLI surface is CLI11's now, and pinned`
  // (src/cmd/planar/parity.t.cpp): captured from the BUILT binary exactly
  // the way the oracle captures were taken, and pinned EXACTLY — trailing
  // whitespace included, never loosened to a `contains` check.
  //
  // Runs without the oracle now — it is an assertion about this binary,
  // not a comparison — so it stays live on a checkout with no zig/ build.
  auto const arena = make_arena("completionfail");

  // Handler-level refusal: exit 2, stderr only. This message comes from
  // the HANDLER, not the parser, so the CLI11 swap never touched it — it
  // was byte-identical to the oracle both before and after task 6123, and
  // still is at transcription time.
  auto const bad = run_pinned(cpp_bin(), std::vector<std::string>{"completion", "badshell"}, arena.cpp_root, "badshell");
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err == "error: unsupported shell 'badshell'; supported: bash, zsh, fish\n");

  // Parser-level refusal: exit 1, stderr only (decision 1004, task 6271 —
  // was BOTH streams before this decision). Two different codes out of
  // one verb — a collapsed exit mapping cannot satisfy both cases, and
  // THAT is what this case exists to prove. This binary's own exit-code
  // table says 1 here, where the operator binary's parser-refusal
  // contract says 2 (see `the CLI surface is CLI11's now, and pinned`) —
  // that asymmetry is preserved explicitly by the literal `1` below,
  // not left as an artifact of a since-removed comparison.
  //
  // The BYTES are CLI11's now (task 6123), moved to stderr alone by
  // decision 1004, dropping the CamelCase tag the oracle still emits.
  auto const missing = run_pinned(cpp_bin(), std::vector<std::string>{"completion"}, arena.cpp_root, "noshell");
  CHECK(missing.code == 1);
  CHECK(missing.out.empty());
  CHECK(missing.err == "error: shell is required\n");
}

TEST_CASE("planar-watch parity: an unknown verb still exits 1, matching the oracle", "[cmd][watch][parity]") {
  // TASK 6540 RE-BASELINE, same shape as above. This binary's exit-code
  // table says 1 here, where the operator binary says 2 (see `the CLI
  // surface is CLI11's now, and pinned`) — pinned explicitly below via the
  // literal `1`, so the asymmetry stays visible rather than incidental to
  // a dropped comparison.
  //
  // The BYTES are CLI11's now, moved to stderr alone by decision 1004
  // (task 6271). Transcribed from the built binary at this commit; runs
  // without the oracle from here on.
  auto const arena = make_arena("unknownverb");
  auto const got   = run_pinned(cpp_bin(), std::vector<std::string>{"nosuchverb"}, arena.cpp_root, "unknownverb");
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: planar-watch: The following argument was not expected: nosuchverb\n");
}

TEST_CASE("planar-watch: no ported invocation creates a database file", "[cmd][watch][parity][readonly]") {
  // The read-only invariant observed from outside the process. Runs
  // WITHOUT the oracle too — it is an assertion about this binary, not a
  // comparison — so it stays live even on a checkout with no zig/ build.
  auto const arena = make_arena("nodb");
  for (auto const& argv :
       std::vector<std::vector<std::string>>{{"version"}, {"schema"}, {"completion", "bash"}, {"--help"}, {"nosuchverb"}}) {
    auto const tag = argv.front();
    (void)run_pinned(cpp_bin(), argv, arena.cpp_root, tag);
    INFO("argv: " << tag);
    CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "planar.db"));
  }
}

TEST_CASE("planar-watch: a read verb still creates no database file", "[cmd][watch][parity][readonly]") {
  // The outside-the-process half of the read-only invariant, now over a
  // verb that genuinely wants a database rather than only over verbs that
  // never open one. It must FAIL to open, not bootstrap: the operator
  // binary's context would create AND migrate the file here.
  auto const arena = make_arena("readnodb");
  for (auto const& argv :
       std::vector<std::vector<std::string>>{{"ps"}, {"claims"}, {"actions"}, {"plans"}, {"tree"}, {"log", "--task", "1"}}) {
    auto const tag = argv.front();
    auto const got = run_pinned(cpp_bin(), argv, arena.cpp_root, std::format("nodb_{}", tag));
    INFO("argv: " << tag);
    CHECK(got.code == 1);
    CHECK(got.err == "error: OpenFailed\n");
    CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "planar.db"));
  }
}

TEST_CASE("planar-watch parity: every command declares what the oracle declares", "[cmd][watch][parity][catalog]") {
  // TASK 6540 RE-BASELINE. Transcribed from the built binary at commit
  // 4fc09c07, where this catalog was confirmed to declare exactly what the
  // Zig oracle declares (task 6065's `diff_against_oracle` and
  // `oracle_only_commands` both returned empty at that commit). The
  // structural facts that comparison proved are pinned directly below
  // instead of re-deriving them from a live diff every run.
  //
  // The byte-identical pin in the next case already subsumes this one
  // structurally, but the case stays — same name, same purpose — because
  // it names WHICH commands and WHY, which a byte diff does not.
  //
  // Runs without the oracle now: an assertion about this binary alone.
  auto const arena = make_arena("catalog");
  auto const got   = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "catalog");
  REQUIRE(got.code == 0);

  auto const mine = planar::cmd::parity::parse_catalog(got.out);
  REQUIRE(mine.has_value());

  // Non-vacuous: an empty catalog, or a document that failed to parse,
  // would otherwise look exactly like a clean assertion.
  CHECK(mine->size() == 17);
  CHECK(mine->contains("planar-watch ps"));
  // The four task 6065 declared that the previous tree did not have at all.
  CHECK(mine->contains("planar-watch feed"));
  CHECK(mine->contains("planar-watch sync-events"));
  CHECK(mine->contains("planar-watch run list"));
  CHECK(mine->contains("planar-watch run show"));
  // Plan 1080, task hq-watch-queue: the host queue view.
  CHECK(mine->contains("planar-watch queue"));
  // Task hq-watch-history: the ended entries.
  CHECK(mine->contains("planar-watch queue history"));
}

TEST_CASE("planar-watch parity: the catalog is BYTE-identical to the oracle's", "[cmd][watch][parity][catalog]") {
  // TASK 6540 RE-BASELINE. `diff_against_oracle` leaves out key order,
  // `docs`, `flagGroups`, `hidden`, `deprecated`, `path`, `name`, and the
  // `default` literal it deliberately skips. Before this task those were
  // covered by a live byte diff against the Zig oracle; this binary's
  // catalog was confirmed BYTE-IDENTICAL to the oracle's at commit
  // 4fc09c07, so that whole document is transcribed here and pinned
  // exactly instead, following the same pattern task 6123 established for
  // the CLI11-rendered surface in `the CLI surface is CLI11's now, and
  // pinned` (src/cmd/planar/parity.t.cpp).
  //
  // Pinned EXACTLY, byte for byte, including key order — loosening this to
  // a structural comparison would stop catching a key-order or
  // whitespace regression the oracle diff used to catch for free.
  //
  // Runs without the oracle now: an assertion about this binary alone.
  auto const arena = make_arena("catalog-bytes");
  auto const got   = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "catalog-bytes");
  REQUIRE(got.code == 0);
  REQUIRE(got.out.size() > 20000); // Not an empty string.

  // clang-format off
  std::string const expected = R"CATALOG({"schemaVersion":1,"layout":"flat","root":"planar-watch","commands":[{"name":"planar-watch","aliases":[],"hidden":false,"deprecated":null,"path":[],"command":"planar-watch","summary":"Read-only viewer for live agent activity (feed / ps / claims / actions / plans / log / tree / run).","description":"planar-watch is the human-facing live cockpit for agent activity.\n\n  The default invocation with no args is the activity feed.\n  Subcommands narrow the view; `--follow` turns each one into a\n  streaming view that emits new rows as the underlying tables\n  change. The binary opens the database in strict read-only mode\n  (SQLITE_OPEN_READONLY) — every write SQL string is rejected by\n  the SQLite driver itself, the second line of defense behind\n  this binary's `no write verbs registered` capability boundary.\n\n  `tree` renders the orchestrator → sub-agent forest by walking\n  agent_actions.parent_action_id chains.","subcommands":["feed","ps","claims","actions","plans","log","tree","run","sync-events","queue","version","completion","schema"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"feed","aliases":[],"hidden":false,"deprecated":null,"path":["feed"],"command":"planar-watch feed","summary":"Cross-cutting activity feed across all vendors (default verb).","description":"One event per claim transition, action transition, or task status\n  change, in occurrence-time order. The default planar-watch\n  invocation routes here.\n\n  Without --follow: print the initial snapshot up to --limit\n  events (default 100), newest first.\n  With --follow: print the snapshot, then stream new events as\n  they appear. Tier-1 poll; --interval defaults to 1s.\n\n  --tail N: emit the most-recent N events on first call (the\n  journalctl -f -n idiom). --tail 0 or negative exits with\n  InvalidValue. Combined with --follow: the tail emission comes\n  first, then only NEW events stream (no re-emit of tailed events).\n\n  Filters (--vendor / --plan / --task / --since) narrow both the\n  snapshot and the streaming view.\n\n  --json emits NDJSON — one JSON object per line, no surrounding\n  array, no trailing comma. Consumers can pipe through `jq -c`.","subcommands":[],"flags":[{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream new events until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id filter (matches plan-direct, task-on-plan, and plan_step-on-plan events)","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--since","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only events with at >= this ISO8601 timestamp","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Snapshot row cap (default 100)","completion":{"kind":"none","values":[]},"env":null},{"long":"--tail","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Return only the most-recent N events (must be > 0)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit NDJSON","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch feed --tail 20","planar-watch feed --follow --vendor claude"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"ps","aliases":[],"hidden":false,"deprecated":null,"path":["ps"],"command":"planar-watch ps","summary":"Snapshot of active (and stale) agent claims.","description":"Lists every currently active agent claim — one row per claim_token.\n\n  --stale also includes claims whose lease has expired OR whose\n  status is `stale` (set by `planar-agent reconcile`).\n\n  --vendor / --plan narrow the result.\n\n  --sort-by heartbeat (default) orders by most-recently-heartbeated\n  first. --sort-by lease restores the pre-M3 claimed_at ordering.\n\n  --follow turns the snapshot into a streaming view (Tier-1 poll;\n  --interval defaults to 1s). Exits 0 on SIGINT.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by vendor (claude, codex, copilot, ...)","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)","completion":{"kind":"none","values":[]},"env":null},{"long":"--stale","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Include stale + lease-expired claims","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream snapshots until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s; e.g. 100ms)","completion":{"kind":"none","values":[]},"env":null},{"long":"--sort-by","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Sort order for active claims: heartbeat (default) or lease","completion":{"kind":"none","values":[]},"env":null},{"long":"--group-by","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Group claims by dimension: role, scope, or vendor","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch ps","planar-watch ps --stale --sort-by lease"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"claims","aliases":[],"hidden":false,"deprecated":null,"path":["claims"],"command":"planar-watch claims","summary":"List claims in the agent_work_claims ledger (filterable by status).","description":"Returns claim rows from agent_work_claims. The default is\n  --status active.\n\n  --status active : claim row is in 'active' state with an\n                    unexpired lease (default).\n  --status stale  : status='stale' OR an expired-lease active\n                    claim (matches `ps --stale`).\n  --status all    : every row (active, released, completed,\n                    aborted, stale) — the full claim ledger.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by vendor","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"active (default) | stale | all","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream snapshots until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch claims --status active","planar-watch claims --plan 7 --json"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"actions","aliases":[],"hidden":false,"deprecated":null,"path":["actions"],"command":"planar-watch actions","summary":"List agent_actions rows with optional filters.","description":"Returns agent_actions rows ordered by started_at descending.\n\n  --kind     : action_kind filter (coder, reviewer, tool_call, etc.).\n  --entity   : restrict to one entity, `kind:id` form (e.g. `task:42`).\n  --plan     : restrict to actions on the plan, or on tasks/plan_steps belonging to it.\n  --task     : restrict to actions whose entity_kind=task, entity_id=N.\n  --vendor   : vendor filter.\n  --limit    : cap row count (default 100).","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"action_kind filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to one entity, kind:id form","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by task id","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Row cap (default 100)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream snapshots until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch actions --task 42 --limit 20"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"plans","aliases":[],"hidden":false,"deprecated":null,"path":["plans"],"command":"planar-watch plans","summary":"List plans with in-flight agent work.","description":"Each row pairs a plan with its in-flight summary:\n    active_claims  — claims with status='active' and\n                     lease_expires_at >= now() targeting any task\n                     under the plan.\n    active_actions — agent_actions rows with ended_at IS NULL\n                     whose entity_kind/entity_id refer to a task\n                     under the plan.\n    last_event_at  — max of claim claimed_at / heartbeat /\n                     released_at and action started_at /\n                     ended_at across the plan's tasks; null\n                     when no events recorded.\n\n  --in-flight-only drops plans where active_claims=0 AND\n  active_actions=0.","subcommands":[],"flags":[{"long":"--in-flight-only","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip plans with no live work","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream snapshots until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch plans --in-flight-only"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"log","aliases":[],"hidden":false,"deprecated":null,"path":["log"],"command":"planar-watch log","summary":"Per-entity / per-claim history (union of agent actions and claim transitions).","description":"Streams the agent_actions + agent_work_claims history scoped to\n  one entity or one claim_token. Exactly one of\n  --task / --plan / --entity / --session / --claim is required.\n\n  Entries are emitted in occurrence-time order (oldest first)\n  as a discriminated union: each entry carries a `.kind` field\n  that is either `action` (full ActionRow payload) or\n  `claim_acquired` / `claim_heartbeat` / `claim_released` /\n  `claim_stale` (with ClaimRow payload).","subcommands":[],"flags":[{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter to one task id","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter to one plan id (matches entity_kind=plan rows)","completion":{"kind":"none","values":[]},"env":null},{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter to one entity, kind:id form","completion":{"kind":"none","values":[]},"env":null},{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter to one session_id","completion":{"kind":"none","values":[]},"env":null},{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter to one claim_token","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Row cap (default 100)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch log --task 42","planar-watch log --plan 7 --limit 50"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"tree","aliases":[],"hidden":false,"deprecated":null,"path":["tree"],"command":"planar-watch tree","summary":"Render the orchestrator → sub-agent action forest.","description":"Walks agent_actions.parent_action_id chains and renders the\n  orchestrator → sub-agent forest. Root rows have parent_action_id IS NULL.\n  Each child is indented with unicode tree characters (├── / └── / │).\n\n  --root-session <id>  scope to one session's subtree (error if unknown).\n  --follow             stream; re-renders on WAL change (Tier-2 wake).\n  --interval           maximum poll cadence for --follow (default 1s).\n\n  Each row shows the claim's: scope vendor activity worktree branch last_hb.","subcommands":[],"flags":[{"long":"--root-session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Scope output to one session's subtree (session id)","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream re-renders until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s; e.g. 100ms)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch tree --root-session 3"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"run","aliases":[],"hidden":false,"deprecated":null,"path":["run"],"command":"planar-watch run","summary":"Observe workflow runs and their context records.","description":"Read-only view of run tables. `list` covers both workflow_runs (wf)\nand the runs table (op-arm); `show` drills into wf-source runs only.\n\n  list  — list runs (--plan / --status / --arm filters).\n  show  — drill into one wf-source run's context records.","subcommands":["list","show"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["run","list"],"command":"planar-watch run list","summary":"List workflow runs (filterable by plan, status, and source arm).","description":"Returns runs ordered by started_at descending.\n\n  --plan <id>    restrict to runs for the given plan.\n  --status <s>   restrict by status: running | completed | failed |\n                 interrupted | abandoned. Default: all.\n  --arm <a>      source table: wf (workflow_runs / context-plane),\n                 op (runs / op-arm), or all (default, both).\n  --json         emit a single JSON object instead of human text.","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by status (default: all)","completion":{"kind":"none","values":[]},"env":null},{"long":"--arm","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Source arm: wf | op | all (default: all)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch run list --plan 7 --status running"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["run","show"],"command":"planar-watch run show","summary":"Show one workflow run plus its context_records grouped by stage.","description":"Returns the full workflow_runs row for <id> plus all\n  context_records for that run, grouped and ordered by\n  stage then created_at.\n\n  Exits non-zero when the run id is unknown.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"id","kind":"string","required":true,"default":null,"description":"Workflow run id (integer)","completion":{"kind":"none","values":[]}}],"docs":{"examples":["planar-watch run show 3"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"sync-events","aliases":[],"hidden":false,"deprecated":null,"path":["sync-events"],"command":"planar-watch sync-events","summary":"List sync_events rows with optional filters (read-only).","description":"Returns sync_events rows ordered by `at` descending.\n\n  --plan     : restrict to events whose link belongs to the given plan id.\n  --system   : restrict to events via a link on the given external system slug.\n  --entity   : restrict to events via a link on one entity, `kind:id` form.\n  --outcome  : filter by outcome value (ok, conflict, error, noop, …).\n  --since    : only return rows with `at` >= this ISO8601 timestamp.\n  --limit    : cap row count (default 100).","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--system","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by external system slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by entity, kind:id form (e.g. task:42)","completion":{"kind":"none","values":[]},"env":null},{"long":"--outcome","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by outcome (ok, conflict, error, noop, …)","completion":{"kind":"none","values":[]},"env":null},{"long":"--since","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only rows at >= this ISO8601 timestamp","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Row cap (default 100)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch sync-events --plan 7 --outcome conflict"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"queue","aliases":[],"hidden":false,"deprecated":null,"path":["queue"],"command":"planar-watch queue","summary":"List the host build and test queue: running and waiting entries, marking any that are not live.","description":"Lists every running and waiting entry of the host build and test queue, read-only.\n\n  Entries are in sequence order, running and waiting alike (POS is the place among the\n  waiting entries). An entry that fails the liveness rules is marked NOT-LIVE and is left in\n  place: this view never reaps, refreshes or writes. Nested entries are marked nested:<parent>.\n  A missing planar.db is an error, as for every viewer verb.\n\n  Text: one line per entry, columns SEQ STATE POS NOTES WAITED RAN VENDOR ROLE LABEL DIRECTORY\n  COMMAND, the command shell-quoted. --json: an array of objects, one per entry.\n\n  `queue history` lists the entries that have ended.","subcommands":["history"],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"history","aliases":[],"hidden":false,"deprecated":null,"path":["queue","history"],"command":"planar-watch queue history","summary":"Lists the entries of the host build and test queue that have ended, read-only.\n\n  Oldest first (by end time). Each row gives the outcome (exited, signaled, timeout, cancelled, wait_timeout,\n  not_started or abandoned), the exit code or signal, how long it waited and ran, who submitted it, and\n  who cancelled it or which entry replaced it. --since <duration> keeps only rows that ended within that\n  long (an integer and a unit ms, s, m, h or d, at most 36500d). A missing planar.db is an error, as for every viewer verb.\n\n  Text: one line per row, columns SEQ OUTCOME RESULT ENDED WAITED RAN NOTES VENDOR ROLE LABEL DIRECTORY\n  COMMAND. --json: an array of objects, one per row.","description":"Lists the entries of the host build and test queue that have ended, read-only.\n\n  Oldest first (by end time). Each row gives the outcome (exited, signaled, timeout, cancelled, wait_timeout,\n  not_started or abandoned), the exit code or signal, how long it waited and ran, who submitted it, and\n  who cancelled it or which entry replaced it. --since <duration> keeps only rows that ended within that\n  long (an integer and a unit ms, s, m, h or d, at most 36500d). A missing planar.db is an error, as for every viewer verb.\n\n  Text: one line per row, columns SEQ OUTCOME RESULT ENDED WAITED RAN NOTES VENDOR ROLE LABEL DIRECTORY\n  COMMAND. --json: an array of objects, one per row.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"inherited","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null},{"long":"--since","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only rows that ended within this long: an integer and a unit (ms, s, m, h, d), at most 36500d","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-watch queue history --since 24h"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"version","aliases":[],"hidden":false,"deprecated":null,"path":["version"],"command":"planar-watch version","summary":"Print the planar-watch version, commit, and C++ toolchain.","description":"Print the planar-watch version, commit, and C++ toolchain.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"completion","aliases":[],"hidden":false,"deprecated":null,"path":["completion"],"command":"planar-watch completion","summary":"Generate the autocompletion script for the specified shell.","description":"Generate the autocompletion script for the specified shell.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"shell","kind":"string","required":true,"default":null,"description":"Shell: bash, zsh, or fish","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"schema","aliases":[],"hidden":false,"deprecated":null,"path":["schema"],"command":"planar-watch schema","summary":"Print the full command tree as a JSON catalog (flags, aliases, positionals).","description":"Print the full command tree as a JSON catalog (flags, aliases, positionals).","subcommands":[],"flags":[{"long":"--command","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Emit only this command's catalog object, by full path (\"planar task update\") or relative to the root (\"task update\"); an unknown path exits 2 with nothing on stdout","completion":{"kind":"none","values":[]},"env":null},{"long":"--compact","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit one {command, summary} row per command instead of the full catalog; with --command, only that command's row","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}}]}
)CATALOG";
  // clang-format on
  CHECK(got.out == expected);
}

TEST_CASE("planar-watch: `run list` / `run show` / `sync-events` are ported, not stubbed",
          "[cmd][watch][parity][not-implemented]") {
  // Task 6448 landed real handlers for all three. This case used to pin
  // their exit-64 stub refusal; it is rewritten (not deleted) to pin what
  // replaced it, so the suite keeps grading this surface rather than
  // silently losing coverage of it.
  auto const arena = make_arena("unported");
  auto const run   = [&](std::vector<std::string> args, std::string_view tag) {
    return run_pinned(cpp_bin(), args, arena.cpp_root, tag);
  };

  // `feed` was one of these until task 6039 landed it for real (see
  // handlers/feed.cppm and the default-verb cases in handlers.t.cpp); it
  // is no longer declared-but-unported and does not belong in this list.

  // Against an EMPTY arena (no `planar init`), a ported read verb that
  // reaches the read-only database handle fails the same way `ps` already
  // does: `OpenFailed`, exit 1 — NOT exit 64. That is the discrimination
  // that makes "ported now" observable rather than assumed.
  auto const events = run({"sync-events"}, "syncevents");
  CHECK(events.code == 1);
  CHECK(events.err == "error: OpenFailed\n");

  // A nested one, to prove the key is the full path and not the leaf name.
  auto const run_list = run({"run", "list"}, "runlist");
  CHECK(run_list.code == 1);
  CHECK(run_list.err == "error: OpenFailed\n");

  auto const run_show = run({"run", "show", "1"}, "runshow");
  CHECK(run_show.code == 1);
  CHECK(run_show.err == "error: OpenFailed\n");

  // And the discrimination that makes the three above mean something: a
  // verb that needs no database at all does NOT answer OpenFailed —
  // `version` needs nothing and exits 0.
  auto const ported = run({"version"}, "ported");
  CHECK(ported.code == 0);
  CHECK(ported.err.empty());
  CHECK(ported.out.starts_with("planar-watch "));

  auto const ported_db = run({"ps", "--json"}, "porteddb");
  CHECK(ported_db.code == 1);
  CHECK(ported_db.err == "error: OpenFailed\n");

  // A pure GROUP still renders help at exit 0 — matching the oracle, which
  // has no dual node on this binary.
  auto const group = run({"run"}, "group");
  CHECK(group.code == 0);
  CHECK(group.out.contains("list"));
  CHECK(group.out.contains("show"));

  // `run list --arm bogus` against a database that DOES exist: the
  // database is opened first (matching the oracle's own ordering — see
  // `handlers::run_list`), so this arm's `--arm` validation is exercised
  // once a handle is available. Pinned in `handlers.t.cpp` against a
  // migrated fixture rather than here, where every other case in this
  // TEST_CASE deliberately points at an EMPTY arena to pin `OpenFailed`.
}

// --- `schema --command` and `schema --compact` (task 7204) ---------------
//
// Expectations are derived from the task's contract, not from the emitter:
// a single catalog object for one path, a two-key row per command, exit 2
// with an empty stdout for an unknown path, and a bare verb-shaped value
// (`task`) that is a lookup rather than a reordered subcommand.

namespace {

/// @brief Count non-overlapping occurrences of `needle` in `text`.
auto count_of(std::string_view text, std::string_view needle) -> std::size_t {
  std::size_t n = 0;
  for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

} // namespace

TEST_CASE("planar-watch schema --command emits exactly one command's object", "[cmd][watch][schema][7204]") {
  auto const arena = make_arena("schema7204one");
  auto const one =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-watch ps"}, arena.cpp_root, "one");
  CHECK(one.code == 0);
  CHECK(one.err.empty());
  CHECK(one.out.starts_with(R"({"name":")"));
  CHECK(one.out.ends_with("}\n"));
  CHECK(one.out.contains(R"("command":"planar-watch ps")"));
  CHECK(count_of(one.out, R"("command":")") == 1);
  CHECK(one.out.size() < 4096);

  // The relative spelling resolves to the same bytes.
  auto const rel = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command=ps"}, arena.cpp_root, "rel");
  CHECK(rel.code == 0);
  CHECK(rel.out == one.out);

  // The object is the one the full catalog carries, byte for byte.
  auto const full = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "full");
  REQUIRE(full.code == 0);
  CHECK(full.out.contains(one.out.substr(0, one.out.size() - 1)));
}

TEST_CASE("planar-watch schema --command with an unknown path exits 2, names it, prints nothing", "[cmd][watch][schema][7204]") {
  auto const arena = make_arena("schema7204bad");
  auto const bad =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-watch ps nonesuch"}, arena.cpp_root, "bad");
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err.contains("planar-watch ps nonesuch"));
}

TEST_CASE("planar-watch schema --compact is one two-key row per command", "[cmd][watch][schema][7204]") {
  auto const arena   = make_arena("schema7204compact");
  auto const full    = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "full");
  auto const compact = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--compact"}, arena.cpp_root, "compact");
  REQUIRE(full.code == 0);
  REQUIRE(compact.code == 0);
  CHECK(compact.err.empty());
  auto const commands = count_of(full.out, R"("path":[)");
  CHECK(commands > 1);
  CHECK(count_of(compact.out, R"({"command":")") == commands);
  CHECK(count_of(compact.out, R"(,"summary":")") == commands);
  CHECK_FALSE(compact.out.contains(R"("flags")"));
  CHECK_FALSE(compact.out.contains(R"("name":)"));
  CHECK(compact.out.size() < 40 * 1024);
  CHECK(compact.out.size() < full.out.size());

  // Both flags: that one command's row, bare.
  auto const row =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--compact", "--command", "ps"}, arena.cpp_root, "row");
  CHECK(row.code == 0);
  CHECK(row.out.starts_with(R"({"command":"planar-watch ps","summary":")"));
  CHECK(count_of(row.out, R"("command":")") == 1);
  CHECK(compact.out.contains(row.out.substr(0, row.out.size() - 1)));
}

TEST_CASE("planar-watch schema --command ps is a lookup, not the ps subcommand", "[cmd][watch][schema][7204]") {
  auto const arena = make_arena("schema7204edge");
  auto const bare  = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "ps"}, arena.cpp_root, "bare");
  auto const full =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-watch ps"}, arena.cpp_root, "full");
  CHECK(bare.code == 0);
  CHECK(bare.err.empty());
  CHECK(bare.out.starts_with(R"({"name":"ps",)"));
  CHECK(bare.out.contains(R"("command":"planar-watch ps")"));
  CHECK(bare.out == full.out);
  // The `=` form. (A leading flag routes to the default verb `feed` on this binary, so the
  // value-before-verb spelling is not a `schema` invocation here.)
  auto const eq = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command=ps"}, arena.cpp_root, "eq");
  CHECK(eq.out == bare.out);
}
