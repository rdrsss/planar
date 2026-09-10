/// @file manifest.cpp
/// @brief The frozen host-fn manifest, the command allowlist, and the pure
/// guards — everything in `planar.engine_execute` that touches no Lua state,
/// no process and no filesystem.
///
/// Kept in its own translation unit precisely BECAUSE it is pure: these are
/// the declarations the whole boundary argument rests on, and they are
/// readable, testable and reviewable without a `lua_State` anywhere in
/// sight. `host.cpp` next door is the only file in the module that includes
/// a Lua header or calls `fork`.

module planar.engine_execute;

import std;
import planar.json_text;

namespace planar::engine::execute {

namespace {

/// @brief The five host tables, in the order the registrar creates them.
constexpr std::array<std::string_view, 5> k_host_tables{"cli", "git", "fs", "flow", "ctx"};

/// @brief D7's allowlist — the entire host-call capability surface.
///
/// Alphabetical within each table group, which is also the order
/// `registered_host_surface()`'s sorted live enumeration produces, so the
/// two compare element-wise without either side re-sorting.
///
/// Twenty-five entries. The count is asserted separately from the contents
/// in `surface.t.cpp`, because a test that only compares contents to this
/// same table would pass trivially if someone edited both — the count is a
/// second, independently-stated fact about the same surface.
constexpr std::array<host_fn, 25> k_allowed{{
    // cli.* — allowlisted-binary shells. NOT a general exec: each function
    // hardcodes its binary, and `command_allowed` gates the verb on top.
    {"cli", "planar"},
    {"cli", "planar_agent"},
    {"cli", "planar_agent_json"},
    {"cli", "planar_json"},
    {"cli", "planar_watch"},
    {"cli", "planar_watch_json"},
    // git.* — confined group. The host injects `-C <worktree>` from the run
    // config; a workflow never names the directory and cannot escape it.
    {"git", "checkout"},
    {"git", "clean"},
    {"git", "diff_name_only"},
    {"git", "head_sha"},
    {"git", "reset_hard"},
    // fs.* — confined beneath the sandbox root.
    {"fs", "exists"},
    {"fs", "mkdir"},
    {"fs", "read"},
    {"fs", "write"},
    // flow.* — pure: log, phase marker, fail, result.
    {"flow", "fail"},
    {"flow", "log"},
    {"flow", "phase"},
    {"flow", "result"},
    // ctx.* — deterministic planner reads.
    {"ctx", "brief"},
    {"ctx", "context"},
    {"ctx", "plan_show"},
    {"ctx", "recommend_strategy"},
    {"ctx", "task_show"},
    {"ctx", "task_touches"},
}};

/// @brief Names that must never appear on the host surface.
///
/// Two groups, and both are here for a reason rather than for symmetry. The
/// first eight are the orchestration primitives the previous planar-execute
/// accreted — `agent`, `parallel`, `pipeline`, `workflow`, `dispatch_table`,
/// `compact`, `budget` — plus `exec`, the general escape they all reduce to.
/// The rest name the shapes a re-introduction would most plausibly take:
/// `spawn`, `child`, `claude`, `codex`, `headless`, `model`.
constexpr std::array<std::string_view, 14> k_denied{
    "agent", "parallel", "pipeline", "workflow", "dispatch_table", "compact",  "budget",
    "exec",  "spawn",    "child",    "claude",   "codex",          "headless", "model",
};

/// @brief The only binaries a `cli.*` function may shell.
constexpr std::array<std::string_view, 3> k_cli_bins{"planar", "planar-agent", "planar-watch"};

/// @brief COMPILE-TIME lock #2 of three (see execute.cppm's header): no
/// allowlisted name may collide with a denied one.
///
/// This fires before a single test runs. A registrar entry named `exec` or
/// `spawn` is not a failing assertion — it is a translation unit that does
/// not compile.
consteval auto allow_and_deny_are_disjoint() -> bool {
  for (auto const& allowed : k_allowed) {
    for (auto const& denied : k_denied) {
      if (allowed.name == denied) {
        return false;
      }
    }
  }
  return true;
}
static_assert(allow_and_deny_are_disjoint(), "engine/execute: ALLOWED_HOST_FNS names a DENIED host function. "
                                             "planar-execute exposes no spawn-shaped primitive (plan 633 D5).");

/// @brief And the manifest must stay inside the five declared tables — a
/// sixth table would be a whole new capability group arriving unannounced.
consteval auto every_entry_has_a_known_table() -> bool {
  for (auto const& allowed : k_allowed) {
    bool found = false;
    for (auto const& table : k_host_tables) {
      found = found || (allowed.table == table);
    }
    if (!found) {
      return false;
    }
  }
  return true;
}
static_assert(every_entry_has_a_known_table(), "engine/execute: a manifest entry names a table outside cli/git/fs/flow/ctx.");

} // namespace

auto host_tables() -> std::span<const std::string_view> {
  return k_host_tables;
}

auto allowed_host_fns() -> std::span<const host_fn> {
  return k_allowed;
}

auto denied_host_fns() -> std::span<const std::string_view> {
  return k_denied;
}

auto allowed_cli_bins() -> std::span<const std::string_view> {
  return k_cli_bins;
}

auto command_allowed(std::string_view bin, std::span<const std::string> argv) -> bool {
  if (argv.empty()) {
    return false;
  }
  if (bin == "planar-agent") {
    // The claim ritual plus the read verbs. Note what is here and what the
    // presence of `complete`/`fail`/`release`/`block` means: a workflow CAN
    // close a claim. That is a mutation, not a spawn — the ritual is exactly
    // the deterministic bookkeeping this engine exists to automate.
    constexpr std::array<std::string_view, 11> allowed{"pull",  "claim",   "heartbeat", "complete", "fail",  "release",
                                                       "block", "context", "run",       "action",   "schema"};
    return std::ranges::find(allowed, argv[0]) != allowed.end();
  }
  if (bin == "planar-watch") {
    constexpr std::array<std::string_view, 9> allowed{"feed", "ps",         "claims", "actions", "plans",
                                                      "log",  "syncevents", "schema", "version"};
    return std::ranges::find(allowed, argv[0]) != allowed.end();
  }
  if (bin != "planar") {
    return false;
  }
  constexpr std::array<std::string_view, 4> single{"schema", "health", "resume", "report"};
  if (std::ranges::find(single, argv[0]) != single.end()) {
    return true;
  }
  if (argv.size() < 2) {
    return false;
  }
  // `planar` is matched as a PAIR. A one-token match would let
  // `planar task done` through on the strength of `planar task show` being
  // allowed, which is precisely the terminal-verb surface a workflow must
  // reach through `planar-agent`'s atomic verbs instead.
  constexpr std::array<std::pair<std::string_view, std::string_view>, 26> pairs{{
      {"scope", "show"},
      {"plan", "show"},
      {"plan", "list"},
      {"plan", "descendants"},
      {"plan", "next"},
      {"plan", "closeout"},
      {"plan", "recommend-strategy"},
      {"task", "show"},
      {"task", "list"},
      {"task", "packet"},
      {"task", "add"},
      {"task", "touches"},
      {"question", "list"},
      {"question", "add"},
      {"links", "list"},
      {"run", "start"},
      {"run", "event"},
      {"run", "finish"},
      {"bench", "start"},
      {"bench", "show"},
      {"bench", "harvest"},
      {"bench", "finish"},
      {"ext", "propagate-one"},
      {"capture", "snapshot"},
      {"handoff", "create"},
      {"handoff", "validate"},
  }};
  return std::ranges::any_of(pairs, [&](auto const& pair) { return argv[0] == pair.first && argv[1] == pair.second; });
}

auto is_hex_object_id(std::string_view value) -> bool {
  if (value.size() < 7 || value.size() > 64) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char c) { return std::isxdigit(c) != 0; });
}

