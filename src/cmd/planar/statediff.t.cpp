// @file statediff.t.cpp
// @brief The STATE differential: run the C++ `planar` and the Zig oracle
// over one identical ordered sequence of verbs, in two pinned scratch
// arenas, and diff EVERY table of the two SQLite databases after every step
// (plan 996, task 6198).
//
// ## Why this lane exists at all
//
// `parity.t.cpp` beside this file compares exit code, stdout and stderr.
// That is the operator-visible surface, and it is worth what it costs. But
// NINE of the ten divergences found across M4-M9 did not live there — they
// lived in database state, several of them with byte-identical stdout and
// exit 0 on both sides. The `capture session` NULL-column defect and
// `entitylink`'s missing audit rows (green for four consecutive cycles)
// are the two that cost the most. The output lane is structurally blind to
// that whole class: a handler that renders correctly and writes the wrong
// rows passes it every time.
//
// This lane closed four more the first time it ran, over families that were
// already ported, already reviewed and already green (tasks 6199-6202).
//
// ## The seven behaviours, and why each one is load-bearing
//
// Every one of these exists because the naive version was unreadable or
// wrong. Do not simplify any of them away.
//
//  1. EVERY table, taken from `sqlite_master`, never a hand-listed set. A
//     new migration's table is covered the day it lands, without anyone
//     remembering to add it here. The FTS shadow tables come along for the
//     ride, which is intentional: an index that disagrees is a finding.
//
//  2. Volatility by SUFFIX (`*_at`) plus a SHORT exact list, never a
//     hand-maintained per-column enumeration. See `is_volatile`. Every
//     entry in the exact list is checked to still exist somewhere in the
//     schema (`the volatile-column exact list has no stale entries`), so a
//     column that is renamed or dropped fails this lane instead of quietly
//     becoming dead weight that normalizes nothing.
//
//     The reconnaissance note for this lane justified the suffix rule by
//     saying `checksum` is deliberately not volatile. There IS no `checksum`
//     column in this schema — `schema_migrations` is `(version, applied_at,
//     description)`, and `applied_at` is normalized by the suffix rule like
//     every other stamp. The rule stands; its stated justification named a
//     column that does not exist. See the guard case below, which pins both
//     the absence and the real near-miss (`dirty_at_claim`).
//
//  3. Scrub arena paths and ISO-8601 stamps out of stdout/stderr. Those are
//     the only two things that legitimately cannot agree between two
//     processes writing into two different scratch roots.
//
//  4. Report the ROW-LEVEL delta, never two table dumps. A table dump pair
//     is not a diagnosis; `+1 zig-only: {...}` is.
//
//  5. Report each distinct delta ONCE, at the step that INTRODUCED it.
//     Without this, one missing row reprints the whole table on every
//     subsequent step and the cause is indistinguishable from its echoes.
//
//  6. Collapse id-shift echoes. A missing row shifts every later
//     autoincrement id by a constant, so every later write differs from its
//     counterpart only by id. Those are paired by content-without-id and
//     COUNTED, not printed. On the current tree this is the difference
//     between 29 noisy steps and 4 real ones.
//
//  7. Exit 64 + "not implemented in this build" is UNPORTED, not divergent.
//     An unported leaf is a known gap. It is skipped for comparison and
//     reported separately — and the expected set is PINNED, so porting one
//     of them fails this lane until its entry is removed.
//
//     One refinement over the prototype: an unported step's state effects
//     are still absorbed into the `seen` set rather than skipped outright.
//     Otherwise anything the ORACLE wrote during a step the C++ tree cannot
//     run gets blamed on the next step, which is a false attribution to a
//     handler that did nothing wrong.
//
// ## Staging the known findings — why the list must fail BOTH ways
//
// The four divergences this lane found are real defects with their own
// tasks (6199-6202) and are not fixed here. They are listed in
// `known_divergences` with their EXACT observed detail, and the lane
// asserts the list in both directions:
//
//   - an observed divergence that is NOT listed fails (a new regression);
//   - a listed divergence that is NOT observed fails (a stale expectation).
//
// The second half is the one that matters. A one-directional allowlist rots
// into a permanent excuse; this one forces each finding's own task to
// remove its entry red-then-green, and forces this file to be revisited the
// moment any of the four behaviours changes for any reason.
//
// ## Database safety
//
// Everything here goes through `../parity_harness.hpp`'s `run_pinned` /
// `make_arena`. Read that file's header before touching this one: the `cd`
// then `env` ordering and the per-invocation capture files were each a real
// bug once, and the first of them wrote rows into the operator's live
// database. Nothing in this file opens a database except the two scratch
// ones, and it opens those READ-ONLY.

#include <catch2/catch_test_macros.hpp>

#include "parity_strict.hpp"

#include <sys/wait.h>

import std;
import planar.db;

#include "catalog_steps.hpp"
#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::arena;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;
namespace state_catalog = planar::cmd::state_catalog;

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the Zig reference binary (set by this target's CMakeLists).
/// @return The path.
auto zig_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_ZIG_BIN};
}

/// @brief True when the reference binary is present to diff against.
/// @return `true` if the oracle exists.
auto oracle_available() -> bool {
  return std::filesystem::exists(zig_bin());
}

// -------------------------------------------------------------------------
// Volatility
// -------------------------------------------------------------------------

