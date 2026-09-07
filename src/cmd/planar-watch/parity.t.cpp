// @file parity.t.cpp
// @brief Differential tests: `planar-watch` (C++) against the Zig reference
// over identical argv in identical pinned scratch environments (plan 996,
// task 6107).
//
// Harness in `../parity_harness.hpp`; see that file for the database-safety
// rules it enforces and why it is a header rather than a target.
//
// ## What is compared
//
// Leaf help pages, both `completion` failure paths (the exit-1/exit-2 pair
// that discriminates this binary's policy), the unknown-verb path, and —
// the case that matters most for this particular binary — that no ported
// invocation creates a database FILE. That last one is the read-only
// invariant observed from OUTSIDE the process, complementing
// `context.t.cpp`'s inside-the-process write-refusal proof.
//
// TASK 6065 added the `schema` CATALOG to the compared set, and it is now
// the strongest case in the file: this binary's catalog is BYTE-IDENTICAL
// to the oracle's. It could not be before — the tree declared three of the
// oracle's twelve verbs.
//
// NOT compared: `version` (inherited `cxx` vs `zig` tag), the ROOT help
// page (`planar-watch --help` lists twelve verbs in both trees now, but a
// bare `planar-watch` still renders it here and routes to `feed` in the
// oracle — see `planar.cmd.planar_watch.surface`), and `completion
// <shell>`'s generated script (planar.cliapp.completion defers flag-VALUE
// completion — its own module header says so).
//
// SKIP, not fail, when the oracle is absent (D6).

#include <catch2/catch_test_macros.hpp>

#include "parity_strict.hpp"

import std;

#include "catalog_parity.hpp"
#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::capture;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the Zig reference binary.
/// @return The path.
auto zig_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_ZIG_BIN};
}

/// @brief True when the reference binary is present to diff against.
/// @return `true` if the oracle exists.
auto oracle_available() -> bool {
  return std::filesystem::exists(zig_bin());
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
  CHECK(version.out == "Print the planar-watch version, commit, and zig runtime.\n"
                       "\n"
                       "\n"
                       "version [OPTIONS]\n"
                       "\n"
                       "\n"
                       "OPTIONS:\n"
                       "  -h,     --help              Print this help message and exit\n");

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
                      "  -h,     --help              Print this help message and exit\n");

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
                          "  -h,     --help              Print this help message and exit\n");
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

// ===========================================================================
// The six read verbs, against the oracle (task 6120)
// ===========================================================================

namespace {

/// @brief Path to the Zig `planar` operator binary — the seed's author.
/// @return The path.
auto zig_planar() -> std::filesystem::path {
  return zig_bin().parent_path() / "planar";
}

/// @brief Path to the Zig `planar-agent` binary — the claim ritual's.
/// @return The path.
auto zig_agent() -> std::filesystem::path {
  return zig_bin().parent_path() / "planar-agent";
}

/// @brief True when all three oracle binaries the seed needs are present.
/// @return `true` when the fixture can be built.
auto seed_oracle_available() -> bool {
  return oracle_available() && std::filesystem::exists(zig_planar()) && std::filesystem::exists(zig_agent());
}

/// @brief Run one oracle seed step, failing the test if it does not exit 0.
/// @param bin The binary to run.
/// @param args The argument tail.
/// @param root The arena root.
/// @param tag A discriminator so each step gets its own capture files.
/// @return The captured result, for callers that need its stdout.
auto seed_step(const std::filesystem::path& bin, std::vector<std::string> args, const std::filesystem::path& root,
               std::string_view tag) -> capture {
  auto const got = run_pinned(bin, args, root, tag);
  INFO("seed step " << tag << " stderr: " << got.err);
  REQUIRE(got.code == 0);
  return got;
}

/// @brief The `claim_token` out of a `--json` claim payload.
///
/// A deliberately crude scan rather than a JSON parse: this file imports no
/// JSON library, the key appears once in every payload it is used on, and
/// the token is 32 hex characters with nothing to escape.
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

/// @brief Strip the two values that CANNOT match between two invocations.
///
/// `generated_at` is read from the host clock at emit time, and the
/// `last_hb:` column ages between the C++ run and the Zig run. Both are
/// nondeterministic BY CONSTRUCTION (see
/// `planar.cmd.planar_watch.handlers.format`'s header on the clock), so
/// pinning them would pin the moment the test ran. Everything else —
/// including every row timestamp, which comes from SQLite and is therefore
/// identical for both readers — is compared verbatim.
/// @param text The payload to normalize.
/// @return The normalized payload.
auto normalize(std::string_view text) -> std::string {
  std::string out{text};
  auto const  blank_after = [&out](std::string_view key, std::string_view stop) {
    std::size_t at = 0;
    while ((at = out.find(key, at)) != std::string::npos) {
      auto const start = at + key.size();
      auto       end   = out.find_first_of(stop, start);
      if (end == std::string::npos) {
        end = out.size();
      }
      out.replace(start, end - start, "<normalized>");
      at = start + std::string_view{"<normalized>"}.size();
    }
  };
  blank_after("\"generated_at\":\"", "\"");
  // The relative-time renderer emits `just now` / `7s` / `2h` — the space
  // in `just now` is why the stop set here is the newline and the
  // two-space column separator rather than any whitespace.
  std::size_t at = 0;
  while ((at = out.find("last_hb:", at)) != std::string::npos) {
    auto const start = at + std::string_view{"last_hb:"}.size();
    auto       end   = out.find("  ", start);
    auto const eol   = out.find('\n', start);
    if (eol != std::string::npos && (end == std::string::npos || eol < end)) {
      end = eol;
    }
    if (end == std::string::npos) {
      end = out.size();
    }
    out.replace(start, end - start, "<normalized>");
    at = start + std::string_view{"<normalized>"}.size();
  }
  return out;
}

} // namespace

