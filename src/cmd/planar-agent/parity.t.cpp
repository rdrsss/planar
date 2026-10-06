// @file parity.t.cpp
// @brief Differential tests: run the built C++ `planar-agent` and the Zig
// reference over identical argv in identical pinned scratch environments,
// and require identical stdout, stderr and exit code (plan 996, tasks 6107
// and 6123).
//
// The harness lives in `../parity_harness.hpp` — a plain header, included
// rather than linked, because D18 forbids a `cmd_* -> cmd_*` edge and a
// shared layer-1 library for test scaffolding would be worse. That file
// documents the database-safety rules it enforces (`cd` then `env`, never
// `VAR=x cd dir && binary`; per-invocation capture files because Zig's
// writer uses POSITIONAL writes).
//
// A live diff is stronger evidence than a transcribed expectation, because
// a transcription can be wrong and a diff cannot. M9's parity gate rests on
// exactly this shape.
//
// ## What is compared, and what deliberately is not
//
// COMPARED: every argv shape whose output is derived from ENGINE state or
// engine renderers — the claim-ritual verbs' payloads — plus the EXIT CODE
// of every parse-failure path, which is where this binary's divergence from
// the operator binary lives and which task 6123 was required to preserve.
//
// ALSO COMPARED, and this is the task-6123 replacement for the leaf-help
// diffs: the DECLARED SURFACE of every ported command, taken from the
// `schema` catalog both binaries emit. See `../catalog_parity.hpp` for the
// full argument — the short version is that a rendered help page was only
// ever a proxy for "were these flags/positionals transcribed correctly",
// CLI11 renders help now so the page is no longer comparable, and the
// catalog answers the underlying question directly and more precisely.
//
// NOT COMPARED, each for a stated reason:
//
//   `version`   The `cxx <compiler>` vs `zig <version>` divergence is
//               inherited from `planar.cliapp.version` and shared with the
//               operator binary. Shape is pinned in handlers.t.cpp.
//   `--help`    CLI11 renders help now (task 6123), so no page in this
//               binary is oracle-comparable. Leaf pages are pinned exactly
//               against the built binary in handlers.t.cpp instead; the
//               ROOT page additionally lists this tree's FOURTEEN verbs
//               against the oracle's eighteen.
//   parse-error
//   BYTES       CLI11 writes its own wording (task 6123). The two-stream
//               SHAPE and the exit CODE are still compared; the bytes are
//               pinned against the built binary.
//
// SKIP, not fail, when the oracle is absent: `zig/zig-out/bin/planar-agent`
// is a build artifact, not a checked-in file (D6).
//
// TASK 6539 (decision 963/982's third gate condition): every case that used
// to skip on an absent oracle in THIS file has been retired onto a pinned
// expectation, transcribed from a run in which the C++ and Zig binaries
// agreed, at commit 4fc09c0777cb0456e8ef52889dc7ca7f44a93f34. None of the
// nine `TEST_CASE`s below reads `zig/` any more; the file compiles and
// passes on a checkout with no Zig build at all. The break-probe case
// ("the catalog comparison actually discriminates") never read the oracle
// either — it always fixtured both sides by hand — and is unchanged.

#include <catch2/catch_test_macros.hpp>

// The claim-ritual cases below normalise volatile fields with a regex, and
// read the two arenas' final row state back through `planar.db`.
#include <regex>

import std;
import planar.db;

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

// TASK 6539: `zig_bin()`, `oracle_available()` and `both()` used to live
// here — the last of every case in this file that read the Zig oracle
// live. All seven are retired (see each `TEST_CASE` below); nothing in
// this translation unit calls any of the three any more, so they are
// deleted rather than left as dead code an unused-function warning would
// have to route around. `PLANAR_CPP_BIN`/`cpp_bin()` above is the only
// binary path this file still needs.

} // namespace

TEST_CASE("the pinned environment actually reaches planar-agent", "[cmd][agent][parity][safety]") {
  // A guard on the HARNESS, not on the binary, and it exists because this
  // harness's ancestor got it wrong once and wrote ten rows into the
  // operator's live database. If the environment stopped reaching the
  // child, `$PLANAR_DB` would fall back to `$HOME/.planar/planar.db` — so
  // this asserts the child SEES the pinned value.
  auto const                     arena = make_arena("envguard");
  std::vector<std::string> const args{"-c", "printf '%s' \"$PLANAR_DB\""};
  auto const                     got = run_pinned("/bin/sh", args, arena.cpp_root, "probe");
  CHECK(got.code == 0);
  CHECK(got.out == (arena.cpp_root / "planar.db").string());
}

TEST_CASE("planar-agent parity: parse failures still exit 1, not the operator binary's 2", "[cmd][agent][parity][exitcode]") {
  // TASK 6539 RETIREMENT. This case's exit-CODE comparison against the
  // oracle was confirmed passing (`cpp_verb.code == zig_verb.code == 1`,
  // same for `cpp_flag`) before this rewrite — the oracle-derived half of
  // the contract, "1, not the operator binary's 2", is preserved below as
  // a literal `1` rather than a live diff. The BYTES were already pinned
  // against the built binary as of task 6123 and are unchanged.
  //
  // The whole reason this binary needed its own exit module. If the port
  // had reused `planar`'s policy these would come back 2 and this case
  // would fail on the code alone.
  //
  // Decision 1004 (task 6271): the C++ tree moves the formatted message to
  // stderr and drops the CamelCase tag, so it no longer matches the
  // oracle's own stdout/stderr split at all — that divergence is recorded
  // and deliberate. Only the exit code is compared against the oracle here;
  // the C++ stdout/stderr bytes are pinned against the built binary.
  //
  // Runs without the oracle — it is an assertion about this binary, not a
  // comparison — so it stays live on a checkout with no zig/ build.
  auto const verb_arena = make_arena("unknownverb");
  auto const cpp_verb   = run_pinned(cpp_bin(), std::vector<std::string>{"nosuchverb"}, verb_arena.cpp_root, "unknownverb");
  CHECK(cpp_verb.code == 1);
  CHECK(cpp_verb.out.empty());
  CHECK(cpp_verb.err == "error: planar-agent: The following argument was not expected: nosuchverb\n");

  auto const flag_arena = make_arena("unknownflag");
  auto const cpp_flag =
      run_pinned(cpp_bin(), std::vector<std::string>{"version", "--badflag"}, flag_arena.cpp_root, "unknownflag");
  CHECK(cpp_flag.code == 1);
  CHECK(cpp_flag.out.empty());
  CHECK(cpp_flag.err == "error: version: The following argument was not expected: --badflag\n");
}

TEST_CASE("planar-agent parity: every ported command declares what the oracle declares", "[cmd][agent][parity][catalog]") {
  // TASK 6539 RETIREMENT, transcribed at commit
  // 4fc09c0777cb0456e8ef52889dc7ca7f44a93f34, where this case's live
  // `diff_against_oracle` / `oracle_only_commands` comparison against the
  // oracle was confirmed passing before this rewrite: 29 commands on both
  // sides, zero declaration problems, zero oracle-only commands. Both of
  // those functions take a SECOND catalog to diff against, and that second
  // argument was always the oracle's live read — there is no oracle-free
  // way to keep calling them meaningfully. So this case keeps only the
  // non-vacuous structural assertions about the PARSED catalog itself.
  //
  // The stronger claim those two functions used to prove — full
  // field-for-field, order-for-order agreement with the oracle, PLUS key
  // order, `docs`, `flagGroups`, `hidden`, `deprecated`, `path` and `name`,
  // which the structured comparison never covered — is pinned byte-for-byte
  // in the very next case ("the catalog is BYTE-identical to the oracle's"),
  // which is strictly stronger than this one ever was and needs no oracle
  // read to state its own contract.
  //
  // TASK 6123. This case replaces "leaf help pages match byte for byte",
  // which diffed each of the fourteen ported leaves' `--help` output
  // against the oracle. CLI11 renders help now, so that diff cannot pass
  // — but the question it existed to answer ("did `tree.cpp` transcribe
  // the oracle's flags, required-ness and positionals correctly?") is
  // answered here directly, off the `schema` catalog both binaries emit.
  //
  // Strictly stronger in two ways: it covers PARENT nodes and the root as
  // well as leaves, and it distinguishes a required flag from an optional
  // one explicitly rather than through a rendered `REQUIRED` marker. It is
  // weaker in exactly one: it says nothing about help LAYOUT, which is
  // CLI11's now and is pinned against the built binary in handlers.t.cpp.
  //
  // Runs without the oracle — it is an assertion about this binary, not a
  // comparison — so it stays live on a checkout with no zig/ build.
  auto const arena = make_arena("catalog");
  auto const cpp   = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "catalog");
  REQUIRE(cpp.code == 0);

  auto const mine = planar::cmd::parity::parse_catalog(cpp.out);
  REQUIRE(mine.has_value());

  // Non-vacuous: a catalog that failed to parse would look identical to a
  // clean one, and an empty catalog would trivially "contain" nothing.
  CHECK(mine->size() == 36);
  CHECK(mine->contains("planar-agent fail"));
  CHECK(mine->contains("planar-agent action start"));
  // The unported half, declared by task 6065 and absent before it.
  CHECK(mine->contains("planar-agent ingest"));
  CHECK(mine->contains("planar-agent context capsule"));
  // task 6847: a pid-less run's lease is extended without a pid probe.
  CHECK(mine->contains("planar-agent run heartbeat"));
  // Plan 1080, task hq-queue-run-verb: the queue domain and its foreground
  // verb. The domain node counts as one command and `queue run` as another.
  CHECK(mine->contains("planar-agent queue"));
  CHECK(mine->contains("planar-agent queue run"));
  // Task hq-queue-cancel: the second verb of the domain.
  CHECK(mine->contains("planar-agent queue cancel"));
  // Task hq-queue-status: the read-only status verb.
  CHECK(mine->contains("planar-agent queue status"));
  // Task hq-queue-rule-verb: the verb that prints the agent rule text.
  CHECK(mine->contains("planar-agent queue rule"));
  CHECK(mine->contains("planar-agent queue wait"));
}