/// @brief Column names that cannot agree across two processes and are NOT
/// spelled with the `_at` suffix.
///
/// Deliberately short, and every entry is justified by CONSTRUCTION rather
/// than by observation — a column blanked here is a column this lane stops
/// comparing, which is the one direction in which this file can weaken
/// itself. Entries:
///
///   `at`            `sync_events.at` is a wall-clock stamp whose name
///                   simply predates the `_at` convention. The suffix rule
///                   does not match a bare `at`, so it needs naming.
///   `fs_mtime`      `workbench_sync_state.fs_mtime` is a filesystem mtime
///                   of a file inside the arena.
///   `duration_ms`   `cli_invocations.duration_ms` is measured wall time.
///   `pid`           `workflow_runs.pid` is an OS process id.
///   `claim_token`   `agent_work_claims.claim_token` is randomly generated.
///   `run_uid`       `runs.run_uid` is randomly generated.
///   `preview_token` `routing_dispatch_previews.preview_token` likewise.
///
/// The prototype this lane was ported from listed `execution_time_ms`,
/// which does not exist in this schema and therefore normalized nothing.
/// That is exactly the rot `the volatile-column exact list has no stale
/// entries` below now makes impossible.
constexpr std::array<std::string_view, 7> volatile_exact{"at",          "fs_mtime", "duration_ms",  "pid",
                                                         "claim_token", "run_uid",  "preview_token"};

/// @brief Whether a column's value legitimately cannot agree across two
/// separate processes writing into two separate arenas.
///
/// Every wall-clock stamp this schema declares is spelled `<something>_at`,
/// so the SUFFIX carries almost all of the work without a hand-maintained
/// list that silently goes stale as migrations add columns.
/// @param column The column name.
/// @return `true` when the column must be blanked before comparison.
auto is_volatile(std::string_view column) -> bool {
  if (column.ends_with("_at")) {
    return true;
  }
  return std::ranges::contains(volatile_exact, column);
}

// -------------------------------------------------------------------------
// Scrubbing
// -------------------------------------------------------------------------

/// @brief Replace every occurrence of `needle` in `text` with `with`.
/// @param text The text to rewrite.
/// @param needle The substring to replace; ignored when empty.
/// @param with The replacement.
/// @return The rewritten text.
auto replace_all(std::string text, std::string_view needle, std::string_view with) -> std::string {
  if (needle.empty()) {
    return text;
  }
  std::string out;
  std::size_t at = 0;
  while (true) {
    auto const hit = text.find(needle, at);
    if (hit == std::string::npos) {
      out.append(text, at, std::string::npos);
      return out;
    }
    out.append(text, at, hit - at);
    out.append(with);
    at = hit + needle.size();
  }
}

/// @brief True when `text` from `at` begins an ISO-8601 instant of the shape
/// this codebase emits (`YYYY-MM-DDTHH:MM:SS[.fff][Z]`).
/// @param text The text to inspect.
/// @param at The offset to test.
/// @param length Receives the matched length when the result is `true`.
/// @return `true` on a match.
auto match_iso_instant(std::string_view text, std::size_t at, std::size_t& length) -> bool {
  auto const digits = [&](std::size_t offset, std::size_t count) {
    if (at + offset + count > text.size()) {
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (std::isdigit(static_cast<unsigned char>(text[at + offset + i])) == 0) {
        return false;
      }
    }
    return true;
  };
  auto const literal = [&](std::size_t offset, char expected) {
    return at + offset < text.size() && text[at + offset] == expected;
  };
  if (!digits(0, 4) || !literal(4, '-') || !digits(5, 2) || !literal(7, '-') || !digits(8, 2) || !literal(10, 'T') ||
      !digits(11, 2) || !literal(13, ':') || !digits(14, 2) || !literal(16, ':') || !digits(17, 2)) {
    return false;
  }
  std::size_t end = at + 19;
  if (end < text.size() && text[end] == '.') {
    std::size_t fraction = end + 1;
    while (fraction < text.size() && std::isdigit(static_cast<unsigned char>(text[fraction])) != 0) {
      ++fraction;
    }
    if (fraction > end + 1) {
      end = fraction;
    }
  }
  if (end < text.size() && text[end] == 'Z') {
    ++end;
  }
  length = end - at;
  return true;
}

/// @brief Remove the two things that legitimately differ between the two
/// runs: the arena root each binary was pinned to, and wall-clock stamps.
/// @param text The captured stdout or stderr.
/// @param space The arena both roots came from.
/// @return The scrubbed text.
auto scrub(std::string_view text, const arena& space) -> std::string {
  std::string work{text};
  work = replace_all(std::move(work), space.cpp_root.string(), "<ARENA>");
  work = replace_all(std::move(work), space.zig_root.string(), "<ARENA>");

  std::string out;
  out.reserve(work.size());
  for (std::size_t at = 0; at < work.size();) {
    std::size_t length = 0;
    if (match_iso_instant(work, at, length)) {
      out += "<TS>";
      at += length;
      continue;
    }
    out += work[at];
    ++at;
  }
  return out;
}

// -------------------------------------------------------------------------
// The dump
// -------------------------------------------------------------------------

constexpr char field_separator = '\x1f';

/// @brief Render `text` as the body of a C++ string literal: single-line,
/// unambiguous, and directly pasteable into `known_divergences`.
///
/// Every `detail` this file builds goes through this, so a staged
/// expectation is a literal a reader can compare against the failure report
/// character for character. Without it the `\x1f` field separators render as
/// nothing and `id=6verb='create'entity_kind=` is what the operator sees.
/// @param text The raw text.
/// @return The escaped text.
auto escape(std::string_view text) -> std::string {
  std::string out;
  for (char const c : text) {
    switch (c) {
    case '\\':
      out += R"(\\)";
      break;
    case '"':
      out += R"(\")";
      break;
    case '\n':
      out += R"(\n)";
      break;
    case '\t':
      out += R"(\t)";
      break;
    case '\r':
      out += R"(\r)";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += std::format(R"(\x{:02x})", static_cast<unsigned>(static_cast<unsigned char>(c)));
      } else {
        out += c;
      }
    }
  }
  return out;
}

/// @brief One database row, canonically serialized.
struct row {
  std::string full;    ///< `name=quoted` for every column, `\x1f`-joined.
  std::string sans_id; ///< The same, with the `id` column omitted (see behaviour 6).
};