auto safe_git_ref(std::string_view value) -> bool {
  if (value.empty() || value.front() == '-' || value.find("..") != std::string_view::npos) {
    return false;
  }
  return std::ranges::all_of(value, [](char c) {
    auto const u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '/' || c == '_' || c == '-' || c == '.' || c == '~' || c == '^';
  });
}

auto validate_confined_rel(std::string_view rel) -> bool {
  if (rel.empty() || rel.front() == '/' || rel.find('\\') != std::string_view::npos) {
    return false;
  }
  for (auto const component : std::views::split(rel, '/')) {
    std::string_view const part{component.begin(), component.end()};
    if (part.empty() || part == "." || part == "..") {
      return false;
    }
  }
  return true;
}

auto format_double(double value) -> std::string {
  // Delegates to `planar.json_text` (task 6261). This file used to carry
  // its own transcription of Zig's `{d}` -- shortest-round-trip digits
  // rendered WITHOUT an exponent -- and it was one of the two CORRECT
  // transcriptions of four, so nothing about its behaviour changes here.
  // What changes is that there is now one definition instead of four; see
  // json_text.cppm for the inventory and for the two traps this
  // implementation records (`chars_format::fixed` spells out the full
  // exact binary expansion of `1e300`, and `std::from_chars` REJECTS the
  // leading `+` in `e+300` rather than skipping it, which silently yielded
  // exponent 0 and rendered `1e300` as `1`).
  //
  // `format_double_fixed`, NOT `append_json_double`: this surface is the
  // Lua workflow manifest, not a JSON document. Its pins require Zig's bare
  // `inf` / `-inf` / `nan` spellings, and the reason the deleted body gave
  // for keeping them stands -- faking `null` here would make a workflow
  // that divided by zero look like one that returned nothing. The bare
  // spellings are what `format_double_fixed` returns for non-finite input,
  // and that arm exists precisely so this caller can share the finite path
  // without inheriting a JSON emitter's null.
  return json_text::format_double_fixed(value);
}

} // namespace planar::engine::execute