TEST_CASE("planar-agent parity: the catalog pins the current declared surface", "[cmd][agent][parity][catalog]") {
  // TASK 6539 RETIREMENT. The catalog size above and the catalog's
  // shape are transcription-verified, non-oracle assertions; this case
  // pins every byte of the current `schema` document. Its original baseline
  // was transcribed from a run in which the C++ and Zig binaries agreed
  // (commit 4fc09c0777cb0456e8ef52889dc7ca7f44a93f34); later declared
  // surfaces, including queue wait, are deliberate additions. This is
  // the strongest statement available (task 6065): it covers key ORDER,
  // `docs`, `flagGroups`, `hidden`, `deprecated`, `path` and `name` —
  // everything the structured comparison above leaves out.
  //
  // `planar` cannot make this claim: four of its flags declare an
  // empty-string default that CLI11 cannot distinguish from no default at
  // all. That is measured and enumerated in ../catalog_parity.hpp.
  //
  // Runs without the oracle — it is an assertion about this binary, not a
  // comparison — so it stays live on a checkout with no zig/ build.
  //
  // DELIBERATE DIVERGENCE FROM THE ORACLE (plan 1033, tasks 6488/6490):
  // re-pinned after engine supervision added `--as` / `--attempt` to
  // complete, fail, release, block and heartbeat, `--override-supervisor` to
  // the four terminal verbs, and `--supervisor` / `--attempt` to
  // claim-associate, whose `--run` became optional; task 6489 added
  // `--override-supervisor` to reconcile and abort. Every other byte is the
  // oracle transcription above; the divergence is recorded as a decision on
  // plan 1033.
  auto const arena = make_arena("catalog-bytes");
  auto const cpp   = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "catalog-bytes");
  REQUIRE(cpp.code == 0);
  REQUIRE(cpp.out.size() > 30000); // Not an empty string.
  CHECK(
      cpp.out ==
      R"CATALOG({"schemaVersion":1,"layout":"flat","root":"planar-agent","commands":[{"name":"planar-agent","aliases":[],"hidden":false,"deprecated":null,"path":[],"command":"planar-agent","summary":"Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).\n\n  This binary writes agent_actions, agent_work_claims, the routing_dispatch_*\n  tables, and the host-queue tables (queue_entries, queue_history,\n  queue_schema). It changes tasks.status only as part of a coordinated\n  claim operation; planning entities are written by planar.","description":"Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).\n\n  This binary writes agent_actions, agent_work_claims, the routing_dispatch_*\n  tables, and the host-queue tables (queue_entries, queue_history,\n  queue_schema). It changes tasks.status only as part of a coordinated\n  claim operation; planning entities are written by planar.","subcommands":["version","pull","peek","complete","fail","release","block","claim","heartbeat","claim-associate","action","ingest","reconcile","abort","schema","run","dispatch","context","queue"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"version","aliases":[],"hidden":false,"deprecated":null,"path":["version"],"command":"planar-agent version","summary":"Print the planar-agent version, commit, and C++ toolchain.","description":"Print the planar-agent version, commit, and C++ toolchain.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"pull","aliases":[],"hidden":false,"deprecated":null,"path":["pull"],"command":"planar-agent pull","summary":"Atomically pick the next eligible task, claim it, and flip status to doing.","description":"Atomically pick the next eligible task, claim it, and flip status to doing.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"planar-agent","description":"Vendor tag (default: planar-agent)","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor-session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor session id (e.g. claude:s1)","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Role name (planner|coder|reviewer|test_coder|...)","completion":{"kind":"none","values":[]},"env":null},{"long":"--ttl","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"600","description":"Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)","completion":{"kind":"none","values":[]},"env":null},{"long":"--purpose","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Free-text purpose recorded on the claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--base-ref","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Git ref the work is based on","completion":{"kind":"none","values":[]},"env":null},{"long":"--worktree","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Worktree id or path for isolation context","completion":{"kind":"none","values":[]},"env":null},{"long":"--repo-root","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Absolute path of checkout to probe locality against","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-locality-probe","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the git locality probe","completion":{"kind":"none","values":[]},"env":null},{"long":"--metadata","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Opaque text (typically JSON) persisted on the dispatch action row; validated as well-formed JSON when supplied","completion":{"kind":"none","values":[]},"env":null},{"long":"--parent-action","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Parent action id; wires the new action as a child of this action in `planar-watch tree` (cross-session hierarchy)","completion":{"kind":"none","values":[]},"env":null},{"long":"--run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"workflow_runs.id to associate with this claim (populated by an external workflow harness; omit for interactive claims)","completion":{"kind":"none","values":[]},"env":null},{"long":"--stage","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Workflow stage name (e.g. code, review) to record on the claim; requires --run","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"int","required":true,"default":null,"description":"Plan id to pull from","completion":{"kind":"none","values":[]}}],"docs":{"examples":["planar-agent pull 7 --vendor claude --role coder","planar-agent pull 7 --vendor claude --role coder --ttl 4h --worktree ../wt"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"peek","aliases":[],"hidden":false,"deprecated":null,"path":["peek"],"command":"planar-agent peek","summary":"Read-only what's-next selector (same query as pull, no writes).","description":"Read-only what's-next selector (same query as pull, no writes).","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"plan-id","kind":"int","required":true,"default":null,"description":"Plan id to peek into","completion":{"kind":"none","values":[]}}],"docs":{"examples":["planar-agent peek 7","planar-agent peek 7 --json"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"complete","aliases":[],"hidden":false,"deprecated":null,"path":["complete"],"command":"planar-agent complete","summary":"Atomically end the work session: task → done, claim → completed.","description":"Atomically end the work session: task → done, claim → completed.","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token returned by pull/claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--summary","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Free-text completion summary recorded on the action","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-locality-probe","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the git locality probe and commit collection","completion":{"kind":"none","values":[]},"env":null},{"long":"--as","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["caller","engine"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":null,"description":"Who is acting: caller (default) or engine (the supervisor of an engine-associated claim; needs --attempt)","completion":{"kind":"none","values":[]},"env":null},{"long":"--attempt","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The Centurion attempt the engine acts for; must match the claim's associated attempt","completion":{"kind":"none","values":[]},"env":null},{"long":"--override-supervisor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Operator recovery: let the caller terminate an engine-supervised claim (logged as supervisor_override)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent complete --claim 68fba76b319bcba1772f6e40a9b76f75 --summary \"Landed the migration\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"fail","aliases":[],"hidden":false,"deprecated":null,"path":["fail"],"command":"planar-agent fail","summary":"Atomically fail the work session: task → todo, claim → aborted.","description":"Atomically fail the work session: task → todo, claim → aborted.","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token returned by pull/claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Failure reason recorded on the claim and action","completion":{"kind":"none","values":[]},"env":null},{"long":"--category","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["usage_limit","context_limit","output_limit","tool_failure","validation","unknown"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":"unknown","description":"Closed failure category (default: unknown)","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-locality-probe","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the git locality probe and commit collection","completion":{"kind":"none","values":[]},"env":null},{"long":"--as","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["caller","engine"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":null,"description":"Who is acting: caller (default) or engine (the supervisor of an engine-associated claim; needs --attempt)","completion":{"kind":"none","values":[]},"env":null},{"long":"--attempt","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The Centurion attempt the engine acts for; must match the claim's associated attempt","completion":{"kind":"none","values":[]},"env":null},{"long":"--override-supervisor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Operator recovery: let the caller terminate an engine-supervised claim (logged as supervisor_override)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent fail --claim 68fba76b319bcba1772f6e40a9b76f75 --reason \"Tests fail on the roundtrip\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"release","aliases":[],"hidden":false,"deprecated":null,"path":["release"],"command":"planar-agent release","summary":"Graceful give-up: task → todo, claim → released (vs fail's aborted).","description":"Graceful give-up: task → todo, claim → released (vs fail's aborted).","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token returned by pull/claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Optional reason for releasing","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-locality-probe","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the git locality probe and commit collection","completion":{"kind":"none","values":[]},"env":null},{"long":"--as","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["caller","engine"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":null,"description":"Who is acting: caller (default) or engine (the supervisor of an engine-associated claim; needs --attempt)","completion":{"kind":"none","values":[]},"env":null},{"long":"--attempt","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The Centurion attempt the engine acts for; must match the claim's associated attempt","completion":{"kind":"none","values":[]},"env":null},{"long":"--override-supervisor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Operator recovery: let the caller terminate an engine-supervised claim (logged as supervisor_override)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent release --claim 68fba76b319bcba1772f6e40a9b76f75 --reason \"Out of turn budget\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"block","aliases":[],"hidden":false,"deprecated":null,"path":["block"],"command":"planar-agent block","summary":"Atomically park the task on an external blocker.","description":"Atomically park the task on an external blocker.","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token returned by pull/claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--blocker","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Task id of the blocker (entity_links target)","completion":{"kind":"none","values":[]},"env":null},{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Free-text reason recorded on the claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-locality-probe","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the git locality probe and commit collection","completion":{"kind":"none","values":[]},"env":null},{"long":"--as","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["caller","engine"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":null,"description":"Who is acting: caller (default) or engine (the supervisor of an engine-associated claim; needs --attempt)","completion":{"kind":"none","values":[]},"env":null},{"long":"--attempt","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The Centurion attempt the engine acts for; must match the claim's associated attempt","completion":{"kind":"none","values":[]},"env":null},{"long":"--override-supervisor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Operator recovery: let the caller terminate an engine-supervised claim (logged as supervisor_override)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent block --claim 68fba76b319bcba1772f6e40a9b76f75 --blocker 43 --reason \"Needs the schema change\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"claim","aliases":[],"hidden":false,"deprecated":null,"path":["claim"],"command":"planar-agent claim","summary":"Direct entity claim; task claims atomically transition todo to doing by default.","description":"Direct entity claim; task claims atomically transition todo to doing by default.","subcommands":[],"flags":[{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Entity ref: task:<id> | plan:<id> | plan_step:<id>","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"planar-agent","description":"Vendor tag (default: planar-agent)","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor-session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor session id (e.g. claude:s1)","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Role name (planner|coder|reviewer|test_coder|...)","completion":{"kind":"none","values":[]},"env":null},{"long":"--model","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Model actually used, recorded verbatim as an opaque string. Never validated against a supported list.","completion":{"kind":"none","values":[]},"env":null},{"long":"--ttl","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"600","description":"Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)","completion":{"kind":"none","values":[]},"env":null},{"long":"--purpose","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Free-text purpose recorded on the claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--worktree","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Worktree id or path for isolation context","completion":{"kind":"none","values":[]},"env":null},{"long":"--repo-root","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Absolute path of checkout to probe locality against","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-locality-probe","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the git locality probe","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-transition","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Claim without changing task status (plan and plan_step are always unchanged)","completion":{"kind":"none","values":[]},"env":null},{"long":"--force","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Take over an existing live claim (operator recovery)","completion":{"kind":"none","values":[]},"env":null},{"long":"--run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"workflow_runs.id to associate with this claim (populated by an external workflow harness; omit for interactive claims)","completion":{"kind":"none","values":[]},"env":null},{"long":"--stage","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Workflow stage name (e.g. code, review) to record on the claim; requires --run","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent claim --entity task:42 --vendor claude --role coder","planar-agent claim --entity task:42 --no-transition"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"heartbeat","aliases":[],"hidden":false,"deprecated":null,"path":["heartbeat"],"command":"planar-agent heartbeat","summary":"Refresh the lease on an active claim.","description":"Refresh the lease on an active claim.","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token to refresh","completion":{"kind":"none","values":[]},"env":null},{"long":"--ttl","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Set a new TTL absolutely (accepts bare int seconds or suffixed duration: 10m, 1h, 500ms). When omitted, the claim's CURRENT lease length is renewed from now — a heartbeat never shortens the lease it was sent to preserve.","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Free-text status string recorded on the heartbeat action row's summary column","completion":{"kind":"none","values":[]},"env":null},{"long":"--as","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["caller","engine"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":null,"description":"Who is acting: caller (default) or engine (the supervisor of an engine-associated claim; needs --attempt)","completion":{"kind":"none","values":[]},"env":null},{"long":"--attempt","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The Centurion attempt the engine acts for; must match the claim's associated attempt","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent heartbeat --claim 68fba76b319bcba1772f6e40a9b76f75","planar-agent heartbeat --claim 68fba76b319bcba1772f6e40a9b76f75 --status \"validating: unit tests\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"claim-associate","aliases":[],"hidden":false,"deprecated":null,"path":["claim-associate"],"command":"planar-agent claim-associate","summary":"Associate a pre-acquired active claim with a workflow run (and optional stage), and/or hand it to the engine supervisor. At least one of --run or --supervisor.","description":"Associate a pre-acquired active claim with a workflow run (and optional stage), and/or hand it to the engine supervisor. At least one of --run or --supervisor.","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token to associate","completion":{"kind":"none","values":[]},"env":null},{"long":"--run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"workflow_runs.id to stamp on the claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--stage","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Stage name to record (e.g. code, review); omit for NULL","completion":{"kind":"none","values":[]},"env":null},{"long":"--supervisor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["caller","engine"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":null,"description":"engine: hand the claim to the engine supervisor (one-way; needs --attempt). caller: assert it is still caller-supervised","completion":{"kind":"none","values":[]},"env":null},{"long":"--attempt","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The Centurion attempt supervising the claim (with --supervisor engine)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent claim-associate --claim 68fba76b319bcba1772f6e40a9b76f75 --run 3 --stage build"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"action","aliases":[],"hidden":false,"deprecated":null,"path":["action"],"command":"planar-agent action","summary":"Nested action lifecycle (sub-tool-calls inside a claim).","description":"Nested action lifecycle (sub-tool-calls inside a claim).","subcommands":["start","end"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"start","aliases":[],"hidden":false,"deprecated":null,"path":["action","start"],"command":"planar-agent action start","summary":"Start a nested action under a claim (child of the claim's role action).","description":"Start a nested action under a claim (child of the claim's role action).","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token the action attaches to","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Action kind (planner|coder|tool_call|heartbeat|...)","completion":{"kind":"none","values":[]},"env":null},{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Optional entity ref kind:id","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor-role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Optional vendor role tag","completion":{"kind":"none","values":[]},"env":null},{"long":"--repo-root","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Absolute path of checkout to probe locality against","completion":{"kind":"none","values":[]},"env":null},{"long":"--no-locality-probe","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip the git locality probe","completion":{"kind":"none","values":[]},"env":null},{"long":"--metadata","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Opaque text (typically JSON) persisted on the action row; validated as well-formed JSON when supplied","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent action start --claim 68fba76b319bcba1772f6e40a9b76f75 --kind edit --entity task:42"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"end","aliases":[],"hidden":false,"deprecated":null,"path":["action","end"],"command":"planar-agent action end","summary":"Close a nested action started under a claim.","description":"Close a nested action started under a claim.","subcommands":[],"flags":[{"long":"--action","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"N","default":null,"description":"Action id returned by `action start`","completion":{"kind":"none","values":[]},"env":null},{"long":"--outcome","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"ok","description":"ok | error | aborted | timeout (default ok)","completion":{"kind":"none","values":[]},"env":null},{"long":"--summary","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Optional free-text summary recorded on the action","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent action end --action 17 --outcome ok --summary \"Edited the handler\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"ingest","aliases":[],"hidden":false,"deprecated":null,"path":["ingest"],"command":"planar-agent ingest","summary":"Translate a vendor hook event into store primitives (claude + copilot adapters wired; codex reserved).","description":"Translate a vendor hook event into store primitives (claude + copilot adapters wired; codex reserved).","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Vendor tag (claude|copilot wired; codex reserved)","completion":{"kind":"none","values":[]},"env":null},{"long":"--event","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Event JSON: @<file> reads from path; @- reads from stdin","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent ingest --vendor claude --event @event.json"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"reconcile","aliases":[],"hidden":false,"deprecated":null,"path":["reconcile"],"command":"planar-agent reconcile","summary":"Operator recovery: mark expired claims stale, close orphaned actions, abandon dead runs.","description":"Operator recovery: mark expired claims stale, close orphaned actions, abandon dead runs.","subcommands":[],"flags":[{"long":"--dry-run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Report candidates without writing","completion":{"kind":"none","values":[]},"env":null},{"long":"--stale-after","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"0","description":"Additional grace beyond lease expiry (default 0s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)","completion":{"kind":"none","values":[]},"env":null},{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":0,"description":"Scope the sweep to a single session id (0 = global sweep, the default)","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Scope the sweep to claims/actions/runs belonging to this plan id (0 or absent = global sweep)","completion":{"kind":"none","values":[]},"env":null},{"long":"--category","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["usage_limit","context_limit","output_limit","tool_failure","validation","unknown"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":null,"description":"Optional closed failure category applied to claims made stale","completion":{"kind":"none","values":[]},"env":null},{"long":"--override-supervisor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Also reconcile engine-supervised claims and centurion runs (skipped by default; logged as supervisor_override)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent reconcile --dry-run","planar-agent reconcile --stale-after 2h"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"abort","aliases":[],"hidden":false,"deprecated":null,"path":["abort"],"command":"planar-agent abort","summary":"Operator force-release of a stuck claim (any session, not just the owner).","description":"Operator force-release of a stuck claim (any session, not just the owner).","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token to force-release","completion":{"kind":"none","values":[]},"env":null},{"long":"--reason","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Optional reason recorded on the claim and audit row","completion":{"kind":"none","values":[]},"env":null},{"long":"--category","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"choice","choices":["usage_limit","context_limit","output_limit","tool_failure","validation","unknown"],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":null,"description":"Optional closed failure category for the recovered claim","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":"planar-agent","description":"Vendor tag for the aborting session","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor-session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor session id for the aborting session","completion":{"kind":"none","values":[]},"env":null},{"long":"--override-supervisor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Abort an engine-supervised claim (refused otherwise; logged as supervisor_override)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent abort --claim 68fba76b319bcba1772f6e40a9b76f75 --reason \"Operator stopped the session\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"schema","aliases":[],"hidden":false,"deprecated":null,"path":["schema"],"command":"planar-agent schema","summary":"Print the full command tree as a JSON catalog (flags, aliases, positionals).","description":"Print the full command tree as a JSON catalog (flags, aliases, positionals).","subcommands":[],"flags":[{"long":"--command","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Emit only this command's catalog object, by full path (\"planar task update\") or relative to the root (\"task update\"); an unknown path exits 2 with nothing on stdout","completion":{"kind":"none","values":[]},"env":null},{"long":"--compact","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit one {command, summary} row per command instead of the full catalog; with --command, only that command's row","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"run","aliases":[],"hidden":false,"deprecated":null,"path":["run"],"command":"planar-agent run","summary":"Workflow run lifecycle (start / end / heartbeat). Used by an external workflow harness to stay DB-handle-free.","description":"Workflow run lifecycle (start / end / heartbeat). Used by an external workflow harness to stay DB-handle-free.","subcommands":["start","end","heartbeat"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"start","aliases":[],"hidden":false,"deprecated":null,"path":["run","start"],"command":"planar-agent run start","summary":"Insert a workflow_runs row in running status.","description":"Insert a workflow_runs row in running status.","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Plan id the run belongs to","completion":{"kind":"none","values":[]},"env":null},{"long":"--workflow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Workflow name (e.g. isolated-sequential)","completion":{"kind":"none","values":[]},"env":null},{"long":"--run-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Unique run identifier (run-<pid>-<nanos>)","completion":{"kind":"none","values":[]},"env":null},{"long":"--pid","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"PID of the external workflow harness process. Exactly one of --pid / --ttl is required: a pid-bound run is probed for liveness by reconcile; a pid-less run needs --ttl instead","completion":{"kind":"none","values":[]},"env":null},{"long":"--ttl","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Lease TTL for a pid-less run (accepts bare int seconds or suffixed duration: 10m, 1h, 500ms), extended by `run heartbeat` and enforced by reconcile. Required when --pid is omitted","completion":{"kind":"none","values":[]},"env":null},{"long":"--repo-root","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Absolute path of the repo root the harness is driving","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent run start --plan 7 --workflow build --run-id run-1a2b3c --repo-root . --ttl 10m"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"end","aliases":[],"hidden":false,"deprecated":null,"path":["run","end"],"command":"planar-agent run end","summary":"Close a workflow_runs row with a terminal status (completed|failed|interrupted).","description":"Close a workflow_runs row with a terminal status (completed|failed|interrupted).","subcommands":[],"flags":[{"long":"--run-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"The run identifier (run-<pid>-<nanos>) returned by run start","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Terminal status: completed | failed | interrupted","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent run end --run-id run-1a2b3c --status completed"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"heartbeat","aliases":[],"hidden":false,"deprecated":null,"path":["run","heartbeat"],"command":"planar-agent run heartbeat","summary":"Extend a pid-less run's lease (expires_at).","description":"Extend a pid-less run's lease (expires_at).","subcommands":[],"flags":[{"long":"--run-id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"The run identifier (run-<pid>-<nanos>) returned by run start","completion":{"kind":"none","values":[]},"env":null},{"long":"--ttl","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"New lease TTL, set absolutely from now (accepts bare int seconds or suffixed duration: 10m, 1h, 500ms). Refuses on a pid-supervised run or a run no longer `running`","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent run heartbeat --run-id run-1a2b3c --ttl 10m"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"dispatch","aliases":[],"hidden":false,"deprecated":null,"path":["dispatch"],"command":"planar-agent dispatch","summary":"Routing dispatch authorization (preview / confirm). Binds current state to a single-use token.","description":"Routing dispatch authorization (preview / confirm). Binds current state to a single-use token.","subcommands":["preview","confirm"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"preview","aliases":[],"hidden":false,"deprecated":null,"path":["dispatch","preview"],"command":"planar-agent dispatch preview","summary":"Bind current routing state to a single-use, expiry-bound confirmation token.","description":"Bind current routing state to a single-use, expiry-bound confirmation token.","subcommands":[],"flags":[{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Task id this dispatch targets","completion":{"kind":"none","values":[]},"env":null},{"long":"--work-item","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Logical work item id","completion":{"kind":"none","values":[]},"env":null},{"long":"--project","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Project id","completion":{"kind":"none","values":[]},"env":null},{"long":"--validation-policy","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Validation policy version","completion":{"kind":"none","values":[]},"env":null},{"long":"--routing-policy","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Routing policy version","completion":{"kind":"none","values":[]},"env":null},{"long":"--profile-rule","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Profile rule version","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Vendor (opaque)","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Role (opaque)","completion":{"kind":"none","values":[]},"env":null},{"long":"--tier","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"small|medium|large","completion":{"kind":"none","values":[]},"env":null},{"long":"--work-type","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"schema|engine|architectural|cli|feature|mechanical","completion":{"kind":"none","values":[]},"env":null},{"long":"--complexity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"bounded|standard|high-risk","completion":{"kind":"none","values":[]},"env":null},{"long":"--packet-digest","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Digest of the authoritative packet","completion":{"kind":"none","values":[]},"env":null},{"long":"--profile-digest","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Digest of the compiled profile","completion":{"kind":"none","values":[]},"env":null},{"long":"--policy-digest","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Digest of the policy snapshot","completion":{"kind":"none","values":[]},"env":null},{"long":"--capability-digest","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Digest of the host capability snapshot","completion":{"kind":"none","values":[]},"env":null},{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Requested candidate row id","completion":{"kind":"none","values":[]},"env":null},{"long":"--host","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Host id whose capability snapshot was consulted","completion":{"kind":"none","values":[]},"env":null},{"long":"--class","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"fallback|default|override|declared_experiment","completion":{"kind":"none","values":[]},"env":null},{"long":"--experiment","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Experiment id (required for declared_experiment)","completion":{"kind":"none","values":[]},"env":null},{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Claim token this dispatch is bound to","completion":{"kind":"none","values":[]},"env":null},{"long":"--claim-status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Claim status observed at preview time","completion":{"kind":"none","values":[]},"env":null},{"long":"--evidence-state","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"evidential|observational","completion":{"kind":"none","values":[]},"env":null},{"long":"--expires-at","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"RFC3339 instant after which the token is dead","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent dispatch preview --work-item 42 --project 1 --validation-policy default --routing-policy default --profile-rule rule-1 --vendor claude --role coder --tier medium --work-type feature --complexity standard --packet-digest sha256:aa --profile-digest sha256:bb --policy-digest sha256:cc --capability-digest sha256:dd --candidate 3 --host devbox --class default --evidence-state evidential --expires-at 2026-10-05T09:00:00Z"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"confirm","aliases":[],"hidden":false,"deprecated":null,"path":["dispatch","confirm"],"command":"planar-agent dispatch confirm","summary":"Revalidate a preview token against current state and atomically write the dispatch snapshot.","description":"Revalidate a preview token against current state and atomically write the dispatch snapshot.","subcommands":[],"flags":[{"long":"--token","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Preview token to spend","completion":{"kind":"none","values":[]},"env":null},{"long":"--dispatch-key","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Unique key for the resulting dispatch","completion":{"kind":"none","values":[]},"env":null},{"long":"--now","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"RFC3339 instant to evaluate expiry against","completion":{"kind":"none","values":[]},"env":null},{"long":"--packet-digest","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently observed packet digest","completion":{"kind":"none","values":[]},"env":null},{"long":"--profile-digest","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently observed profile digest","completion":{"kind":"none","values":[]},"env":null},{"long":"--policy-digest","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently observed policy digest","completion":{"kind":"none","values":[]},"env":null},{"long":"--capability-digest","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently observed capability digest","completion":{"kind":"none","values":[]},"env":null},{"long":"--candidate","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently resolved candidate row id","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently resolved vendor","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently resolved role","completion":{"kind":"none","values":[]},"env":null},{"long":"--tier","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently resolved tier","completion":{"kind":"none","values":[]},"env":null},{"long":"--work-type","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently resolved work type","completion":{"kind":"none","values":[]},"env":null},{"long":"--complexity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently resolved complexity","completion":{"kind":"none","values":[]},"env":null},{"long":"--validation-policy","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently active validation policy version","completion":{"kind":"none","values":[]},"env":null},{"long":"--routing-policy","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Currently active routing policy version","completion":{"kind":"none","values":[]},"env":null},{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Currently held claim token","completion":{"kind":"none","values":[]},"env":null},{"long":"--claim-status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Currently observed claim status","completion":{"kind":"none","values":[]},"env":null},{"long":"--reviewer","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Reviewer disposition to record (default required)","completion":{"kind":"none","values":[]},"env":null},{"long":"--decision","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"confirmed|overridden (default confirmed)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent dispatch confirm --token tok-1 --dispatch-key key-1 --now 2026-10-04T09:00:00Z --packet-digest sha256:aa --profile-digest sha256:bb --policy-digest sha256:cc --capability-digest sha256:dd --candidate 3 --vendor claude --role coder --tier medium --work-type feature --complexity standard --validation-policy default --routing-policy default"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"context","aliases":[],"hidden":false,"deprecated":null,"path":["context"],"command":"planar-agent context","summary":"Run-scoped working-memory records (add / list / resolve / capsule). Used by an external workflow harness.","description":"Run-scoped working-memory records (add / list / resolve / capsule). Used by an external workflow harness.","subcommands":["add","capsule","list","resolve"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"add","aliases":[],"hidden":false,"deprecated":null,"path":["context","add"],"command":"planar-agent context add","summary":"Write a context_records row stamped from the claim's run_id, stage, session_id.","description":"Write a context_records row stamped from the claim's run_id, stage, session_id.","subcommands":[],"flags":[{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Claim token that owns this context record","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Record kind: finding|risk|artifact|followup|summary|capsule","completion":{"kind":"none","values":[]},"env":null},{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Record body text","completion":{"kind":"none","values":[]},"env":null},{"long":"--compiled-from","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Comma-separated context_record ids this capsule was compiled from (capsule kind only)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent context add --claim 68fba76b319bcba1772f6e40a9b76f75 --kind finding --body \"Backoff cap is 30 seconds\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"capsule","aliases":[],"hidden":false,"deprecated":null,"path":["context","capsule"],"command":"planar-agent context capsule","summary":"Write a compiled capsule context_records row, run-keyed (claim_id=NULL, decision 456).","description":"Write a compiled capsule context_records row, run-keyed (claim_id=NULL, decision 456).","subcommands":[],"flags":[{"long":"--run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"workflow_runs.id — the run that owns this capsule","completion":{"kind":"none","values":[]},"env":null},{"long":"--stage","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Stage name this capsule distills (e.g. 'plan', 'code')","completion":{"kind":"none","values":[]},"env":null},{"long":"--body","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Compiled capsule body text","completion":{"kind":"none","values":[]},"env":null},{"long":"--compiled-from","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Comma-separated context_record ids this capsule distills (provenance)","completion":{"kind":"none","values":[]},"env":null},{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"session_id (integer). Optional — an ephemeral session is created when omitted.","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent context capsule --run 3 --stage build --body \"Build stage notes\""],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["context","list"],"command":"planar-agent context list","summary":"List context_records for a run, with optional stage/status/kind filters.","description":"List context_records for a run, with optional stage/status/kind filters.","subcommands":[],"flags":[{"long":"--run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Run id (integer) to query","completion":{"kind":"none","values":[]},"env":null},{"long":"--stage","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter to records from this stage","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by status: active|consumed|superseded","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by kind: finding|risk|artifact|followup|summary|capsule","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent context list --run 3","planar-agent context list --run 3 --status open --json"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"resolve","aliases":[],"hidden":false,"deprecated":null,"path":["context","resolve"],"command":"planar-agent context resolve","summary":"Transition context_records active → consumed|superseded (single record or bulk stage sweep).","description":"Transition context_records active → consumed|superseded (single record or bulk stage sweep).","subcommands":[],"flags":[{"long":"--id","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Single record id to transition","completion":{"kind":"none","values":[]},"env":null},{"long":"--run","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Run id for bulk stage sweep (use with --stage)","completion":{"kind":"none","values":[]},"env":null},{"long":"--stage","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Stage name for bulk sweep (use with --run)","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":true,"source":"local","valueName":"VALUE","default":null,"description":"Target status: consumed|superseded","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit machine-readable JSON instead of text","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent context resolve --id 5 --status consumed"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":7,"meaning":"The database schema is behind or ahead of this binary."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"queue","aliases":[],"hidden":false,"deprecated":null,"path":["queue"],"command":"planar-agent queue","summary":"Host-wide build and test queue (run a command in turn).","description":"Host-wide build and test queue (run a command in turn).","subcommands":["run","cancel","status","wait","rule"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"run","aliases":[],"hidden":false,"deprecated":null,"path":["queue","run"],"command":"planar-agent queue run","summary":"Run a command in turn, host-wide: wait for the command's turn in the queue, run it in the caller's directory with the caller's environment, and exit with its status.","description":"Run a command in turn, host-wide: wait for the command's turn in the queue, run it in the caller's directory with the caller's environment, and exit with its status.","subcommands":[],"flags":[{"long":"--detach","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Return a ticket at once and let a detached submitter wait, run the command and record its history: print the sequence number and the path of the output file <planar-db-directory>/queue-logs/<seq>.log, one per line, and exit 0; exit 125 when no ticket could be issued","completion":{"kind":"none","values":[]},"env":null},{"long":"--timeout","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"How long the command may run before it is stopped (SIGTERM, then SIGKILL after the grace period): an integer and a unit, ms, s, m or h, at most 24h; default 30m","completion":{"kind":"none","values":[]},"env":null},{"long":"--wait-timeout","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"How long the entry may wait for its turn before it is removed without running: an integer and a unit, ms, s, m or h, at most 24h; default no limit","completion":{"kind":"none","values":[]},"env":null},{"long":"--label","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Operator-facing label recorded with the queue entry","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The submitting agent's vendor, recorded with the queue entry; default $PLANAR_VENDOR, empty when neither is set","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The submitting agent's role, recorded with the queue entry; default $PLANAR_ROLE, empty when neither is set","completion":{"kind":"none","values":[]},"env":null},{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Renew this claim at half its lease interval while the entry waits and while the command runs; a renewal that fails is reported on standard error (or in the output file) and never stops the command","completion":{"kind":"none","values":[]},"env":null},{"long":"--notices","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Write the entry's queue position, start and final outcome to standard error; standard output is never touched","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"command","kind":"string","required":true,"default":null,"description":"The command and its arguments, after `--`","completion":{"kind":"none","values":[]}}],"docs":{"examples":["planar-agent queue run -- make test","planar-agent queue run --detach --vendor claude --role coder -- make test"],"exitCodes":[{"code":0,"meaning":"The command exited 0. Any other status of the command passes through unchanged, as 128 plus N when signal N ended it."},{"code":1,"meaning":"A usage error; the command was not run."},{"code":2,"meaning":"Refused before queueing: the command is a model launcher or --timeout is invalid."},{"code":124,"meaning":"The command was stopped at its run limit (--timeout)."},{"code":125,"meaning":"The queue failed: planar.db is unreachable or incompatible, the wait limit passed, the entry was cancelled, or an internal error."},{"code":126,"meaning":"The command was found but could not be executed."},{"code":127,"meaning":"The command was not found."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"cancel","aliases":[],"hidden":false,"deprecated":null,"path":["queue","cancel"],"command":"planar-agent queue cancel","summary":"Cancel a queue entry: remove a waiting one, or stop a running one (SIGTERM to its command's process group, then SIGKILL after the grace period if the group still has members) and wait until its group is empty.","description":"Cancel a queue entry: remove a waiting one, or stop a running one (SIGTERM to its command's process group, then SIGKILL after the grace period if the group still has members) and wait until its group is empty.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The cancelling agent's vendor, recorded as who cancelled; default $PLANAR_VENDOR, empty when neither is set","completion":{"kind":"none","values":[]},"env":null},{"long":"--role","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"The cancelling agent's role, recorded as who cancelled; default $PLANAR_ROLE, empty when neither is set","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"seq","kind":"string","required":true,"default":null,"description":"The entry's sequence number, as `queue run --detach` and the notices name it","completion":{"kind":"none","values":[]}}],"docs":{"examples":["planar-agent queue cancel 1000001"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"No such entry, or a usage error."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":6,"meaning":"The entry has already ended."},{"code":125,"meaning":"The queue failed: planar.db is unreachable or incompatible, the wait limit passed, the entry was cancelled, or an internal error."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"status","aliases":[],"hidden":false,"deprecated":null,"path":["queue","status"],"command":"planar-agent queue status","summary":"Report what became of a queue entry: its state and place in the queue, how it ended, the exit status, where its output was saved and, for a cancelled entry, who cancelled it. Reads only; changes nothing.","description":"Report what became of a queue entry: its state and place in the queue, how it ended, the exit status, where its output was saved and, for a cancelled entry, who cancelled it. Reads only; changes nothing.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Print one JSON object with the documented fields, null where a field does not apply","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"seq","kind":"string","required":true,"default":null,"description":"The sequence number, the first line of the ticket `queue run --detach` prints","completion":{"kind":"none","values":[]}}],"docs":{"examples":["planar-agent queue status 1000001","planar-agent queue status 1000001 --json"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"No such entry, or a usage error."},{"code":2,"meaning":"Bad input: an invalid value or entity ref."},{"code":125,"meaning":"The queue failed: planar.db is unreachable or incompatible, the wait limit passed, the entry was cancelled, or an internal error."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"wait","aliases":[],"hidden":false,"deprecated":null,"path":["queue","wait"],"command":"planar-agent queue wait","summary":"Observe one logical queue ticket until recorded completion or a finite observer deadline; changes nothing.","description":"Observe one logical queue ticket until recorded completion or a finite observer deadline; changes nothing.","subcommands":[],"flags":[{"long":"--timeout","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Observer budget from before store open: positive integer with ms, s, m or h; default 30m, maximum 24h","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Print one result object with wait_reason, recorded status and error fields","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"seq","kind":"string","required":true,"default":null,"description":"Positive sequence number printed by `queue run --detach`; successors are followed","completion":{"kind":"none","values":[]}}],"docs":{"examples":["planar-agent queue wait 1000001 --timeout 3h --json"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"Ticket history is unavailable, or a usage error."},{"code":2,"meaning":"Invalid sequence or observation timeout."},{"code":124,"meaning":"Observer deadline expired, or recorded command timeout or exit 124; inspect wait_reason and status."},{"code":125,"meaning":"Observation failed or stalled, or recorded command exit 125, cancellation or wait timeout; inspect wait_reason and status."},{"code":126,"meaning":"The command was found but could not be executed."},{"code":127,"meaning":"The command was not found."},{"code":130,"meaning":"Observation was interrupted by SIGINT."},{"code":143,"meaning":"Observation was interrupted by SIGTERM."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"rule","aliases":[],"hidden":false,"deprecated":null,"path":["queue","rule"],"command":"planar-agent queue rule","summary":"Print the rule text that tells an agent to send builds and tests through the queue, for pasting into a project's own agent guide. Opens no database.","description":"Print the rule text that tells an agent to send builds and tests through the queue, for pasting into a project's own agent guide. Opens no database.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":["planar-agent queue rule"],"exitCodes":[{"code":0,"meaning":"Success."},{"code":1,"meaning":"A usage error."}],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}}]}
)CATALOG");
}

TEST_CASE("planar-agent parity: the catalog comparison actually discriminates", "[cmd][agent][parity][catalog][break-probe]") {
  // Break-probe in permanent form. `diff_against_oracle` returning an empty
  // vector is the pass condition above, and a function that always returned
  // one would pass just as happily. Each mutation below is a transcription
  // slip the deleted help-page diff used to catch, or a field task 6065
  // added to the comparison; each must produce at least one problem.
  using planar::cmd::parity::catalog_map;
  using planar::cmd::parity::command_surface;
  using planar::cmd::parity::diff_against_oracle;
  using planar::cmd::parity::flag_surface;
  using planar::cmd::parity::oracle_only_commands;
  using planar::cmd::parity::positional_surface;

  auto const flag = [](std::string name, bool required) {
    return flag_surface{.name = name, .rendered = std::format("kind=\"string\" long=\"{}\" required={}", name, required)};
  };

  catalog_map const oracle{
      {"planar-agent fail",
       command_surface{.summary     = "Fail a claim.",
                       .description = "Fail a claim.",
                       .flags = {flag("--category", false), flag("--claim", true), flag("--json", false), flag("--reason", true)},
                       .positionals = {},
                       .subcommands = {}}},
      {"planar-agent", command_surface{.subcommands = {"fail", "pull"}}},
  };
  auto faithful = oracle;

  SECTION("the unmutated fixture produces no problems") {
    // The control. Without it every CHECK_FALSE below could pass because
    // the comparison reports a problem for EVERYTHING.
    CHECK(diff_against_oracle(faithful, oracle).empty());
    CHECK(oracle_only_commands(faithful, oracle).empty());
  }
  SECTION("a dropped flag is caught") {
    auto mutated = faithful;
    mutated.at("planar-agent fail").flags.pop_back();
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("a flag that lost its required-ness is caught") {
    auto mutated                             = faithful;
    mutated.at("planar-agent fail").flags[1] = flag("--claim", false);
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("flags in a DIFFERENT ORDER are caught") {
    // New at task 6065: the comparison used to sort both sides, so a
    // reordered flag list was invisible. CLI11 renders help in insertion
    // order, so it is not.
    auto mutated = faithful;
    std::ranges::reverse(mutated.at("planar-agent fail").flags);
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("a flag field OTHER than long/required is caught") {
    // Also new at task 6065: `kind`, `choices`, `short`, `aliases`,
    // `description`, `valueName`, `list`, `count`, `hidden`, `deprecated`,
    // `env` and `completion` are all compared now.
    auto mutated                                      = faithful;
    mutated.at("planar-agent fail").flags[0].rendered = "kind=\"int\" long=\"--category\" required=false";
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("an invented positional is caught") {
    auto mutated = faithful;
    mutated.at("planar-agent fail").positionals.push_back({.name = "task-id", .rendered = "required=true"});
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("a summary that drifted from the oracle's is caught") {
    auto mutated                            = faithful;
    mutated.at("planar-agent fail").summary = "Something else entirely.";
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("a subcommand the oracle does not have is caught") {
    auto mutated                                  = faithful;
    mutated.at("planar-agent").subcommands.back() = "invented";
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("subcommands in a DIFFERENT ORDER are caught") {
    auto mutated = faithful;
    std::ranges::reverse(mutated.at("planar-agent").subcommands);
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("a command the oracle does not have at all is caught") {
    auto mutated = faithful;
    mutated.emplace("planar-agent nosuchverb", command_surface{});
    CHECK_FALSE(diff_against_oracle(mutated, oracle).empty());
  }
  SECTION("a command the oracle HAS and the tree does not is caught") {
    // The 6065 direction. `diff_against_oracle` is blind to it by
    // construction — it only walks the C++ side — so a subset would pass
    // it forever. This is the half that makes full-surface parity an
    // assertion.
    auto mutated = faithful;
    mutated.erase("planar-agent fail");
    CHECK(diff_against_oracle(mutated, oracle).empty());
    auto const missing = oracle_only_commands(mutated, oracle);
    REQUIRE(missing.size() == 1);
    CHECK(missing.front() == "planar-agent fail");
  }
}

TEST_CASE("planar-agent parity: version diverges only in the runtime tag", "[cmd][agent][parity]") {
  // TASK 6539 RETIREMENT. This case's whole subject was already a
  // DIVERGENCE assertion, not an equality one, so retiring the oracle read
  // means pinning the C++ side's fixed fields rather than the comparison
  // itself — the oracle's half is NOT pinned here (see below for where it
  // still lives). Transcribed from a run in which the oracle agreed on
  // fields 0-2 and diverged only on field 3 (`zig` vs `cxx`) and field 4
  // (`0.16.0` vs a Clang version string), confirmed before this rewrite
  // (commit 4fc09c0777cb0456e8ef52889dc7ca7f44a93f34).
  //
  // Fields 1 and 2 are `dev`/`dev`: the version-meta-off sentinel this repo
  // always builds with for tests (`PLANAR_VERSION_META` must never be set —
  // see Build And Test in CLAUDE.md), so pinning them literally is pinning
  // this test suite's own build contract, not an incidental value. Field 4
  // is the pinned toolchain's compiler string (`compiler_version_string()`,
  // `docs/toolchain-parity.md`), deterministic for a build against that
  // exact toolchain.
  //
  // Runs without the oracle — it is an assertion about this binary, not a
  // comparison — so it stays live on a checkout with no zig/ build.
  auto const arena = make_arena("version");
  auto const cpp   = run_pinned(cpp_bin(), std::vector<std::string>{"version"}, arena.cpp_root, "version");
  CHECK(cpp.code == 0);
  CHECK(cpp.err.empty());

  // Field 4 is `compiler_version_string()`'s output (`src/lib/cliapp/version.cpp`):
  // a TOOLCHAIN fact, not a contract of this binary or this test suite. Task
  // 6706: the literal `Clang-23.1.0` used to be transcribed here directly,
  // which meant every `brew upgrade llvm` PATCH bump broke `make test` for
  // everyone on the host with no source change at fault. Deriving the
  // expected value from the SAME compiler-identification macros
  // `compiler_version_string()` itself switches on keeps the invariant that
  // is actually worth pinning — this test binary and the `cpp` binary under
  // test are always built by the identical compiler invocation, so the two
  // derivations must agree — while decoupling the assertion from a version
  // number that changes independently of any code in this tree. This is
  // NOT a loosening of `docs/toolchain-parity.md`'s toolchain pin: that pin
  // is enforced by `CMakePresets.json` selecting the exact compiler binary,
  // untouched here. This assertion is about what the BUILT BINARY emits
  // given whichever pinned compiler built it, not about relaxing which
  // compiler is allowed to build it.
#if defined(__clang__)
  auto const expected_compiler = std::string{"Clang-"} + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) +
                                 "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
  auto const expected_compiler = std::string{"GCC-"} + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
                                 std::to_string(__GNUC_PATCHLEVEL__);
#else
  auto const expected_compiler = std::string{"unknown"};
#endif

  // Pinned EXACTLY, modulo the derived compiler field above.
  CHECK(cpp.out == "planar-agent dev dev cxx " + expected_compiler + "\n");

  // AND the field-shape invariant `planar.cliapp.version`'s header makes,
  // restated as an assertion about the built binary alone. Task 6117: this
  // used to require SIX fields, because `compiler_version_string()` used
  // to return `Clang 22.1.8` with a space of its own; the separator is a
  // hyphen now, so field 4 never contains a space.
  auto const split = [](std::string_view line) {
    std::vector<std::string> fields;
    for (auto const part : std::views::split(line, ' ')) {
      fields.emplace_back(std::string_view{part});
    }
    return fields;
  };
  auto const cpp_fields = split(std::string_view{cpp.out}.substr(0, cpp.out.size() - 1));
  REQUIRE(cpp_fields.size() == 5);
  CHECK(cpp_fields[0] == "planar-agent");
  CHECK(cpp_fields[1] == "dev");
  CHECK(cpp_fields[2] == "dev");
  CHECK(cpp_fields[3] == "cxx");
  CHECK(cpp_fields[4] == expected_compiler);
  CHECK_FALSE(cpp_fields[4].contains(' '));
}

TEST_CASE("planar-agent parity: no ported invocation creates a database", "[cmd][agent][parity][safety]") {
  // TASK 6539: this case never actually read the oracle — it only ever ran
  // `cpp_bin()` — so the `PLANAR_REQUIRE_ORACLE` gate was pure overhead: an
  // oracle-conditional skip on a case with no oracle dependency at all.
  // Dropped outright; nothing else about the case changes.
  //
  // The consumer policy's most operator-visible consequence, checked
  // end-to-end rather than at the context level: none of the ported verbs
  // opens SQLite, so `$PLANAR_DB` must not exist afterwards. This is also
  // the assertion that would catch an eager open added to `main`.
  auto const arena = make_arena("nodb");
  for (auto const& argv : std::vector<std::vector<std::string>>{{"version"}, {"schema"}, {"--help"}, {"nosuchverb"}}) {
    auto const tag = argv.front();
    (void)run_pinned(cpp_bin(), argv, arena.cpp_root, tag);
    INFO("argv: " << tag);
    CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "planar.db"));
  }
}

// ===========================================================================
// The claim ritual, pinned end-to-end (TASK 6539: no longer diffed live)
// ===========================================================================
//
// Everything above compares (or, post-6539, pins) invocations that touch no
// database. These two cases walk the verbs that ARE the database, seeded
// with the C++ `planar` operator binary (`cpp_planar_bin()` above — task
// 6539 moved this off the Zig oracle's `planar` once the C++ port grew
// `init` / `plan create` / `task add`), and pin the resulting bytes after
// two kinds of NORMALISATION that would otherwise vary run to run:
//
//   the 32-hex claim token (minted by `randomblob`), the millisecond
//   timestamps, and the scratch root path. Everything else — every status,
//   every count, every field name, every ORDER — is pinned literally.
//
// Normalising the token is not a weakening: `claim`'s own `--help` calls
// it opaque, and its SHAPE is pinned in agentactivity.t.cpp. What these
// cases are for is the surrounding structure.
//
// Both cases were confirmed to pass as LIVE diffs against the Zig oracle,
// on both the ritual script (45 steps) and the state dump (5 queries),
// before this rewrite — at commit 4fc09c0777cb0456e8ef52889dc7ca7f44a93f34.
// The pinned bytes below are transcribed from that run.

namespace {

/// @brief The C++ OPERATOR binary, beside the agent binary these two cases
/// pin. Used only to seed the fixture.
///
/// TASK 6539: this used to be the Zig oracle's `planar`, because at the
/// time this file was written the C++ `planar` port did not yet carry
/// `init` / `plan create` / `task add`. It does now (plan 996 is mid
/// cutover, not pre-cutover), and retiring the oracle read from the two
/// cases below means the FIXTURE can no longer depend on a Zig binary
/// either — a case that "runs without the oracle" but still shells
/// `zig/zig-out/bin/planar` to build its own fixture has not actually
/// stopped needing `zig/`. Confirmed to produce the identical seeded shape
/// (same plan id, same task ids, same statuses) as the Zig seed it
/// replaces, at commit 4fc09c0777cb0456e8ef52889dc7ca7f44a93f34.
/// @return The path.
auto cpp_planar_bin() -> std::filesystem::path {
  return cpp_bin().parent_path() / "planar";
}

/// @brief Replace the fields that cannot match across two arenas.
///
/// 32-hex runs become `<TOKEN>`, `…T…Z` instants become `<TS>`, and any
/// occurrence of either arena root becomes `<ROOT>`. Nothing else is
/// touched — in particular no status, count, key name or ordering is.
/// @param text The captured output.
/// @param root The arena root to elide.
/// @return The normalised text.
auto normalise(std::string_view text, const std::filesystem::path& root) -> std::string {
  static std::regex const token_re{"[0-9a-f]{32}"};
  static std::regex const stamp_re{"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]+Z"};
  std::string             out = std::regex_replace(std::string{text}, token_re, "<TOKEN>");
  out                         = std::regex_replace(out, stamp_re, "<TS>");
  // LONGEST FORM FIRST. On this platform `/tmp` is a symlink to
  // `/private/tmp`, and the locality probe records the CANONICAL path, so
  // both spellings occur. Replacing the short form first would leave
  // `/private<ROOT>` behind and turn a match into a spurious diff.
  for (auto const& form : {"/private" + root.string(), root.string()}) {
    for (std::size_t at = out.find(form); at != std::string::npos; at = out.find(form, at + 6)) {
      out.replace(at, form.size(), "<ROOT>");
    }
  }
  return out;
}

/// @brief Seed one arena with a project, a plan and `task_count` tasks,
/// using the C++ operator binary.
/// @param root The arena root.
/// @param task_count How many tasks to create.
auto seed_arena(const std::filesystem::path& root, int task_count) -> void {
  auto const seed_root = root.string();
  (void)run_pinned(cpp_planar_bin(), std::array<std::string, 1>{"init"}, root, "seed-init");
  (void)run_pinned(cpp_planar_bin(), std::array<std::string, 5>{"assoc", "create", "project:proj", "--kind", "project"}, root,
                   "seed-assoc");
  (void)run_pinned(cpp_planar_bin(), std::array<std::string, 4>{"assoc", "add", "project:proj", (root / "proj").string()}, root,
                   "seed-add");
  (void)run_pinned(cpp_planar_bin(), std::array<std::string, 3>{"plan", "create", "Test plan"}, root, "seed-plan");
  for (int i = 0; i < task_count; ++i) {
    (void)run_pinned(cpp_planar_bin(), std::array<std::string, 5>{"task", "add", std::format("task {}", i), "--plan", "1"}, root,
                     std::format("seed-task-{}", i));
  }
}

/// @brief One step of a scripted comparison. `@TOKEN<n>` in an argument is
/// replaced, per arena, with claim id `<n>`'s token from THAT arena's
/// database — the tokens differ by construction, so a literal script
/// could not address them.
struct step {
  std::string_view         tag;
  std::vector<std::string> args;
};

/// @brief Read claim `<id>`'s token out of an arena's database.
/// @param root The arena root.
/// @param claim_id The claim row id.
/// @return The token, or empty when absent.
auto token_for(const std::filesystem::path& root, int claim_id) -> std::string {
  auto conn = planar::db::connection::open((root / "planar.db").string());
  if (!conn) {
    return {};
  }
  auto stmt = conn->prepare(std::format("select claim_token from agent_work_claims where id = {}", claim_id));
  if (!stmt) {
    return {};
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != planar::db::step_result::row) {
    return {};
  }
  return stmt->column_text(0);
}

/// @brief Evaluate a scalar-integer (or boolean-as-integer) query against
/// an arena's database. Used by the dependency-roll-up scenario below to
/// assert on row state a text/JSON transcript would make unreadable.
/// @param root The arena root.
/// @param sql The query; must return exactly one row, one integer column.
/// @return The scalar, or `-1` if the query/row is absent (a REQUIRE at
/// the call site turns that into a clear failure rather than a silent 0).
auto scalar_int_state(const std::filesystem::path& root, std::string_view sql) -> std::int64_t {
  auto conn = planar::db::connection::open((root / "planar.db").string());
  if (!conn) {
    return -1;
  }
  auto stmt = conn->prepare(sql);
  if (!stmt) {
    return -1;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != planar::db::step_result::row) {
    return -1;
  }
  return stmt->column_int64(0);
}

/// @brief Substitute `@TOKEN<n>` placeholders against one arena.
/// @param args The scripted arguments.
/// @param root The arena root.
/// @return The concrete arguments.
auto resolve(std::span<const std::string> args, const std::filesystem::path& root) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(args.size());
  for (auto const& arg : args) {
    if (arg.starts_with("@TOKEN")) {
      out.push_back(token_for(root, std::stoi(arg.substr(6))));
    } else {
      out.push_back(arg);
    }
  }
  return out;
}

} // namespace

TEST_CASE("planar-agent parity: the claim ritual, scripted end to end", "[cmd][agent][parity][claims]") {
  // TASK 6539 RETIREMENT. Every step below was confirmed to agree with the
  // Zig oracle -- same exit code, same normalised stdout, same normalised
  // stderr -- before this rewrite, at commit
  // 4fc09c0777cb0456e8ef52889dc7ca7f44a93f34. What follows pins the C++
  // side of that agreement, byte for byte after normalisation, rather than
  // reading the oracle live. `expected_out` / `expected_err` are
  // transcribed EXACTLY, trailing newline included, with the same three
  // substitutions `normalise()` always applied: `<TOKEN>` for the 32-hex
  // claim token, `<TS>` for a millisecond timestamp, and `<ROOT>` for the
  // arena root.
  //
  // Runs without the oracle — it is an assertion about this binary, not a
  // comparison — so it stays live on a checkout with no zig/ build.
  struct pinned_step {
    std::string_view         tag;
    std::vector<std::string> args;
    int                      expected_code;
    std::string_view         expected_out;
    std::string_view         expected_err;
  };

  auto const arena = make_arena("ritual");
  seed_arena(arena.cpp_root, 4);

  // The script walks a whole session the way an orchestrator would, and
  // then walks every refusal path off it. Order matters: each step's
  // output depends on the state the previous ones left.
  std::vector<pinned_step> const script{
      {"peek", {"peek", "1"}, 0, "next: task:1 status:todo\n", ""},
      {"peek-json",
       {"peek", "1", "--json"},
       0,
       "{\"ok\":true,\"no_work\":false,\"task\":{\"id\":1,\"scope_kind\":\"association\",\"scope_id\":1,\"plan_id\":1,\"parent_"
       "task_id\":null,\"title\":\"task "
       "0\",\"body\":null,\"slug\":null,\"status\":\"todo\",\"priority\":100,\"next_action\":null,\"due_at\":null,\"created_at\":"
       "\"<TS>\",\"updated_at\":\"<TS>\"}}\n",
       ""},
      {"pull", {"pull", "1"}, 0, "pulled task:1 claim:<TOKEN> action:1\n", ""},
      {"pull-json",
       {"pull", "1", "--json", "--role", "coder", "--purpose", "p", "--ttl", "30m"},
       0,
       "{\"ok\":true,\"no_work\":false,\"claim_token\":\"<TOKEN>\",\"claim\":{\"id\":2,\"claim_token\":\"<TOKEN>\",\"session_"
       "id\":1,\"entity_kind\":\"task\",\"entity_id\":2,\"claim_scope\":\"exclusive\",\"status\":\"active\",\"vendor\":\"planar-"
       "agent\",\"vendor_session_id\":null,\"role\":\"coder\",\"model\":null,\"worktree_id\":null,\"worktree_path\":null,\"repo_"
       "root\":\"<ROOT>/"
       "proj\",\"branch\":null,\"head_sha_at_claim\":null,\"dirty_at_claim\":\"unknown\",\"purpose\":\"p\",\"base_ref\":null,"
       "\"claimed_at\":\"<TS>\",\"last_heartbeat_at\":\"<TS>\",\"lease_expires_at\":\"<TS>\",\"released_at\":null,\"release_"
       "reason\":null,\"failure_category\":null,\"run_id\":null,\"stage\":null},\"task\":{\"id\":2,\"scope_kind\":"
       "\"association\",\"scope_id\":1,\"plan_id\":1,\"parent_task_id\":null,\"title\":\"task "
       "1\",\"body\":null,\"slug\":null,\"status\":\"doing\",\"priority\":100,\"next_action\":null,\"due_at\":null,\"created_"
       "at\":\"<TS>\",\"updated_at\":\"<TS>\"},\"action_id\":2}\n",
       ""},
      {"pull-badplan", {"pull", "999", "--json"}, 0, "{\"ok\":true,\"no_work\":true}\n", ""},
      {"pull-badttl",
       {"pull", "1", "--ttl", "zzz"},
       1,
       "",
       "error: invalid --ttl 'zzz': expected bare seconds (e.g. 600) or suffixed duration (e.g. 10m, 1h, 500ms)\n"},
      {"pull-badmeta", {"pull", "1", "--metadata", "{"}, 2, "", "error: --metadata is not valid JSON: {\n"},
      {"pull-stage-no-run",
       {"pull", "1", "--stage", "code"},
       2,
       "",
       "error: --stage requires --run: provide a workflow_runs.id via --run <id>\n"},
      {"pull-badparent", {"pull", "1", "--parent-action", "999"}, 1, "", "error: --parent-action 999: action not found\n"},
      {"pull-parent-zero",
       {"pull", "1", "--parent-action", "0"},
       2,
       "",
       "error: --parent-action must be a positive integer (got 0)\n"},
      {"heartbeat", {"heartbeat", "--claim", "@TOKEN1"}, 0, "ok claim:<TOKEN> expires:<TS>\n", ""},
      {"heartbeat-json",
       {"heartbeat", "--claim", "@TOKEN2", "--json", "--ttl", "1h", "--status", "editing"},
       0,
       "{\"ok\":true,\"claim_token\":\"<TOKEN>\",\"claim\":{\"id\":2,\"claim_token\":\"<TOKEN>\",\"session_id\":1,\"entity_"
       "kind\":\"task\",\"entity_id\":2,\"claim_scope\":\"exclusive\",\"status\":\"active\",\"vendor\":\"planar-agent\",\"vendor_"
       "session_id\":null,\"role\":\"coder\",\"model\":null,\"worktree_id\":null,\"worktree_path\":null,\"repo_root\":\"<ROOT>/"
       "proj\",\"branch\":null,\"head_sha_at_claim\":null,\"dirty_at_claim\":\"unknown\",\"purpose\":\"p\",\"base_ref\":null,"
       "\"claimed_at\":\"<TS>\",\"last_heartbeat_at\":\"<TS>\",\"lease_expires_at\":\"<TS>\",\"released_at\":null,\"release_"
       "reason\":null,\"failure_category\":null,\"run_id\":null,\"stage\":null}}\n",
       ""},
      {"heartbeat-unknown", {"heartbeat", "--claim", "deadbeef"}, 1, "", "error: heartbeat: ClaimNotFound\n"},
      {"complete",
       {"complete", "--claim", "@TOKEN1", "--summary", "done"},
       0,
       "ok task:1 status:done claim_status:completed\n",
       ""},
      {"complete-twice", {"complete", "--claim", "@TOKEN1"}, 1, "", "error: complete: ClaimNotActive\n"},
      {"release-json",
       {"release", "--claim", "@TOKEN2", "--reason", "giving up", "--json"},
       0,
       "{\"ok\":true,\"claim_token\":\"<TOKEN>\",\"claim\":{\"id\":2,\"claim_token\":\"<TOKEN>\",\"session_id\":1,\"entity_"
       "kind\":\"task\",\"entity_id\":2,\"claim_scope\":\"exclusive\",\"status\":\"released\",\"vendor\":\"planar-agent\","
       "\"vendor_session_id\":null,\"role\":\"coder\",\"model\":null,\"worktree_id\":null,\"worktree_path\":null,\"repo_root\":"
       "\"<ROOT>/"
       "proj\",\"branch\":null,\"head_sha_at_claim\":null,\"dirty_at_claim\":\"unknown\",\"purpose\":\"p\",\"base_ref\":null,"
       "\"claimed_at\":\"<TS>\",\"last_heartbeat_at\":\"<TS>\",\"lease_expires_at\":\"<TS>\",\"released_at\":\"<TS>\",\"release_"
       "reason\":\"giving "
       "up\",\"failure_category\":null,\"run_id\":null,\"stage\":null},\"task\":{\"id\":2,\"scope_kind\":\"association\",\"scope_"
       "id\":1,\"plan_id\":1,\"parent_task_id\":null,\"title\":\"task "
       "1\",\"body\":null,\"slug\":null,\"status\":\"todo\",\"priority\":100,\"next_action\":null,\"due_at\":null,\"created_at\":"
       "\"<TS>\",\"updated_at\":\"<TS>\"}}\n",
       ""},
      {"claim", {"claim", "--entity", "task:3"}, 0, "claim:<TOKEN> entity:task:3 status:active\n", ""},
      {"claim-contended", {"claim", "--entity", "task:3"}, 1, "", "error: claim: ClaimContention\n"},
      {"claim-force-doing", {"claim", "--entity", "task:3", "--force"}, 1, "", "error: claim: IllegalTransition\n"},
      {"claim-no-transition",
       {"claim", "--entity", "task:4", "--no-transition", "--json"},
       0,
       "{\"ok\":true,\"claim_token\":\"<TOKEN>\",\"claim\":{\"id\":4,\"claim_token\":\"<TOKEN>\",\"session_id\":1,\"entity_"
       "kind\":\"task\",\"entity_id\":4,\"claim_scope\":\"exclusive\",\"status\":\"active\",\"vendor\":\"planar-agent\",\"vendor_"
       "session_id\":null,\"role\":null,\"model\":null,\"worktree_id\":null,\"worktree_path\":null,\"repo_root\":\"<ROOT>/"
       "proj\",\"branch\":null,\"head_sha_at_claim\":null,\"dirty_at_claim\":\"unknown\",\"purpose\":null,\"base_ref\":null,"
       "\"claimed_at\":\"<TS>\",\"last_heartbeat_at\":\"<TS>\",\"lease_expires_at\":\"<TS>\",\"released_at\":null,\"release_"
       "reason\":null,\"failure_category\":null,\"run_id\":null,\"stage\":null}}\n",
       ""},
      {"claim-bad-kind",
       {"claim", "--entity", "bogus:1"},
       1,
       "",
       "error: invalid --entity 'bogus:1' (UnsupportedEntityKind); expected task:<id>|plan:<id>|plan_step:<id>\n"},
      {"claim-bad-id",
       {"claim", "--entity", "task:abc"},
       2,
       "",
       "error: invalid --entity 'task:abc' (InvalidEntityRef); expected task:<id>|plan:<id>|plan_step:<id>\n"},
      {"claim-no-colon",
       {"claim", "--entity", "nocolon"},
       2,
       "",
       "error: invalid --entity 'nocolon' (InvalidEntityRef); expected task:<id>|plan:<id>|plan_step:<id>\n"},
      {"claim-missing-task", {"claim", "--entity", "task:99"}, 1, "", "error: claim: TaskNotFound\n"},
      {"claim-plan",
       {"claim", "--entity", "plan:1", "--json"},
       0,
       "{\"ok\":true,\"claim_token\":\"<TOKEN>\",\"claim\":{\"id\":5,\"claim_token\":\"<TOKEN>\",\"session_id\":1,\"entity_"
       "kind\":\"plan\",\"entity_id\":1,\"claim_scope\":\"exclusive\",\"status\":\"active\",\"vendor\":\"planar-agent\",\"vendor_"
       "session_id\":null,\"role\":null,\"model\":null,\"worktree_id\":null,\"worktree_path\":null,\"repo_root\":\"<ROOT>/"
       "proj\",\"branch\":null,\"head_sha_at_claim\":null,\"dirty_at_claim\":\"unknown\",\"purpose\":null,\"base_ref\":null,"
       "\"claimed_at\":\"<TS>\",\"last_heartbeat_at\":\"<TS>\",\"lease_expires_at\":\"<TS>\",\"released_at\":null,\"release_"
       "reason\":null,\"failure_category\":null,\"run_id\":null,\"stage\":null}}\n",
       ""},
      {"complete-todo-task", {"complete", "--claim", "@TOKEN4"}, 1, "", "error: complete: IllegalTransition\n"},
      {"fail",
       {"fail", "--claim", "@TOKEN3", "--reason", "broke", "--category", "tool_failure"},
       0,
       "ok task:3 status:todo claim_status:aborted\n",
       ""},
      {"action-start",
       {"action", "start", "--claim", "@TOKEN4", "--kind", "tool_call", "--json"},
       0,
       "{\"ok\":true,\"action_id\":5,\"action\":{\"id\":5,\"session_id\":1,\"session_entry_id\":null,\"parent_action_id\":null,"
       "\"claim_id\":4,\"action_kind\":\"tool_call\",\"entity_kind\":null,\"entity_id\":null,\"vendor\":\"planar-agent\","
       "\"vendor_role\":null,\"model\":null,\"started_at\":\"<TS>\",\"ended_at\":null,\"outcome\":null,\"summary\":null,\"head_"
       "sha\":null,\"dirty\":null,\"metadata\":null}}\n",
       ""},
      {"action-start-bad-kind",
       {"action", "start", "--claim", "@TOKEN4", "--kind", "bogus"},
       2,
       "",
       "error: unknown action kind 'bogus'\n"},
      {"action-start-entity",
       {"action", "start", "--claim", "@TOKEN4", "--kind", "coder", "--entity", "decision:1"},
       0,
       "action:6 kind:coder claim:<TOKEN>\n",
       ""},
      {"action-start-bad-entity",
       {"action", "start", "--claim", "@TOKEN4", "--kind", "coder", "--entity", "bogus:1"},
       2,
       "",
       "error: unknown entity kind 'bogus'\n"},
      {"action-end",
       {"action", "end", "--action", "5", "--outcome", "ok", "--summary", "s", "--json"},
       0,
       "{\"ok\":true,\"action_id\":5,\"action\":{\"id\":5,\"session_id\":1,\"session_entry_id\":null,\"parent_action_id\":null,"
       "\"claim_id\":4,\"action_kind\":\"tool_call\",\"entity_kind\":null,\"entity_id\":null,\"vendor\":\"planar-agent\","
       "\"vendor_role\":null,\"model\":null,\"started_at\":\"<TS>\",\"ended_at\":\"<TS>\",\"outcome\":\"ok\",\"summary\":\"s\","
       "\"head_sha\":null,\"dirty\":null,\"metadata\":null}}\n",
       ""},
      {"action-end-again", {"action", "end", "--action", "5"}, 0, "ok action:5 outcome:ok\n", ""},
      {"action-end-missing", {"action", "end", "--action", "999"}, 1, "", "error: getActionById: ClaimNotFound\n"},
      {"action-end-bad-outcome",
       {"action", "end", "--action", "5", "--outcome", "bogus"},
       2,
       "",
       "error: unknown --outcome 'bogus'\n"},
      {"block",
       {"block", "--claim", "@TOKEN4", "--blocker", "2", "--reason", "waiting", "--json"},
       0,
       "{\"ok\":true,\"claim_token\":\"<TOKEN>\",\"claim\":{\"id\":4,\"claim_token\":\"<TOKEN>\",\"session_id\":1,\"entity_"
       "kind\":\"task\",\"entity_id\":4,\"claim_scope\":\"exclusive\",\"status\":\"released\",\"vendor\":\"planar-agent\","
       "\"vendor_session_id\":null,\"role\":null,\"model\":null,\"worktree_id\":null,\"worktree_path\":null,\"repo_root\":\"<"
       "ROOT>/"
       "proj\",\"branch\":null,\"head_sha_at_claim\":null,\"dirty_at_claim\":\"unknown\",\"purpose\":null,\"base_ref\":null,"
       "\"claimed_at\":\"<TS>\",\"last_heartbeat_at\":\"<TS>\",\"lease_expires_at\":\"<TS>\",\"released_at\":\"<TS>\",\"release_"
       "reason\":\"waiting\",\"failure_category\":null,\"run_id\":null,\"stage\":null},\"task\":{\"id\":4,\"scope_kind\":"
       "\"association\",\"scope_id\":1,\"plan_id\":1,\"parent_task_id\":null,\"title\":\"task "
       "3\",\"body\":null,\"slug\":null,\"status\":\"blocked\",\"priority\":100,\"next_action\":null,\"due_at\":null,\"created_"
       "at\":\"<TS>\",\"updated_at\":\"<TS>\"}}\n",
       ""},
      {"abort",
       {"abort", "--claim", "@TOKEN5", "--reason", "stuck", "--json"},
       0,
       "{\"ok\":true,\"claim_token\":\"<TOKEN>\",\"claim\":{\"id\":5,\"claim_token\":\"<TOKEN>\",\"session_id\":1,\"entity_"
       "kind\":\"plan\",\"entity_id\":1,\"claim_scope\":\"exclusive\",\"status\":\"aborted\",\"vendor\":\"planar-agent\","
       "\"vendor_session_id\":null,\"role\":null,\"model\":null,\"worktree_id\":null,\"worktree_path\":null,\"repo_root\":\"<"
       "ROOT>/"
       "proj\",\"branch\":null,\"head_sha_at_claim\":null,\"dirty_at_claim\":\"unknown\",\"purpose\":null,\"base_ref\":null,"
       "\"claimed_at\":\"<TS>\",\"last_heartbeat_at\":\"<TS>\",\"lease_expires_at\":\"<TS>\",\"released_at\":\"<TS>\",\"release_"
       "reason\":\"stuck\",\"failure_category\":null,\"run_id\":null,\"stage\":null},\"aborting_session\":1}\n",
       ""},
      {"abort-unknown", {"abort", "--claim", "deadbeef"}, 1, "", "error: abort: ClaimNotFound\n"},
      {"reconcile-dry", {"reconcile", "--dry-run"}, 0, "dry-run: 0 claim candidate(s), 0 run candidate(s)\n", ""},
      {"reconcile-dry-json",
       {"reconcile", "--dry-run", "--json"},
       0,
       "{\"ok\":true,\"claims_marked_stale\":0,\"actions_closed\":0,\"runs_abandoned\":0,\"candidates\":[],\"run_candidates\":[]}"
       "\n",
       ""},
      {"reconcile-badgrace",
       {"reconcile", "--stale-after", "zzz"},
       1,
       "",
       "error: invalid --stale-after 'zzz': expected bare seconds (e.g. 0) or suffixed duration (e.g. 10m, 1h, 500ms)\n"},
      {"reconcile",
       {"reconcile", "--json", "--category", "usage_limit"},
       0,
       "{\"ok\":true,\"claims_marked_stale\":0,\"actions_closed\":0,\"runs_abandoned\":0}\n",
       ""},
      {"reconcile-text", {"reconcile"}, 0, "reconciled: 0 claim(s) stale, 0 action(s) closed, 0 run(s) abandoned\n", ""},
      {"reconcile-session",
       {"reconcile", "--session", "1", "--json"},
       0,
       "{\"ok\":true,\"claims_marked_stale\":0,\"actions_closed\":0,\"runs_abandoned\":0}\n",
       ""},
      {"reconcile-plan",
       {"reconcile", "--plan", "1", "--json"},
       0,
       "{\"ok\":true,\"claims_marked_stale\":0,\"actions_closed\":0,\"runs_abandoned\":0}\n",
       ""},
  };

  for (auto const& item : script) {
    auto const cpp = run_pinned(cpp_bin(), resolve(item.args, arena.cpp_root), arena.cpp_root, item.tag);
    INFO("step: " << item.tag);
    CHECK(cpp.code == item.expected_code);
    CHECK(normalise(cpp.out, arena.cpp_root) == item.expected_out);
    CHECK(normalise(cpp.err, arena.cpp_root) == item.expected_err);
  }
}

TEST_CASE("planar-agent parity: the two databases end in identical states", "[cmd][agent][parity][claims]") {
  // TASK 6539 RETIREMENT. This case's SUBJECT is a comparison between two
  // databases, not two byte streams — but the five queries below name
  // exactly the columns that vary (status, category, reason, outcome,
  // summary, the `entity_links` edge) and name NONE of the columns that
  // would make a literal expectation flaky (no claim token, no timestamp
  // appears in any of them). So, unlike the header comment before this
  // rewrite worried, there IS an artifact here: the query RESULT, which is
  // exactly as pinnable as any other transcribed byte string. Confirmed
  // identical between the C++ and Zig databases before this rewrite, at
  // commit 4fc09c0777cb0456e8ef52889dc7ca7f44a93f34; the expected strings
  // below are transcribed from the C++ side of that run.
  //
  // Output parity is not state parity: two binaries can print the same
  // line and write different rows. This walks a shorter script and then
  // pins the ROWS — statuses, categories, reasons, action outcomes, the
  // `entity_links` edge `block` writes, and the plan roll-up the injected
  // policy performs.
  //
  // Runs without the oracle — it is an assertion about this binary, not a
  // comparison — so it stays live on a checkout with no zig/ build.
  auto const arena = make_arena("state");
  seed_arena(arena.cpp_root, 3);

  std::vector<step> const script{
      {"pull", {"pull", "1"}},
      {"complete", {"complete", "--claim", "@TOKEN1", "--summary", "done"}},
      {"pull2", {"pull", "1"}},
      {"fail", {"fail", "--claim", "@TOKEN2", "--reason", "broke", "--category", "validation"}},
      {"claim", {"claim", "--entity", "task:3"}},
      {"block", {"block", "--claim", "@TOKEN3", "--blocker", "1", "--reason", "waiting"}},
  };
  for (auto const& [tag, args] : script) {
    (void)run_pinned(cpp_bin(), resolve(args, arena.cpp_root), arena.cpp_root, tag);
  }

  auto const dump = [](const std::filesystem::path& root, std::string_view sql) {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare(sql);
    REQUIRE(stmt.has_value());
    std::string out;
    while (true) {
      auto stepped = stmt->step();
      REQUIRE(stepped.has_value());
      if (*stepped != planar::db::step_result::row) {
        break;
      }
      out += stmt->column_text(0);
      out += '\n';
    }
    return out;
  };

  struct pinned_query {
    std::string_view sql;
    std::string_view expected;
  };

  for (auto const& item : std::vector<pinned_query>{
           {"select id || '|' || entity_kind || '|' || entity_id || '|' || status || '|' || "
            "coalesce(failure_category,'') || '|' || coalesce(release_reason,'') from agent_work_claims order by id",
            "1|task|1|completed||\n2|task|2|aborted|validation|broke\n3|task|3|released||waiting\n"},
           {"select id || '|' || status from tasks order by id", "1|done\n2|todo\n3|blocked\n"},
           {"select id || '|' || status from plans order by id", "1|active\n"},
           {"select id || '|' || action_kind || '|' || coalesce(outcome,'') || '|' || coalesce(summary,'') "
            "from agent_actions order by id",
            "1|coder|ok|done\n2|coder|error|\n3|claim_check|aborted|waiting\n"},
           {"select from_kind || '|' || from_id || '|' || to_kind || '|' || to_id || '|' || relationship from entity_links "
            "order by id",
            "task|3|task|1|depends-on\n"},
       }) {
    INFO("query: " << item.sql);
    CHECK(dump(arena.cpp_root, item.sql) == item.expected);
  }
}

TEST_CASE("planar-agent parity: pull respects an open depends-on blocker, and completion auto-unblocks a dependent",
          "[cmd][agent][parity][claims][deps]") {
  // End-to-end proof of both plan-1068 M2 engine fixes (tasks 6841 and
  // 6875), walked through the REAL binaries rather than the engine layer
  // directly — the shape a real orchestrator hits.
  //
  //   A = task:1  (no dependency)
  //   B = task:2  `task block --on 1`        -- depends-on A, status BLOCKED
  //   D = task:3  `task link --relationship depends-on` on B, priority 5
  //               (the LOWEST of the three) -- depends-on B, status stays
  //               TODO the whole time
  //
  // D's priority is the load-bearing part: a selector that ignored
  // dependencies entirely would pick D FIRST at both pulls below, because
  // 5 < 100 < 100. That it never does is task 6841's regression test,
  // exercised twice — once while its blocker (B) is `blocked`, once after
  // B is `todo` but still not `done`.
  //
  // Task 6875's regression is the middle step: `complete`, not
  // `task done`, is what finishes A. Before the fix, B stayed `blocked`
  // forever because `planar-agent complete`'s terminal transaction never
  // ran the dependency roll-up — only `mark_done`/`mark_cancelled`/
  // `update_task` (the `planar task ...` paths) did.
  auto const arena = make_arena("depspull");
  seed_arena(arena.cpp_root, 3);

  (void)run_pinned(cpp_planar_bin(), std::vector<std::string>{"task", "update", "3", "--priority", "5"}, arena.cpp_root,
                   "set-priority-d");
  (void)run_pinned(cpp_planar_bin(), std::vector<std::string>{"task", "block", "2", "--on", "1", "--reason", "waiting"},
                   arena.cpp_root, "block-b-on-a");
  (void)run_pinned(cpp_planar_bin(), std::vector<std::string>{"task", "link", "3", "task:2", "--relationship", "depends-on"},
                   arena.cpp_root, "link-d-on-b");

  // Sanity on the fixture itself, so a failure below is unambiguous about
  // WHICH invariant broke.
  REQUIRE(scalar_int_state(arena.cpp_root, "select status = 'blocked' from tasks where id = 2") == 1);
  REQUIRE(scalar_int_state(arena.cpp_root, "select priority from tasks where id = 3") == 5);

  // --- pull #1: D is excluded despite the lowest priority; B is not
  //     `todo` at all; A is the only real candidate. ---
  auto const pull1 = run_pinned(cpp_bin(), std::vector<std::string>{"pull", "1"}, arena.cpp_root, "pull-1");
  CHECK(pull1.code == 0);
  CHECK(normalise(pull1.out, arena.cpp_root) == "pulled task:1 claim:<TOKEN> action:1\n");

  // --- complete A via the AGENT plane, not `planar task done` — the
  //     exact path task 6875 fixes. ---
  auto const token1 = token_for(arena.cpp_root, 1);
  REQUIRE_FALSE(token1.empty());
  auto const complete1 = run_pinned(cpp_bin(), std::vector<std::string>{"complete", "--claim", token1, "--summary", "done"},
                                    arena.cpp_root, "complete-1");
  CHECK(complete1.code == 0);
  CHECK(complete1.out == "ok task:1 status:done claim_status:completed\n");

  // B's roll-up: `blocked` -> `todo`, WITHOUT going through `task done`.
  // Black-box convention (CLAUDE.md): assert post-state via `show --json`,
  // not raw SQL against the fixture database.
  auto const show_b1 = run_pinned(cpp_planar_bin(), std::vector<std::string>{"task", "show", "2", "--json"}, arena.cpp_root,
                                  "show-b-after-complete");
  CHECK(show_b1.code == 0);
  CHECK(show_b1.out.contains(R"("status":"todo")"));
  // D is untouched by the roll-up (it was never `blocked`) and its OWN
  // blocker (B) is still not `done` — still excluded.
  auto const show_d1 = run_pinned(cpp_planar_bin(), std::vector<std::string>{"task", "show", "3", "--json"}, arena.cpp_root,
                                  "show-d-after-complete");
  CHECK(show_d1.code == 0);
  CHECK(show_d1.out.contains(R"("status":"todo")"));

  // --- pull #2: B is now eligible (its blocker A is `done`); D is STILL
  //     excluded (its blocker B is `todo`, not `done`), despite still
  //     having the lower priority. If either the 6841 exclusion or the
  //     6875 roll-up regressed, this claims task:3 or reports `no_work`
  //     instead of task:2. ---
  auto const pull2 = run_pinned(cpp_bin(), std::vector<std::string>{"pull", "1"}, arena.cpp_root, "pull-2");
  CHECK(pull2.code == 0);
  CHECK(normalise(pull2.out, arena.cpp_root) == "pulled task:2 claim:<TOKEN> action:2\n");
}

TEST_CASE("planar-agent parity: task cancel also rolls up a blocked dependent (second production binding)",
          "[cmd][agent][parity][claims][deps]") {
  // The case above pins `clear_unblocked_dependents` reached through
  // `planar-agent complete` -> `mark_done` -- the ONLY production call
  // site it exercises end to end. `mark_cancelled` is a second, distinct
  // production call site for the same roll-up (decision 1122: cancelled
  // is terminal and clears dependents exactly as done does), reached
  // through `planar task cancel` rather than the agent plane at all. This
  // case pins that second binding so a regression specific to the
  // `mark_cancelled` call site (task.cpp) is not masked by the `mark_done`
  // coverage above.
  auto const arena = make_arena("cancelpull");
  seed_arena(arena.cpp_root, 2);

  (void)run_pinned(cpp_planar_bin(), std::vector<std::string>{"task", "block", "2", "--on", "1", "--reason", "waiting"},
                   arena.cpp_root, "block-b-on-a");
  REQUIRE(scalar_int_state(arena.cpp_root, "select status = 'blocked' from tasks where id = 2") == 1);

  auto const cancel_a = run_pinned(cpp_planar_bin(), std::vector<std::string>{"task", "cancel", "1"}, arena.cpp_root, "cancel-a");
  CHECK(cancel_a.code == 0);

  auto const show_b = run_pinned(cpp_planar_bin(), std::vector<std::string>{"task", "show", "2", "--json"}, arena.cpp_root,
                                 "show-b-after-cancel");
  CHECK(show_b.code == 0);
  CHECK(show_b.out.contains(R"("status":"todo")"));
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

TEST_CASE("planar-agent schema --command emits exactly one command's object", "[cmd][agent][schema][7204]") {
  auto const arena = make_arena("schema7204one");
  auto const one =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-agent queue status"}, arena.cpp_root, "one");
  CHECK(one.code == 0);
  CHECK(one.err.empty());
  CHECK(one.out.starts_with(R"({"name":")"));
  CHECK(one.out.ends_with("}\n"));
  CHECK(one.out.contains(R"("command":"planar-agent queue status")"));
  CHECK(count_of(one.out, R"("command":")") == 1);
  CHECK(one.out.size() < 4096);

  // The relative spelling resolves to the same bytes.
  auto const rel = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command=queue status"}, arena.cpp_root, "rel");
  CHECK(rel.code == 0);
  CHECK(rel.out == one.out);

  // The object is the one the full catalog carries, byte for byte.
  auto const full = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "full");
  REQUIRE(full.code == 0);
  CHECK(full.out.contains(one.out.substr(0, one.out.size() - 1)));
}

TEST_CASE("planar-agent schema --command with an unknown path exits 2, names it, prints nothing", "[cmd][agent][schema][7204]") {
  auto const arena = make_arena("schema7204bad");
  auto const bad   = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-agent queue status nonesuch"},
                                arena.cpp_root, "bad");
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err.contains("planar-agent queue status nonesuch"));
}

TEST_CASE("planar-agent schema --compact is one two-key row per command", "[cmd][agent][schema][7204]") {
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
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--compact", "--command", "queue status"}, arena.cpp_root, "row");
  CHECK(row.code == 0);
  CHECK(row.out.starts_with(R"({"command":"planar-agent queue status","summary":")"));
  CHECK(count_of(row.out, R"("command":")") == 1);
  CHECK(compact.out.contains(row.out.substr(0, row.out.size() - 1)));
}

TEST_CASE("planar-agent schema --command queue is a lookup, not the queue subcommand", "[cmd][agent][schema][7204]") {
  auto const arena = make_arena("schema7204edge");
  auto const bare  = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "queue"}, arena.cpp_root, "bare");
  auto const full =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-agent queue"}, arena.cpp_root, "full");
  CHECK(bare.code == 0);
  CHECK(bare.err.empty());
  CHECK(bare.out.starts_with(R"({"name":"queue",)"));
  CHECK(bare.out.contains(R"("command":"planar-agent queue")"));
  CHECK(bare.out == full.out);
  // Same value placed before the verb, and in the `=` form.
  auto const before = run_pinned(cpp_bin(), std::vector<std::string>{"--command", "queue", "schema"}, arena.cpp_root, "before");
  CHECK(before.code == 0);
  CHECK(before.out == bare.out);
  auto const eq = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command=queue"}, arena.cpp_root, "eq");
  CHECK(eq.out == bare.out);
}