/// @brief One table's normalized contents, or the error that prevented
/// reading it.
struct table_dump {
  std::vector<row> rows;  ///< Sorted, so comparison is order-insensitive.
  std::string      error; ///< Non-empty when the table could not be read.
};

/// @brief A whole database: table name to contents.
using db_dump = std::map<std::string, table_dump>;

/// @brief Read every user table of `path`, normalized for comparison.
///
/// Values are serialized through SQLite's own `quote()`, which renders NULL,
/// integers, reals, `'text'` and `X'blob'` distinguishably — so a column that
/// changed TYPE without changing its printed form is still a divergence, and
/// blob-valued FTS shadow tables serialize without special handling.
/// @param path The database file; a missing file yields an empty dump.
/// @param roots The arena roots to rewrite to `<ARENA>`.
/// @return The dump.
auto dump_database(const std::filesystem::path& path, std::span<const std::string> roots) -> db_dump {
  db_dump out;
  if (!std::filesystem::exists(path)) {
    return out;
  }
  auto opened = planar::db::connection::open_read_only(path.string());
  if (!opened) {
    out["<open>"] = table_dump{.rows = {}, .error = opened.error().message_};
    return out;
  }
  auto& connection = *opened;

  std::vector<std::string> tables;
  {
    auto listing = connection.prepare("select name from sqlite_master where type = 'table' "
                                      "and name not like 'sqlite_%' order by name");
    if (!listing) {
      out["<sqlite_master>"] = table_dump{.rows = {}, .error = listing.error().message_};
      return out;
    }
    while (true) {
      auto stepped = listing->step();
      if (!stepped || *stepped == planar::db::step_result::done) {
        break;
      }
      tables.push_back(listing->column_text(0));
    }
  }

  for (auto const& table : tables) {
    std::vector<std::string> columns;
    auto                     info = connection.prepare(std::format(R"(pragma table_info("{}"))", table));
    if (!info) {
      out[table] = table_dump{.rows = {}, .error = info.error().message_};
      continue;
    }
    while (true) {
      auto stepped = info->step();
      if (!stepped || *stepped == planar::db::step_result::done) {
        break;
      }
      columns.push_back(info->column_text(1));
    }
    if (columns.empty()) {
      out[table] = table_dump{};
      continue;
    }

    std::string select = "select ";
    for (std::size_t i = 0; i < columns.size(); ++i) {
      select += i == 0 ? "" : ", ";
      select += std::format(R"(quote("{}"))", columns[i]);
    }
    select += std::format(R"( from "{}")", table);

    auto query = connection.prepare(select);
    if (!query) {
      out[table] = table_dump{.rows = {}, .error = query.error().message_};
      continue;
    }
    table_dump dumped;
    while (true) {
      auto stepped = query->step();
      if (!stepped) {
        dumped.error = stepped.error().message_;
        break;
      }
      if (*stepped == planar::db::step_result::done) {
        break;
      }
      row built;
      for (std::size_t i = 0; i < columns.size(); ++i) {
        std::string value = query->column_text(static_cast<int>(i));
        if (is_volatile(columns[i])) {
          if (value != "NULL") {
            value = "<volatile>";
          }
        } else {
          for (auto const& root : roots) {
            value = replace_all(std::move(value), root, "<ARENA>");
          }
        }
        auto const field = std::format("{}={}", columns[i], value);
        if (!built.full.empty()) {
          built.full += field_separator;
        }
        built.full += field;
        if (columns[i] != "id") {
          if (!built.sans_id.empty()) {
            built.sans_id += field_separator;
          }
          built.sans_id += field;
        }
      }
      dumped.rows.push_back(std::move(built));
    }
    std::ranges::sort(dumped.rows, {}, &row::full);
    out[table] = std::move(dumped);
  }
  return out;
}

/// @brief Multiset difference: the rows of `left` that `right` does not also
/// contain, counting duplicates.
/// @param left The side to take rows from.
/// @param right The side to subtract.
/// @return `left` minus `right`.
auto multiset_minus(const std::vector<row>& left, const std::vector<row>& right) -> std::vector<row> {
  std::map<std::string, int> counts;
  for (auto const& entry : right) {
    ++counts[entry.full];
  }
  std::vector<row> out;
  for (auto const& entry : left) {
    auto found = counts.find(entry.full);
    if (found != counts.end() && found->second > 0) {
      --found->second;
      continue;
    }
    out.push_back(entry);
  }
  return out;
}

// -------------------------------------------------------------------------
// Findings
// -------------------------------------------------------------------------

/// @brief One divergence, keyed so it can be matched against the staged
/// known-divergence list.
struct finding {
  std::string tag;     ///< The step that introduced it.
  std::string channel; ///< `exit`, `stdout`, `stderr` or `state:<table>`.
  std::string detail;  ///< The exact observed delta.
};

/// @brief Render a finding as one copy-pasteable `known_divergences` entry,
/// so re-baselining after an intentional change is mechanical rather than
/// transcribed by hand.
///
/// `detail` is already escaped by construction (see `escape`), so this only
/// has to wrap it.
/// @param observed The finding.
/// @return The rendered entry.
auto as_entry(const finding& observed) -> std::string {
  return std::format(R"(      {{"{}", "{}", "{}"}},)", observed.tag, observed.channel, observed.detail);
}

// -------------------------------------------------------------------------
// The sequence
// -------------------------------------------------------------------------

/// @brief One verb invocation in the ordered sequence.
struct step {
  std::vector<std::string> args; ///< The argv tail.
};