TEST_CASE("planar-watch parity: the six read verbs agree with the oracle over a seeded database",
          "[cmd][watch][parity][oracle]") {
  // TASK 6540 ASSESSED, NOT RETIRED. Every other oracle-conditional case in
  // this file transcribes exact bytes and pins them against the built
  // binary alone (see the completion/unknown-verb/catalog cases above) —
  // that pattern only works because the compared bytes are DETERMINISTIC
  // given fixed argv: a help page, a schema catalog.
  //
  // This case's fixture is not. Re-seeding it with the CPP `planar` /
  // `planar-agent` binaries (dropping the need for the oracle to build the
  // fixture) and re-running it twice back to back — measured at this
  // commit — showed every one of the 47 read-verb outputs differing
  // between runs: `claim_token` is 32 fresh random hex characters per
  // `pull`, and every row timestamp (`claimed_at`, `started_at`,
  // `lease_expires_at`, `created_at`, and more, at millisecond precision)
  // is the real wall clock at insert. `normalize()` below already strips
  // the two fields that are nondeterministic BETWEEN TWO READERS of the
  // SAME row (`generated_at`, `last_hb`) — that narrow scope works
  // specifically because both readers agree on every other byte, since
  // they read identical rows. It does not generalize to a literal pin
  // against a single binary's own output captured on a DIFFERENT run: a
  // full pin would need a generic normalizer for every timestamp field and
  // for the claim token, which risks a normalizer permissive enough to
  // mask a real rendering regression behind it — worse than leaving the
  // case oracle-gated.
  //
  // DECISION 1034 (2026-09-07): this case is DELETED in task 6045, in the
  // same commit that deletes `zig/` — the treatment decision 999 gave the
  // `planar-ext` cases that became structurally incomparable.
  //
  // The reason is not determinism. The fixture is ONE arena, ONE seeded
  // database, TWO readers: the nondeterministic values are identical for
  // both because they are baked into the shared rows, which is why the
  // narrow `normalize()` above suffices. The subject is cross-IMPLEMENTATION
  // agreement, and after cutover there is no second implementation. A frozen
  // clock/seeded RNG would enable a DIFFERENT test (a rendering
  // characterization pin, task 6545), not preserve this one.
  //
  // Leaving it gated was rejected: it would skip silently forever under
  // plain `ctest` (SKIP_RETURN_CODE 4) and fail permanently under
  // `make test-cpp-strict` (PLANAR_PARITY_STRICT=1, --max-skips 0).
  //
  // It stays LIVE until then: while `zig/` builds, this is a real
  // cross-implementation check. See task 6540's report for the measurement.
  PLANAR_REQUIRE_ORACLE(seed_oracle_available(),
                        "Zig oracle binaries not built (zig/zig-out/bin/{planar,planar-agent,planar-watch})");

  // ONE ARENA, BOTH BINARIES — the deliberate departure from every other
  // case in this file, and it is a property of THIS binary specifically.
  //
  // The usual shape gives each side its own arena and seeds them
  // identically. That works when the fixture is deterministic. It cannot
  // work here: `claim_token` is 32 RANDOM hex characters minted per claim,
  // and every row timestamp is the wall clock at insert. Two arenas would
  // differ in almost every byte of the output being compared, and the only
  // way to salvage it would be to normalize away the very fields the verbs
  // exist to display.
  //
  // Pointing both readers at ONE database is safe precisely because of the
  // invariant under test — `planar-watch` cannot write. And it makes the
  // comparison STRICTER, not weaker: identical input bytes, so any
  // difference in output is the port's.
  //
  // The fixture is built by the ORACLE's `planar` and `planar-agent`, which
  // is legitimate for the same reason the `unlink` case above gives: the
  // subject under test is the READ verbs, and seeding with the reference
  // binaries means the database is correct by construction rather than by
  // assertion.
  auto const space = make_arena("watchread");
  auto const root  = space.cpp_root;

  seed_step(zig_planar(), {"init"}, root, "s00");
  seed_step(zig_planar(), {"assoc", "create", "project:proj", "--kind", "project"}, root, "s01");
  seed_step(zig_planar(), {"assoc", "add", "project:proj", (root / "proj").string()}, root, "s02");
  seed_step(zig_planar(), {"plan", "create", "Demo plan"}, root, "s03");
  for (int i = 1; i <= 8; ++i) {
    seed_step(zig_planar(), {"task", "add", std::format("Task {}", i), "--plan", "1"}, root, std::format("s1{}", i));
  }
  seed_step(zig_planar(), {"plan", "update", "1", "--status", "active"}, root, "s20");
  // A second plan with NO agent activity — `plans --in-flight-only` has
  // nothing to drop without it.
  seed_step(zig_planar(), {"plan", "create", "Idle plan"}, root, "s21");

  auto const worktree = root / "proj" / "a-very-long-worktree-path-well-over-forty-characters" / "agent-42";
  std::filesystem::create_directories(worktree);

  // A long worktree path (the `…basename` elision) and a role.
  auto const first =
      seed_step(zig_agent(), {"pull", "1", "--vendor", "claude", "--role", "coder", "--worktree", worktree.string(), "--json"},
                root, "c01");
  auto const first_token = token_from(first.out);
  // A FOUR-LEVEL forest, and every edge below is chosen rather than
  // incidental. `tree` has three glyph decisions and a shallow fixture
  // reaches only some of them; TWO break-probes survived before this shape
  // existed. Actions are numbered in pull order, so the parents below
  // produce:
  //
  //     action:1                  <- root, no prefix
  //     ├── a2                    <- has a later sibling  -> ├──
  //     │   ├── a3                <- THE VERTICAL GUIDE at level 1, drawn
  //     └── a4                       only because a2 is not last
  //             └── a5            <- depth 3: reads the guide state for
  //     └── a6                       levels 1 AND 2
  //
  //   * `│   ` at level 1 (probe: replacing it with four spaces survived a
  //     two-level fixture — a5/a6 are what catch it).
  //   * THE DEEPER-LEVEL CLEARING. a3 sets "level 2 is open" because a6
  //     follows it under the same parent. a4 is at depth 1, so it must
  //     CLEAR level 2 before a5 (depth 3) reads it — otherwise a5 renders
  //     `    │   └── ` instead of `        └── `. Probe: deleting the
  //     clearing loop survived every shallower fixture.
  //   * `├──` vs `└──`, which any two-level fixture already covers.
  seed_step(zig_agent(), {"pull", "1", "--vendor", "codex", "--role", "reviewer", "--parent-action", "1", "--json"}, root, "c02");
  seed_step(zig_agent(), {"pull", "1", "--vendor", "copilot", "--role", "test_coder", "--parent-action", "2", "--json"}, root,
            "c03");
  // Depth 1 again — the node whose rendering must clear the deeper levels.
  // With no `--role` at all, so it also gives `--group-by role` its
  // "unknown" bucket.
  seed_step(zig_agent(), {"pull", "1", "--vendor", "claude", "--parent-action", "1", "--json"}, root, "c04");
  // Depth 3, under a3.
  seed_step(zig_agent(), {"pull", "1", "--vendor", "codex", "--role", "coder", "--parent-action", "3", "--json"}, root, "c04b");
  // A second child of a2, which is what makes a3 a NON-last sibling and so
  // opens level 2 in the first place.
  auto const sibling = seed_step(
      zig_agent(), {"pull", "1", "--vendor", "copilot", "--role", "reviewer", "--parent-action", "2", "--json"}, root, "c04c");
  auto const sibling_token = token_from(sibling.out);
  // An action with NO `--entity` at all, so `actions`' text arm renders its
  // null entity — `entity:-:0`, a literal dash and a zero standing in for
  // columns that do not exist. Probe: rendering the empty string instead of
  // `-` survived a fixture in which every action had an entity, which is
  // every action a `pull` creates.
  seed_step(zig_agent(), {"action", "start", "--claim", sibling_token, "--kind", "heartbeat", "--json"}, root, "c04d");
  // A terminal claim carrying a closed failure category -> the `category:`
  // column, which only appears on a failed claim.
  auto const failed       = seed_step(zig_agent(), {"pull", "1", "--vendor", "codex", "--role", "coder", "--json"}, root, "c05");
  auto const failed_token = token_from(failed.out);
  seed_step(zig_agent(), {"fail", "--claim", failed_token, "--reason", "gate failed", "--category", "tool_failure"}, root, "c06");

  // A summary well over the 80-byte activity limit whose 77TH BYTE LANDS
  // INSIDE A MULTI-BYTE CHARACTER (0x94, the third byte of the third em
  // dash), so the truncation path is exercised on a real UTF-8 boundary
  // rather than on ASCII. Verified by construction in `format.t.cpp`; the
  // point of repeating it here is that this arm compares the whole rendered
  // column against the ORACLE, which `format.t.cpp` cannot do.
  seed_step(zig_agent(), {"action", "start", "--claim", first_token, "--kind", "tool_call", "--entity", "task:1", "--json"}, root,
            "c07");
  seed_step(zig_agent(),
            // Action 9: six pulls (1-6), the entity-less `action start` (7),
            // the failed pull (8), and the `action start` immediately above.
            {"action", "end", "--action", "9", "--outcome", "ok", "--summary",
             "the coordination layer rewired every caller and then some more words "
             "\xe2\x80\x94\xe2\x80\x94\xe2\x80\x94 tail padding to push past eighty bytes"},
            root, "c08");

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  std::vector<step> const steps{
      {"ps", {"ps"}},
      {"psj", {"ps", "--json"}},
      {"pss", {"ps", "--stale"}},
      {"pslease", {"ps", "--sort-by", "lease"}},
      {"psgrole", {"ps", "--group-by", "role"}},
      {"psgrolej", {"ps", "--group-by", "role", "--json"}},
      {"psgscope", {"ps", "--group-by", "scope"}},
      {"psgvendor", {"ps", "--group-by", "vendor", "--vendor", "claude"}},
      {"psgplan", {"ps", "--group-by", "scope", "--plan", "1"}},
      {"psgplan9", {"ps", "--group-by", "scope", "--plan", "9", "--json"}},
      {"pssortbad", {"ps", "--sort-by", "bogus"}},
      {"psgroupbad", {"ps", "--group-by", "bogus"}},
      {"claims", {"claims"}},
      {"claimsj", {"claims", "--json"}},
      {"claimsall", {"claims", "--status", "all"}},
      {"claimsallj", {"claims", "--status", "all", "--json"}},
      {"claimsstale", {"claims", "--status", "stale", "--json"}},
      {"claimsbogus", {"claims", "--status", "nonsense"}},
      {"claimsvendor", {"claims", "--vendor", "codex", "--status", "all"}},
      {"claimsplan", {"claims", "--plan", "1", "--status", "all"}},
      {"claimsplan9", {"claims", "--plan", "9", "--status", "all"}},
      {"actions", {"actions"}},
      {"actionsj", {"actions", "--json"}},
      {"actionslimit", {"actions", "--limit", "2"}},
      {"actionskind", {"actions", "--kind", "tool_call"}},
      {"actionsentity", {"actions", "--entity", "task:1", "--json"}},
      {"actionstask", {"actions", "--task", "2"}},
      {"actionsplan", {"actions", "--plan", "1"}},
      {"actionsbad", {"actions", "--entity", "nocolon"}},
      {"plans", {"plans"}},
      {"plansj", {"plans", "--json"}},
      {"plansif", {"plans", "--in-flight-only", "--json"}},
      {"tree", {"tree"}},
      {"treesession", {"tree", "--root-session", "1"}},
      {"treezero", {"tree", "--root-session", "0"}},
      {"treemissing", {"tree", "--root-session", "9999"}},
      {"log0", {"log"}},
      {"log2", {"log", "--task", "1", "--plan", "1"}},
      {"logtask", {"log", "--task", "1"}},
      {"logtaskj", {"log", "--task", "1", "--json"}},
      // The seventh pull took task 7, and its claim is the FAILED one — so
      // this timeline carries a `claim_aborted` terminal event with a
      // `failure_category`, which no other case here reaches.
      {"logfailedj", {"log", "--task", "7", "--json"}},
      {"logsession", {"log", "--session", "1", "--json"}},
      {"logclaim", {"log", "--claim", failed_token, "--json"}},
      {"logclaimmiss", {"log", "--claim", "deadbeef", "--json"}},
      {"loglimit", {"log", "--task", "1", "--limit", "1", "--json"}},
      {"logbadentity", {"log", "--entity", "nocolon"}},
      {"logbadid", {"log", "--entity", "task:abc"}},
  };

  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, root, std::format("m_{}", tag));
    auto const ref  = run_pinned(zig_bin(), args, root, std::format("z_{}", tag));
    INFO("leaf: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(normalize(mine.out) == normalize(ref.out));
    CHECK(mine.err == ref.err);
  }

  // THE FIXTURE MUST HAVE REACHED THE INTERESTING STATES. Without this, a
  // port that returned empty output for everything would pass every
  // comparison above — two binaries agreeing on nothing is still agreement.
  auto const rendered = run_pinned(zig_bin(), std::vector<std::string>{"ps"}, root, "probe_ps");
  REQUIRE(rendered.code == 0);
  CHECK(rendered.out.contains("worktree:\xe2\x80\xa6"
                              "agent-42")); // the >40-char elision
  auto const forest = run_pinned(zig_bin(), std::vector<std::string>{"tree"}, root, "probe_tree");
  REQUIRE(forest.code == 0);
  CHECK(forest.out.contains("\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ")); // a non-last child
  CHECK(forest.out.contains("\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 ")); // a last child
  // The VERTICAL GUIDE at depth 2, under a depth-1 node that still has a
  // later sibling. This is the glyph a two-level fixture never reaches.
  CHECK(forest.out.contains("\xe2\x94\x82   \xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "));
  // The truncated activity summary, and specifically that the cut BACKED UP
  // off the em dash rather than splitting it: the body ends in two whole em
  // dashes followed by U+2026. A cut at byte 77 would leave a partial code
  // point here instead.
  CHECK(forest.out.contains("\xe2\x80\x94\xe2\x80\x94\xe2\x80\xa6\""));
  auto const ledger = run_pinned(zig_bin(), std::vector<std::string>{"claims", "--status", "all"}, root, "probe_claims");
  REQUIRE(ledger.code == 0);
  CHECK(ledger.out.contains("category:tool_failure"));
  auto const action_rows = run_pinned(zig_bin(), std::vector<std::string>{"actions"}, root, "probe_actions");
  REQUIRE(action_rows.code == 0);
  CHECK(action_rows.out.contains("entity:-:0")); // the entity-less action
  auto const roll_up = run_pinned(zig_bin(), std::vector<std::string>{"plans"}, root, "probe_plans");
  REQUIRE(roll_up.code == 0);
  CHECK(roll_up.out.contains("in_flight:yes"));
  CHECK(roll_up.out.contains("in_flight:no"));
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
  CHECK(mine->size() == 15);
  CHECK(mine->contains("planar-watch ps"));
  // The four task 6065 declared that the previous tree did not have at all.
  CHECK(mine->contains("planar-watch feed"));
  CHECK(mine->contains("planar-watch sync-events"));
  CHECK(mine->contains("planar-watch run list"));
  CHECK(mine->contains("planar-watch run show"));
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
  std::string const expected = R"CATALOG({"schemaVersion":1,"layout":"flat","root":"planar-watch","commands":[{"name":"planar-watch","aliases":[],"hidden":false,"deprecated":null,"path":[],"command":"planar-watch","summary":"Read-only viewer for live agent activity (feed / ps / claims / actions / plans / log / tree / run).","description":"planar-watch is the human-facing live cockpit for agent activity.\n\n  The default invocation with no args is the activity feed.\n  Subcommands narrow the view; `--follow` turns each one into a\n  streaming view that emits new rows as the underlying tables\n  change. The binary opens the database in strict read-only mode\n  (SQLITE_OPEN_READONLY) — every write SQL string is rejected by\n  the SQLite driver itself, the second line of defense behind\n  this binary's `no write verbs registered` capability boundary.\n\n  `tree` renders the orchestrator → sub-agent forest by walking\n  agent_actions.parent_action_id chains.","subcommands":["feed","ps","claims","actions","plans","log","tree","run","sync-events","version","completion","schema"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"feed","aliases":[],"hidden":false,"deprecated":null,"path":["feed"],"command":"planar-watch feed","summary":"Cross-cutting activity feed across all vendors (default verb).","description":"One event per claim transition, action transition, or task status\n  change, in occurrence-time order. The default planar-watch\n  invocation routes here.\n\n  Without --follow: print the initial snapshot up to --limit\n  events (default 100), newest first.\n  With --follow: print the snapshot, then stream new events as\n  they appear. Tier-1 poll; --interval defaults to 1s.\n\n  --tail N: emit the most-recent N events on first call (the\n  journalctl -f -n idiom). --tail 0 or negative exits with\n  InvalidValue. Combined with --follow: the tail emission comes\n  first, then only NEW events stream (no re-emit of tailed events).\n\n  Filters (--vendor / --plan / --task / --since) narrow both the\n  snapshot and the streaming view.\n\n  --json emits NDJSON — one JSON object per line, no surrounding\n  array, no trailing comma. Consumers can pipe through `jq -c`.","subcommands":[],"flags":[{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream new events until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Plan id filter (matches plan-direct, task-on-plan, and plan_step-on-plan events)","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Task id filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--since","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only events with at >= this ISO8601 timestamp","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Snapshot row cap (default 100)","completion":{"kind":"none","values":[]},"env":null},{"long":"--tail","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Return only the most-recent N events (must be > 0)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Emit NDJSON","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"ps","aliases":[],"hidden":false,"deprecated":null,"path":["ps"],"command":"planar-watch ps","summary":"Snapshot of active (and stale) agent claims.","description":"Lists every currently active agent claim — one row per claim_token.\n\n  --stale also includes claims whose lease has expired OR whose\n  status is `stale` (set by `planar-agent reconcile`).\n\n  --vendor / --plan narrow the result.\n\n  --sort-by heartbeat (default) orders by most-recently-heartbeated\n  first. --sort-by lease restores the pre-M3 claimed_at ordering.\n\n  --follow turns the snapshot into a streaming view (Tier-1 poll;\n  --interval defaults to 1s). Exits 0 on SIGINT.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by vendor (claude, codex, copilot, ...)","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)","completion":{"kind":"none","values":[]},"env":null},{"long":"--stale","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Include stale + lease-expired claims","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream snapshots until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s; e.g. 100ms)","completion":{"kind":"none","values":[]},"env":null},{"long":"--sort-by","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Sort order for active claims: heartbeat (default) or lease","completion":{"kind":"none","values":[]},"env":null},{"long":"--group-by","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Group claims by dimension: role, scope, or vendor","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"claims","aliases":[],"hidden":false,"deprecated":null,"path":["claims"],"command":"planar-watch claims","summary":"List claims in the agent_work_claims ledger (filterable by status).","description":"Returns claim rows from agent_work_claims. The default is\n  --status active.\n\n  --status active : claim row is in 'active' state with an\n                    unexpired lease (default).\n  --status stale  : status='stale' OR an expired-lease active\n                    claim (matches `ps --stale`).\n  --status all    : every row (active, released, completed,\n                    aborted, stale) — the full claim ledger.","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by vendor","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"active (default) | stale | all","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream snapshots until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"actions","aliases":[],"hidden":false,"deprecated":null,"path":["actions"],"command":"planar-watch actions","summary":"List agent_actions rows with optional filters.","description":"Returns agent_actions rows ordered by started_at descending.\n\n  --kind     : action_kind filter (coder, reviewer, tool_call, etc.).\n  --entity   : restrict to one entity, `kind:id` form (e.g. `task:42`).\n  --plan     : restrict to actions on the plan, or on tasks/plan_steps belonging to it.\n  --task     : restrict to actions whose entity_kind=task, entity_id=N.\n  --vendor   : vendor filter.\n  --limit    : cap row count (default 100).","subcommands":[],"flags":[{"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Vendor filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--kind","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"action_kind filter","completion":{"kind":"none","values":[]},"env":null},{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Restrict to one entity, kind:id form","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by task id","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Row cap (default 100)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream snapshots until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"plans","aliases":[],"hidden":false,"deprecated":null,"path":["plans"],"command":"planar-watch plans","summary":"List plans with in-flight agent work.","description":"Each row pairs a plan with its in-flight summary:\n    active_claims  — claims with status='active' and\n                     lease_expires_at >= now() targeting any task\n                     under the plan.\n    active_actions — agent_actions rows with ended_at IS NULL\n                     whose entity_kind/entity_id refer to a task\n                     under the plan.\n    last_event_at  — max of claim claimed_at / heartbeat /\n                     released_at and action started_at /\n                     ended_at across the plan's tasks; null\n                     when no events recorded.\n\n  --in-flight-only drops plans where active_claims=0 AND\n  active_actions=0.","subcommands":[],"flags":[{"long":"--in-flight-only","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Skip plans with no live work","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream snapshots until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"log","aliases":[],"hidden":false,"deprecated":null,"path":["log"],"command":"planar-watch log","summary":"Per-entity / per-claim history (union of agent actions and claim transitions).","description":"Streams the agent_actions + agent_work_claims history scoped to\n  one entity or one claim_token. Exactly one of\n  --task / --plan / --entity / --session / --claim is required.\n\n  Entries are emitted in occurrence-time order (oldest first)\n  as a discriminated union: each entry carries a `.kind` field\n  that is either `action` (full ActionRow payload) or\n  `claim_acquired` / `claim_heartbeat` / `claim_released` /\n  `claim_stale` (with ClaimRow payload).","subcommands":[],"flags":[{"long":"--task","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter to one task id","completion":{"kind":"none","values":[]},"env":null},{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter to one plan id (matches entity_kind=plan rows)","completion":{"kind":"none","values":[]},"env":null},{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter to one entity, kind:id form","completion":{"kind":"none","values":[]},"env":null},{"long":"--session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter to one session_id","completion":{"kind":"none","values":[]},"env":null},{"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter to one claim_token","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Row cap (default 100)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"tree","aliases":[],"hidden":false,"deprecated":null,"path":["tree"],"command":"planar-watch tree","summary":"Render the orchestrator → sub-agent action forest.","description":"Walks agent_actions.parent_action_id chains and renders the\n  orchestrator → sub-agent forest. Root rows have parent_action_id IS NULL.\n  Each child is indented with unicode tree characters (├── / └── / │).\n\n  --root-session <id>  scope to one session's subtree (error if unknown).\n  --follow             stream; re-renders on WAL change (Tier-2 wake).\n  --interval           maximum poll cadence for --follow (default 1s).\n\n  Each row shows the claim's: scope vendor activity worktree branch last_hb.","subcommands":[],"flags":[{"long":"--root-session","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Scope output to one session's subtree (session id)","completion":{"kind":"none","values":[]},"env":null},{"long":"--follow","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"Stream re-renders until SIGINT","completion":{"kind":"none","values":[]},"env":null},{"long":"--interval","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Poll interval for --follow (default 1s; e.g. 100ms)","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"run","aliases":[],"hidden":false,"deprecated":null,"path":["run"],"command":"planar-watch run","summary":"Observe workflow runs and their context records.","description":"Read-only view of run tables. `list` covers both workflow_runs (wf)\nand the runs table (op-arm); `show` drills into wf-source runs only.\n\n  list  — list runs (--plan / --status / --arm filters).\n  show  — drill into one wf-source run's context records.","subcommands":["list","show"],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"list","aliases":[],"hidden":false,"deprecated":null,"path":["run","list"],"command":"planar-watch run list","summary":"List workflow runs (filterable by plan, status, and source arm).","description":"Returns runs ordered by started_at descending.\n\n  --plan <id>    restrict to runs for the given plan.\n  --status <s>   restrict by status: running | completed | failed |\n                 interrupted | abandoned. Default: all.\n  --arm <a>      source table: wf (workflow_runs / context-plane),\n                 op (runs / op-arm), or all (default, both).\n  --json         emit a single JSON object instead of human text.","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--status","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by status (default: all)","completion":{"kind":"none","values":[]},"env":null},{"long":"--arm","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Source arm: wf | op | all (default: all)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"show","aliases":[],"hidden":false,"deprecated":null,"path":["run","show"],"command":"planar-watch run show","summary":"Show one workflow run plus its context_records grouped by stage.","description":"Returns the full workflow_runs row for <id> plus all\n  context_records for that run, grouped and ordered by\n  stage then created_at.\n\n  Exits non-zero when the run id is unknown.","subcommands":[],"flags":[{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[{"name":"id","kind":"string","required":true,"default":null,"description":"Workflow run id (integer)","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"sync-events","aliases":[],"hidden":false,"deprecated":null,"path":["sync-events"],"command":"planar-watch sync-events","summary":"List sync_events rows with optional filters (read-only).","description":"Returns sync_events rows ordered by `at` descending.\n\n  --plan     : restrict to events whose link belongs to the given plan id.\n  --system   : restrict to events via a link on the given external system slug.\n  --entity   : restrict to events via a link on one entity, `kind:id` form.\n  --outcome  : filter by outcome value (ok, conflict, error, noop, …).\n  --since    : only return rows with `at` >= this ISO8601 timestamp.\n  --limit    : cap row count (default 100).","subcommands":[],"flags":[{"long":"--plan","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Filter by plan id","completion":{"kind":"none","values":[]},"env":null},{"long":"--system","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by external system slug","completion":{"kind":"none","values":[]},"env":null},{"long":"--entity","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by entity, kind:id form (e.g. task:42)","completion":{"kind":"none","values":[]},"env":null},{"long":"--outcome","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Filter by outcome (ok, conflict, error, noop, …)","completion":{"kind":"none","values":[]},"env":null},{"long":"--since","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"string","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"VALUE","default":null,"description":"Only rows at >= this ISO8601 timestamp","completion":{"kind":"none","values":[]},"env":null},{"long":"--limit","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"int","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"N","default":null,"description":"Row cap (default 100)","completion":{"kind":"none","values":[]},"env":null},{"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,"kind":"bool","choices":[],"list":false,"count":false,"required":false,"source":"local","valueName":"","default":false,"description":"","completion":{"kind":"none","values":[]},"env":null}],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"version","aliases":[],"hidden":false,"deprecated":null,"path":["version"],"command":"planar-watch version","summary":"Print the planar-watch version, commit, and zig runtime.","description":"Print the planar-watch version, commit, and zig runtime.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"completion","aliases":[],"hidden":false,"deprecated":null,"path":["completion"],"command":"planar-watch completion","summary":"Generate the autocompletion script for the specified shell.","description":"Generate the autocompletion script for the specified shell.","subcommands":[],"flags":[],"flagGroups":[],"positionals":[{"name":"shell","kind":"string","required":true,"default":null,"description":"Shell: bash, zsh, or fish","completion":{"kind":"none","values":[]}}],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},{"name":"schema","aliases":[],"hidden":false,"deprecated":null,"path":["schema"],"command":"planar-watch schema","summary":"Print the full command tree as a JSON catalog (flags, aliases, positionals).","description":"Print the full command tree as a JSON catalog (flags, aliases, positionals).","subcommands":[],"flags":[],"flagGroups":[],"positionals":[],"docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],"authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}}]}
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
