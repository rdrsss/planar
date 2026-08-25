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

/// @brief Run both binaries over `args` in separately-pinned scratch roots.
/// @param tag A short discriminator naming the case.
/// @param args The arguments (excluding argv[0]).
/// @return The two captures, C++ first.
auto both(std::string_view tag, std::vector<std::string> args) -> std::pair<capture, capture> {
  auto const arena = make_arena(tag);
  return {run_pinned(cpp_bin(), args, arena.cpp_root, "cpp"), run_pinned(zig_bin(), args, arena.zig_root, "zig")};
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
  PLANAR_REQUIRE_ORACLE(oracle_available(), "Zig oracle not built (zig/zig-out/bin/planar-watch)");

  // Handler-level refusal: exit 2, stderr only. STILL A TRUE ORACLE DIFF
  // after task 6123 — this message comes from the HANDLER, not the parser,
  // so the CLI11 swap does not touch it. Left as a byte-for-byte
  // comparison deliberately: it is the control that shows the
  // re-baselining below is confined to parser-produced bytes.
  auto const [cpp_bad, zig_bad] = both("badshell", {"completion", "badshell"});
  CHECK(cpp_bad.code == zig_bad.code);
  CHECK(cpp_bad.code == 2);
  CHECK(cpp_bad.out == zig_bad.out);
  CHECK(cpp_bad.err == zig_bad.err);

  // Parser-level refusal: exit 1, BOTH streams. Two different codes out of
  // one verb — a collapsed exit mapping cannot satisfy both cases, and
  // THAT is what this case exists to prove.
  //
  // The exit CODE is still diffed against the oracle, because the per-binary
  // exit-code table is an operator contract task 6123 was required to
  // preserve. The BYTES are not: CLI11 writes the parse-error wording now
  // (task 6123), so they are pinned against the built binary instead.
  auto const [cpp_missing, zig_missing] = both("noshell", {"completion"});
  CHECK(cpp_missing.code == zig_missing.code);
  CHECK(cpp_missing.code == 1);
  CHECK(cpp_missing.out == "error: shell is required\n");
  CHECK(cpp_missing.err == "error: RequiredError\n");
}

TEST_CASE("planar-watch parity: an unknown verb still exits 1, matching the oracle", "[cmd][watch][parity]") {
  PLANAR_REQUIRE_ORACLE(oracle_available(), "Zig oracle not built (zig/zig-out/bin/planar-watch)");

  // As above: the exit CODE is still diffed against the oracle (exit 1
  // here, where the operator binary exits 2 — the divergence task 6123 was
  // required to preserve); the BYTES are CLI11's now and are pinned
  // against the built binary.
  auto const [cpp, zig] = both("unknownverb", {"nosuchverb"});
  CHECK(cpp.code == zig.code);
  CHECK(cpp.code == 1);
  CHECK(cpp.out == "error: planar-watch: The following argument was not expected: nosuchverb\n");
  CHECK(cpp.err == "error: ExtrasError\n");
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
  PLANAR_REQUIRE_ORACLE(oracle_available(), "Zig oracle not built (zig/zig-out/bin/planar-watch)");
  // TASK 6065. Before this task the tree carried nine of the oracle's
  // thirteen leaves, so `schema` was explicitly excluded from the compared
  // set (see this file's header). All thirteen are declared now, and this
  // is the case that says so.
  auto const [cpp, zig] = both("catalog", {"schema"});
  REQUIRE(cpp.code == 0);
  REQUIRE(zig.code == 0);

  auto const mine = planar::cmd::parity::parse_catalog(cpp.out);
  REQUIRE(mine.has_value());
  auto const theirs = planar::cmd::parity::parse_catalog(zig.out);
  REQUIRE(theirs.has_value());

  // Non-vacuous: an empty left-hand side, or a document that failed to
  // parse, would otherwise look exactly like a clean comparison.
  CHECK(mine->size() == 15);
  CHECK(theirs->size() == 15);
  CHECK(mine->contains("planar-watch ps"));
  // The four this task declared and the previous one did not have at all.
  CHECK(mine->contains("planar-watch feed"));
  CHECK(mine->contains("planar-watch sync-events"));
  CHECK(mine->contains("planar-watch run list"));
  CHECK(mine->contains("planar-watch run show"));

  auto const problems = planar::cmd::parity::diff_against_oracle(*mine, *theirs);
  INFO("declaration mismatches:\n" << std::format("{}", problems));
  CHECK(problems.empty());

  auto const missing = planar::cmd::parity::oracle_only_commands(*mine, *theirs);
  INFO("declared by the oracle and NOT by this binary:\n" << std::format("{}", missing));
  CHECK(missing.empty());
}

TEST_CASE("planar-watch parity: the catalog is BYTE-identical to the oracle's", "[cmd][watch][parity][catalog]") {
  PLANAR_REQUIRE_ORACLE(oracle_available(), "Zig oracle not built (zig/zig-out/bin/planar-watch)");
  // Everything `diff_against_oracle` leaves out — key order, `docs`,
  // `flagGroups`, `hidden`, `deprecated`, `path`, `name`, and the `default`
  // literal it deliberately skips — is covered here, for this binary, by
  // comparing the whole document.
  auto const [cpp, zig] = both("catalog-bytes", {"schema"});
  REQUIRE(cpp.code == 0);
  REQUIRE(zig.code == 0);
  REQUIRE(cpp.out.size() > 20000); // Not two empty strings.
  CHECK(cpp.out == zig.out);
}

TEST_CASE("planar-watch: a declared-but-unported verb refuses at exit 64", "[cmd][watch][parity][not-implemented]") {
  // The headline property of the full-surface declaration: DECLARING a
  // verb is not IMPLEMENTING it, and the difference must be loud. A
  // declared node that exits 0 is worse than an absent one.
  auto const arena = make_arena("unported");
  auto const run   = [&](std::vector<std::string> args, std::string_view tag) {
    return run_pinned(cpp_bin(), args, arena.cpp_root, tag);
  };

  auto const feed = run({"feed"}, "feed");
  CHECK(feed.code == 64);
  CHECK(feed.out.empty());
  CHECK(feed.err == "error: feed: not implemented in this build\n");

  auto const events = run({"sync-events"}, "syncevents");
  CHECK(events.code == 64);
  CHECK(events.err == "error: sync-events: not implemented in this build\n");

  // A nested one, to prove the key is the full path and not the leaf name.
  auto const run_list = run({"run", "list"}, "runlist");
  CHECK(run_list.code == 64);
  CHECK(run_list.err == "error: run list: not implemented in this build\n");

  // And the discrimination that makes the three above mean something: a
  // PORTED verb on the same binary does NOT answer 64, so "exit 64" is not
  // simply what this binary now does. Two of them, because they fail
  // differently: `version` needs nothing and exits 0, while `ps` reaches
  // the read-only database handle and exits 1 (`OpenFailed`) against an
  // empty arena — neither is the not-implemented code.
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
}