/// @brief The ordered sequence, run against ONE database per binary.
///
/// A fresh arena per step would only ever compare two empty databases; the
/// point is that `question answer` sees the question `question add` wrote
/// three steps earlier, and that a row missing from step 13 is still
/// missing at step 40.
///
/// TWO ARGV SHAPES WERE CORRECTED WHILE PORTING THIS, and the correction is
/// the reason this lane reports four findings rather than seven. The
/// reconnaissance step list carried `assoc add demo --path .` and
/// `audit list --json`. Neither `--path` on `assoc add` (which takes two
/// POSITIONALS, `<slug> <repo-path>`) nor an `audit list` subcommand
/// (`audit` has trail/commits/session/publish-decision/handoff-readiness)
/// exists on EITHER side. Both were therefore parse failures on both
/// binaries — which registered as three "sanctioned CLI11 re-baselining"
/// divergences while in fact exercising no handler at all. They are
/// replaced here by the nearest valid verbs.
///
/// `init` deliberately keeps `--skip-project`, so the cwd resolves to NO
/// registered scope. That is not an oversight: it is what exposed finding
/// 6200, a divergence in the ORDER `--status` and scope are validated in,
/// which is invisible once a scope resolves.
///
/// The same property is why this lane can only ever be the ALARM and not
/// the diagnosis. Running exclusively outside a scope, it cannot see the
/// in-scope half of any ordering question — which is how 6201 came to be
/// filed as an exit-code defect when the exit code was correct all along.
/// Every finding here needs a pinned-arena probe of BOTH halves before it
/// earns a fix.
auto hand_authored_stateful_sequence() -> std::vector<step> {
  return {
      {{"init", "--skip-project", "--allow-no-repo", "--json"}},

      // `assoc create` rather than the reconnaissance list's `assoc add
      // demo --path .`, which named a flag neither binary declares.
      {{"assoc", "create", "demo", "--kind", "project", "--json"}},
      {{"assoc", "list", "--json"}},

      {{"plan", "create", "First plan", "--slug", "first-plan", "--json"}},
      {{"plan", "create", "Second plan", "--slug", "second-plan", "--status", "draft", "--json"}},
      {{"plan", "list", "--json"}},
      {{"plan", "show", "1", "--json"}},

      {{"task", "add", "Task one", "--plan", "1", "--slug", "task-one", "--no-editor", "--json"}},
      {{"task", "add", "Task two", "--plan", "1", "--slug", "task-two", "--no-editor", "--json"}},
      {{"task", "list", "--json"}},
      {{"task", "list", "--plan", "1", "--json"}},
      {{"task", "list", "--status", "todo", "--json"}},
      {{"task", "show", "1", "--json"}},

      {{"question", "add", "Q one", "--body", "why", "--json"}},
      {{"question", "add", "Q two", "--body", "how", "--plan", "1", "--json"}},
      {{"question", "list", "--json"}},
      {{"question", "answer", "1", "--answer", "because", "--json"}},
      {{"question", "list", "--json"}},

      {{"decision", "add", "D one", "--body", "chose x", "--json"}},
      {{"decision", "add", "D two", "--body", "chose y", "--plan", "1", "--json"}},
      {{"decision", "list", "--json"}},

      {{"scenario", "add", "S one", "--body", "given when then", "--json"}},
      {{"scenario", "add", "S two", "--body", "given when then", "--plan", "1", "--json"}},
      {{"scenario", "list", "--json"}},

      {{"annotate", "add", "--anchor-path", "src/foo.zig", "--line-start", "3", "--line-end", "7", "--title", "T1", "--body",
        "B1", "--tags", "a, b,a"}},
      {{"annotate", "list", "--json"}},

      {{"links", "add", "task:1", "task:2", "--relationship", "depends-on"}},
      {{"links", "list", "task:1", "--json"}},

      // `audit trail` rather than the reconnaissance list's `audit list`,
      // which is not a subcommand on either side.
      {{"audit", "trail", "task:1", "--json"}},

      // The empty-`--status` shapes. Five families; `task` and `decision`
      // diverged here until task 6200.
      //
      // READ THIS BEFORE CONCLUDING ANYTHING FROM THESE FIVE STEPS. The
      // empty string is the WEAKEST probe of the five-family ordering, and
      // taking it at face value is what made 6200's own remedy wrong. It
      // reports "plan/question/scenario resolve scope first, task/decision
      // do not", and that is NOT what the oracle does. The three siblings
      // comma-SPLIT `--status`, so `""` yields zero tokens and never
      // reaches their validator at all; they fall through to the scope
      // error while still validating status FIRST whenever a token exists.
      // `--status bogus` from outside a scope separates them cleanly:
      //
      //     plan/question/scenario -> error: unknown status 'bogus'
      //     task/decision          -> error: cwd is not inside any ...
      //
      // So the oracle genuinely carries TWO orderings, and `task`/`decision`
      // are the odd pair — the opposite of the sibling-conformance story.
      // The `..._list scope/status ordering` cases in `handlers.t.cpp` pin
      // all ten cells directly so this can never be re-derived from `""`.
      {{"plan", "list", "--status", "", "--json"}},
      {{"task", "list", "--status", "", "--json"}},
      {{"question", "list", "--status", "", "--json"}},
      {{"decision", "list", "--status", "", "--json"}},
      {{"scenario", "list", "--status", "", "--json"}},

      // Dangling parent ids: the foreign-key refusal path, one per family.
      {{"decision", "add", "D dangle", "--body", "b", "--plan", "9999", "--json"}},
      {{"scenario", "add", "S dangle", "--body", "b", "--plan", "4242", "--json"}},
      {{"question", "add", "Q dangle", "--body", "b", "--plan", "7777", "--json"}},
      {{"task", "add", "T dangle", "--plan", "8888", "--slug", "t-dangle", "--no-editor", "--json"}},

      {{"audit", "trail", "task:1", "--json"}},
      {{"plan", "recompute-status", "--all", "--json"}},
      {{"plan", "list", "--json"}},

      // `plan closeout` (task 6317) runs LAST, and it is the only step in
      // this sequence that can close a plan. Ordering it here keeps its
      // writes out of every earlier step's post-state; the three steps are
      // ordered refusal -> preview -> apply so the apply's `plans` UPDATE
      // and `audit_log` row are the last state either binary produces.
      //
      // THE TWO PLANS ARE DIFFERENT ON PURPOSE, and a break-probe is why.
      // Plan 2 has NO tasks, so it is READY: pointing the `--dry-run` step at
      // it too would have exercised the ready arm twice and the BLOCKED arm
      // never. Probing that — mutating the `open task(s)` reason string —
      // produced a SURVIVOR against an earlier version of this list, which is
      // exactly the vacuous coverage the probe exists to expose. Plan 1 keeps
      // its two `todo` tasks (nothing in this sequence transitions them), so
      // it is genuinely blocked and the preview renders a reason.
      //
      // The preview step is also the only one that would catch a divergence
      // in the blocked-preview EXIT CODE, which the oracle answers 0.
      //
      // NOTE for whoever extends this list: no claim in this arena carries a
      // `repo_root`, so both binaries emit the identical synthetic `(none)`
      // git-evidence entry and this lane stays green. A step that SEEDS a
      // locality-bearing claim will diverge on the advisory git fields BY
      // DESIGN — read `src/lib/engine/planning/closeout.cppm`'s DIVERGENCE
      // section before filing it as a defect. The hard gate, the exit code
      // and every table this lane diffs are unaffected either way.
      {{"plan", "closeout", "9999", "--json"}},
      {{"plan", "closeout", "1", "--dry-run", "--json"}},
      {{"plan", "closeout", "2", "--json"}},
  };
}

/// @brief Refusals whose deliberately incomplete argv is itself the contract.
///
/// They are intentionally not part of the generated inventory: catalog
/// derivation only produces complete leaf paths.  Keep these separate from
/// the stateful ordering above so the coverage report cannot count a parse
/// refusal as a successful generated leaf exercise.
auto hand_authored_malformed_argv_refusals() -> std::vector<step> {
  return {{{"plan", "show"}}}; // `show` requires its plan id.
}

/// @brief Combine explicit dependent/refusal cases with catalog-derived leaves.
/// @param cpp_catalog The C++ `schema` output.
/// @param zig_catalog The Zig `schema` output.
/// @param error Receives a fail-closed catalog diagnostic.
/// @return The full lane sequence, or unset before a subject verb runs.
auto sequence(std::string_view cpp_catalog, std::string_view zig_catalog, std::string& error)
    -> std::optional<std::vector<step>> {
  auto generated = state_catalog::generated_steps(cpp_catalog, zig_catalog, error);
  if (!generated || !state_catalog::verify_inventory(*generated, cpp_catalog, zig_catalog, error)) {
    return std::nullopt;
  }

  auto out = hand_authored_stateful_sequence();
  for (auto const& item : *generated) {
    out.push_back({item.args});
  }
  return out;
}

/// @brief Build a minimal schema document for catalog-conversion guards.
/// @param paths One path-token array per leaf.
/// @return A valid catalog containing optional-only leaf entries.
auto catalog_fixture(std::span<const std::vector<std::string>> paths) -> std::string {
  std::string document = R"({"commands":[)";
  for (std::size_t i = 0; i < paths.size(); ++i) {
    auto encoded = glz::write_json(paths[i]);
    REQUIRE(encoded.has_value());
    if (i != 0) {
      document += ',';
    }
    document += std::format(R"({{"path":{},"subcommands":[],"positionals":[],"flags":[]}})", *encoded);
  }
  return document + "]}";
}

/// @brief The leaves this binary answers with exit 64 + "not implemented in
/// this build", pinned by step tag.
///
/// Behaviour 7 skips these for comparison, and pinning the SET is what stops
/// the skip from becoming permanent: porting one of them fails this lane
/// until its entry is removed here, which is the same red-then-green
/// discipline `known_divergences` applies to the defects.
/// THREE before task 6262 ported `audit trail`. `s28` and `s38` are its two
/// steps and they left this set together — which is the red-then-green this
/// list exists for. They did NOT stop being compared: they now run as fully
/// compared steps and agree, because `audit trail task:1` is a non-integer
/// id on both sides and both answer exit 2 with the same wording. A port
/// that had changed that refusal would have turned this removal into a
/// divergence rather than a silent pass.
/// THE SET IS NOW EMPTY, as of task 6279. `s02` is `assoc list --json`,
/// the last entry, and it left when the leaf was wired. Emptying it does
/// NOT retire the assertion — it strengthens it: checked in both
/// directions against an empty expectation, ANY step that starts answering
/// exit 64 + "not implemented in this build" now fails this lane
/// immediately, which is exactly the regression an inventory that could
/// only shrink was blind to.
///
/// `s02` did not stop being compared either. It now runs as a fully
/// compared step over all three channels plus the state tables, and the
/// port had to agree on the empty-`--kind`-absent listing, the `[]` JSON
/// for the one row `s01` created, and the `associations` table state — a
/// port that had changed any of them would have turned this removal into a
/// divergence rather than a silent pass.
constexpr std::array<std::string_view, 0> expected_unported{};

/// @brief One staged divergence: a real defect with its own task, listed so
/// this lane is green while the defect stands.
struct known {
  std::string_view tag;     ///< The step that introduces it.
  std::string_view channel; ///< The channel it appears on.
  std::string_view detail;  ///< The exact expected delta.
};

/// @brief The staged, known-divergence allowlist for this differential.
///
/// Removing an entry is how the corresponding task goes red-then-green. The
/// list is asserted in BOTH directions — see this file's header.
///
/// Task 6191 removed the final `s13` session-start audit difference, so the
/// list is intentionally empty. Its bidirectional assertion catches both a
/// reintroduced difference and an accidentally stale allowance.
const std::vector<known>& known_divergences() {
  static const std::vector<known> staged{};
  return staged;
}

} // namespace

TEST_CASE("the volatile-column exact list has no stale entries", "[cmd][parity][state]") {
  // A guard on THIS FILE, not on either binary. Every name in
  // `volatile_exact` blanks a column before comparison, so a name that no
  // longer matches any column is a normalization that silently stopped
  // applying — and the way that reads from outside is "the lane is still
  // green", which is the failure mode this whole lane exists to end. The
  // prototype carried exactly one such entry (`execution_time_ms`).
  //
  // The schema comes from a freshly migrated scratch database, so this also
  // fails the day a migration renames one of the seven.
  auto const space = make_arena("statediff_volatile");
  auto const got =
      run_pinned(cpp_bin(), std::array<std::string, 3>{"init", "--skip-project", "--allow-no-repo"}, space.cpp_root, "vol");
  REQUIRE(got.code == 0);
  REQUIRE(std::filesystem::exists(space.cpp_root / "planar.db"));

  auto opened = planar::db::connection::open_read_only((space.cpp_root / "planar.db").string());
  REQUIRE(opened.has_value());

  std::set<std::string> declared;
  auto                  listing = opened->prepare("select name from sqlite_master where type = 'table' "
                                                  "and name not like 'sqlite_%' order by name");
  REQUIRE(listing.has_value());
  std::vector<std::string> tables;
  while (true) {
    auto stepped = listing->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    tables.push_back(listing->column_text(0));
  }
  REQUIRE_FALSE(tables.empty());
  for (auto const& table : tables) {
    auto info = opened->prepare(std::format(R"(pragma table_info("{}"))", table));
    if (!info) {
      continue;
    }
    while (true) {
      auto stepped = info->step();
      if (!stepped || *stepped == planar::db::step_result::done) {
        break;
      }
      declared.insert(info->column_text(1));
    }
  }

  for (auto const& name : volatile_exact) {
    INFO("volatile_exact entry: " << name);
    CHECK(declared.contains(std::string{name}));
  }

  // The counterexample that keeps the suffix rule ANCHORED. This schema
  // really does declare a column that CONTAINS `_at` and is not a stamp —
  // `agent_work_claims.dirty_at_claim`, a boolean recording whether the
  // worktree was dirty at claim time. A rule spelled `contains("_at")`
  // rather than `ends_with("_at")` would blank it and stop comparing a real
  // value; this pins the anchoring against a column that actually exists.
  CHECK(declared.contains("dirty_at_claim"));
  CHECK_FALSE(is_volatile("dirty_at_claim"));
  CHECK(is_volatile("recorded_at"));

  // AND THE CLAIM THIS REPLACED, recorded because it was believed and is
  // false: the reconnaissance note for this lane justified the suffix rule
  // by saying `checksum` is deliberately NOT volatile, "because migrations
  // are shared files, so a checksum divergence is a real finding". Planar's
  // `schema_migrations` is `(version, applied_at, description)` — there is
  // no `checksum` column anywhere in this schema, in either tree's db
  // layer, or in any migration. That is an sqlx-cli concept Planar does not
  // use. The rule is right; the reason given for it described a column that
  // does not exist, and an assertion resting on it fails.
  CHECK_FALSE(declared.contains("checksum"));
}

TEST_CASE("state catalog: installed eligible inventory is an exact C++/Zig bijection", "[cmd][parity][state][catalog]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `zig build` in zig/ to enable the parity lane");

  auto const space = make_arena("statediff_catalog");
  auto const cpp   = run_pinned(cpp_bin(), std::array<std::string, 1>{"schema"}, space.cpp_root, "catalog_cpp");
  auto const zig   = run_pinned(zig_bin(), std::array<std::string, 1>{"schema"}, space.zig_root, "catalog_zig");
  REQUIRE(cpp.code == 0);
  REQUIRE(zig.code == 0);

  std::string error;
  auto        generated = state_catalog::generated_steps(cpp.out, zig.out, error);
  REQUIRE(generated.has_value());
  INFO("eligible generated leaf count: " << generated->size());
  REQUIRE(state_catalog::verify_inventory(*generated, cpp.out, zig.out, error));
}

TEST_CASE("state catalog: invented and omitted leaves fail the inventory gate", "[cmd][parity][state][catalog]") {
  std::vector<std::vector<std::string>> const paths{{"one"}, {"two"}};
  auto const                                  catalog = catalog_fixture(paths);
  std::string                                 error;
  auto                                        generated = state_catalog::generated_steps(catalog, catalog, error);
  REQUIRE(generated.has_value());
  REQUIRE(generated->size() == 2);

  auto invented = *generated;
  invented.push_back({R"(["invented"])", {"invented"}});
  CHECK_FALSE(state_catalog::verify_inventory(invented, catalog, catalog, error));
  CHECK(error.contains("invented"));

  auto omitted = *generated;
  omitted.pop_back();
  CHECK_FALSE(state_catalog::verify_inventory(omitted, catalog, catalog, error));
  CHECK(error.contains("missing"));
}

TEST_CASE("state catalog: C++ and Zig eligibility disagreement fails before steps run", "[cmd][parity][state][catalog]") {
  std::vector<std::vector<std::string>> const cpp_paths{{"one"}, {"two"}};
  std::vector<std::vector<std::string>> const zig_paths{{"one"}};
  std::string                                 error;
  CHECK_FALSE(state_catalog::generated_steps(catalog_fixture(cpp_paths), catalog_fixture(zig_paths), error).has_value());
  CHECK(error.contains("eligible catalog inventory differs"));
}

TEST_CASE("state catalog: argv boundaries remain discrete and malformed input fails closed", "[cmd][parity][state][catalog]") {
  auto const                     arena     = make_arena("statediff_argv");
  auto const                     evaluated = arena.cpp_root / "was-evaluated";
  std::vector<std::string> const raw{
      "leaf", "has space", "single'quote", "double\"quote", "", ",;|:", std::format("$(touch {})", evaluated.string())};
  std::vector<std::vector<std::string>> const paths{raw};
  auto const                                  catalog = catalog_fixture(paths);

  std::string error;
  auto        generated = state_catalog::generated_steps(catalog, catalog, error);
  REQUIRE(generated.has_value());
  REQUIRE(generated->size() == 1);
  CHECK(generated->front().args == raw);

  // `run_pinned` is the only shell boundary in this lane.  Its input is the
  // generated vector itself, and the witness proves that every element stays
  // one argv value while the shell metacharacters stay data, not code.
  auto const witness = arena.cpp_root / "proj" / "argv-witness";
  auto const script  = arena.cpp_root / "proj" / "capture-argv.sh";
  {
    std::ofstream out(script);
    REQUIRE(out.good());
    out << "#!/bin/sh\nprintf '%s\\n' \"$@\" > " << planar::cmd::parity::shell_quote(witness.string()) << "\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
  auto const captured = run_pinned(script, generated->front().args, arena.cpp_root, "argv_boundaries");
  REQUIRE(captured.code == 0);
  std::string expected;
  for (auto const& value : raw) {
    expected += value + '\n';
  }
  CHECK(planar::cmd::parity::read_all(witness) == expected);
  CHECK_FALSE(std::filesystem::exists(evaluated));

  // Conversion accepts catalog bytes, not a shell command.  A parse or
  // shape failure returns before the state lane can launch either binary.
  CHECK_FALSE(state_catalog::generated_steps(R"({"commands":[)", catalog, error).has_value());
  CHECK(error.contains("malformed"));
  CHECK_FALSE(state_catalog::detail::leaves(R"({"commands":[)", error).has_value());
  CHECK_FALSE(state_catalog::generated_steps(R"({"commands":[{"path":[1],"subcommands":[],"positionals":[],"flags":[]}]})",
                                             catalog, error)
                  .has_value());
  CHECK(error.contains("non-string"));
}

TEST_CASE("state catalog: hand-authored stateful and malformed exceptions are excluded from generated coverage",
          "[cmd][parity][state][catalog]") {
  auto const stateful  = hand_authored_stateful_sequence();
  auto const malformed = hand_authored_malformed_argv_refusals();
  REQUIRE_FALSE(stateful.empty());
  REQUIRE_FALSE(malformed.empty());
  CHECK(malformed.front().args == std::vector<std::string>{"plan", "show"});

  auto const  catalog = R"({"commands":[
    {"path":["plan","show"],"subcommands":[],"positionals":[{"required":true}],"flags":[]},
    {"path":["config","edit"],"subcommands":[],"positionals":[],"flags":[]},
    {"path":["list"],"subcommands":[],"positionals":[],"flags":[]}
  ]})";
  std::string error;
  auto        generated = state_catalog::generated_steps(catalog, catalog, error);
  REQUIRE(generated.has_value());
  CHECK(std::ranges::none_of(*generated, [&](const state_catalog::step& item) { return item.args == malformed.front().args; }));
  CHECK(std::ranges::none_of(
      *generated, [](const state_catalog::step& item) { return item.args == std::vector<std::string>{"config", "edit"}; }));
}

TEST_CASE("C++ and Zig agree on DATABASE STATE across an ordered planning sequence", "[cmd][parity][state]") {
  PLANAR_REQUIRE_ORACLE(
      oracle_available(),
      "zig reference binary not built (zig/zig-out/bin/planar) — run `zig build` in zig/ to enable the parity lane");

  auto const                     space = make_arena("statediff");
  std::vector<std::string> const roots{space.cpp_root.string(), space.zig_root.string()};

  std::vector<finding>       observed;
  std::vector<std::string>   unported;
  std::set<std::string>      seen;            // table \x01 side \x01 row
  std::map<std::string, int> id_shift_echoes; // table -> collapsed row count

  auto const cpp_catalog = run_pinned(cpp_bin(), std::array<std::string, 1>{"schema"}, space.cpp_root, "state_catalog_cpp");
  auto const zig_catalog = run_pinned(zig_bin(), std::array<std::string, 1>{"schema"}, space.zig_root, "state_catalog_zig");
  REQUIRE(cpp_catalog.code == 0);
  REQUIRE(zig_catalog.code == 0);
  std::string catalog_error;
  auto const  steps = sequence(cpp_catalog.out, zig_catalog.out, catalog_error);
  REQUIRE(steps.has_value());
  INFO("catalog-derived state inventory: " << (steps->size() - hand_authored_stateful_sequence().size()) << " leaves");
  for (std::size_t index = 0; index < steps->size(); ++index) {
    auto const& args = (*steps)[index].args;
    auto const  tag  = std::format("s{:02}", index);
    INFO("step " << tag << ": planar " << std::format("{}", args));

    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);

    auto const mine_err = scrub(mine.err, space);
    auto const ref_err  = scrub(ref.err, space);

    // Behaviour 7. The state effects are still absorbed below so the
    // oracle's writes during a step this binary cannot run are not blamed
    // on the next step.
    bool const is_unported = mine.code == 64 && mine.err.contains("not implemented in this build");
    if (is_unported) {
      unported.push_back(tag);
    }

    if (!is_unported) {
      if (mine.code != ref.code) {
        observed.push_back({tag, "exit", std::format("cpp={} zig={}", mine.code, ref.code)});
      }
      auto const mine_out = scrub(mine.out, space);
      auto const ref_out  = scrub(ref.out, space);
      if (mine_out != ref_out) {
        observed.push_back({tag, "stdout", std::format("cpp=[{}] zig=[{}]", escape(mine_out), escape(ref_out))});
      }
      if (mine_err != ref_err) {
        observed.push_back({tag, "stderr", std::format("cpp=[{}] zig=[{}]", escape(mine_err), escape(ref_err))});
      }
    }

    auto const cpp_state = dump_database(space.cpp_root / "planar.db", roots);
    auto const zig_state = dump_database(space.zig_root / "planar.db", roots);

    std::set<std::string> names;
    for (auto const& [table, _] : cpp_state) {
      names.insert(table);
    }
    for (auto const& [table, _] : zig_state) {
      names.insert(table);
    }

    for (auto const& table : names) {
      static const table_dump empty;
      auto const&             left  = cpp_state.contains(table) ? cpp_state.at(table) : empty;
      auto const&             right = zig_state.contains(table) ? zig_state.at(table) : empty;

      if (left.error != right.error) {
        if (!is_unported) {
          observed.push_back({tag, std::format("state:{}", table),
                              std::format("read error cpp=[{}] zig=[{}]", escape(left.error), escape(right.error))});
        }
        continue;
      }

      auto only_cpp = multiset_minus(left.rows, right.rows);
      auto only_zig = multiset_minus(right.rows, left.rows);

      // Behaviour 5: a delta is reported once, at the step that introduced
      // it. Everything already seen is an echo of an earlier finding.
      auto const fresh = [&](std::vector<row>& rows, std::string_view side) {
        std::vector<row> kept;
        for (auto& entry : rows) {
          auto const key = std::format("{}\x01{}\x01{}", table, side, entry.full);
          if (seen.contains(key)) {
            continue;
          }
          seen.insert(key);
          kept.push_back(std::move(entry));
        }
        return kept;
      };
      auto fresh_cpp = fresh(only_cpp, "cpp");
      auto fresh_zig = fresh(only_zig, "zig");
      if (fresh_cpp.empty() && fresh_zig.empty()) {
        continue;
      }

      // Behaviour 6: pair rows that differ ONLY by id and count them.
      std::map<std::string, std::vector<row>> zig_by_content;
      for (auto& entry : fresh_zig) {
        zig_by_content[entry.sans_id].push_back(std::move(entry));
      }
      int              shifted = 0;
      std::vector<row> real_cpp;
      for (auto& entry : fresh_cpp) {
        auto bucket = zig_by_content.find(entry.sans_id);
        if (bucket != zig_by_content.end() && !bucket->second.empty()) {
          bucket->second.pop_back();
          ++shifted;
          continue;
        }
        real_cpp.push_back(std::move(entry));
      }
      std::vector<row> real_zig;
      for (auto& [_, bucket] : zig_by_content) {
        for (auto& entry : bucket) {
          real_zig.push_back(std::move(entry));
        }
      }
      if (real_cpp.empty() && real_zig.empty()) {
        if (shifted > 0) {
          id_shift_echoes[table] += shifted;
        }
        continue;
      }
      if (is_unported) {
        continue;
      }

      // Behaviour 4: the ROW-LEVEL delta, never two table dumps.
      std::ranges::sort(real_cpp, {}, &row::full);
      std::ranges::sort(real_zig, {}, &row::full);
      std::string detail = std::format("+{} cpp-only / +{} zig-only", real_cpp.size(), real_zig.size());
      for (auto const& entry : real_cpp) {
        detail += std::format(" | cpp-only: {}", escape(entry.full));
      }
      for (auto const& entry : real_zig) {
        detail += std::format(" | zig-only: {}", escape(entry.full));
      }
      observed.push_back({tag, std::format("state:{}", table), detail});
    }
  }

  // ---- the unported set, pinned in both directions ---------------------
  {
    std::vector<std::string> const expected{expected_unported.begin(), expected_unported.end()};
    std::vector<std::string>       got = unported;
    std::ranges::sort(got);
    INFO("unported steps reported as `exit 64 + not implemented in this build`");
    CHECK(got == expected);
  }

  // ---- the divergences, matched against the staged list ----------------
  auto const&       staged = known_divergences();
  std::vector<bool> matched(staged.size(), false);

  std::vector<finding> unexpected;
  for (auto const& item : observed) {
    bool found = false;
    for (std::size_t i = 0; i < staged.size(); ++i) {
      if (matched[i]) {
        continue;
      }
      if (staged[i].tag == item.tag && staged[i].channel == item.channel && staged[i].detail == item.detail) {
        matched[i] = true;
        found      = true;
        break;
      }
    }
    if (!found) {
      unexpected.push_back(item);
    }
  }

  if (!unexpected.empty()) {
    std::string report = std::format("\n{} UNEXPECTED state/output divergence(s).\n"
                                     "Each one is either a regression or an intentional change that has not been "
                                     "re-baselined.\n"
                                     "If intentional, add these entries to the `known_divergences` list below verbatim:\n",
                                     unexpected.size());
    for (auto const& item : unexpected) {
      report += std::format("\n[{}] channel={}\n{}\n{}\n", item.tag, item.channel, item.detail, as_entry(item));
      // ALSO on stderr, unwrapped. Catch2 hard-wraps its own message at 80
      // columns, which breaks every entry across lines and makes the
      // "copy-pasteable" promise above false exactly when it is needed.
      // Catch2 does not capture stderr, so this arrives intact.
      std::println(stderr, "{}", as_entry(item));
    }
    FAIL(report);
  }

  for (std::size_t i = 0; i < staged.size(); ++i) {
    if (matched[i]) {
      continue;
    }
    FAIL(std::format("\nSTALE known-divergence entry: [{}] channel={}\n{}\n"
                     "This divergence no longer occurs. If its task fixed it, DELETE the entry from "
                     "the `known_divergences` list below — that deletion is the green half of that task's red-then-green.",
                     staged[i].tag, staged[i].channel, staged[i].detail));
  }

  // Not an assertion — the collapsed echo counts, so a reader of a failing
  // run can tell how much of the delta is downstream of one missing row.
  for (auto const& [table, count] : id_shift_echoes) {
    WARN(std::format("id-shift echo: {}: {} row(s) differ only by id (downstream of an earlier finding)", table, count));
  }
}
