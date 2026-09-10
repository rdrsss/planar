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
  // Non-finite first: the oracle emits Zig's `{d}` spellings, which are bare
  // `inf` / `-inf` / `nan`. None of the three is legal JSON and the oracle
  // emits them anyway; reproducing that is parity, not endorsement. Faking
  // `null` here would make a workflow that divided by zero look like one
  // that returned nothing.
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value < 0 ? "-inf" : "inf";
  }

  // Zig's `{d}` is SHORTEST-ROUND-TRIP digits rendered WITHOUT an exponent.
  // Neither half of that is what a single `to_chars` call gives:
  //
  //   std::format("{}", 1e300)                    -> "1e+300"
  //   to_chars(..., chars_format::fixed)          -> the exact binary value
  //                                                  of the double, 1 followed
  //                                                  by "0000000000000000525…"
  //                                                  — 300 digits of noise
  //   oracle                                      -> "1" + 300 zeros
  //
  // `fixed` is shortest *for fixed notation*, which is not the same thing as
  // the shortest digit string: it must reproduce the value from the decimal
  // point outward, so it spells out the full exact expansion. Both spellings
  // round-trip; only one is the oracle's. So: take the shortest digits from
  // the default (general) format, then place the decimal point by hand.
  //
  // This mattered in practice — a first implementation used `fixed` alone
  // and the differential run against the oracle caught it on `1e300`.
  std::array<char, 64> buffer{};
  auto const           shortest = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  std::string_view     text{buffer.data(), shortest.ptr};

  auto const exponent_at = text.find('e');
  if (exponent_at == std::string_view::npos) {
    return std::string{text}; // Already plain: "0.1", "-0", "100".
  }

  std::string_view mantissa      = text.substr(0, exponent_at);
  std::string_view exponent_text = text.substr(exponent_at + 1);
  // `to_chars` writes `e+300` for positive exponents and `std::from_chars`
  // rejects a leading `+` outright — it does not skip it, it fails and leaves
  // the output untouched. Ignoring that failure silently produced exponent 0,
  // and `1e300` rendered as `1`. Caught by the differential run, not by
  // reading the standard.
  if (!exponent_text.empty() && exponent_text.front() == '+') {
    exponent_text.remove_prefix(1);
  }
  int exponent = 0;
  if (std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent).ec != std::errc{}) {
    return std::string{text};
  }

  std::string sign;
  if (!mantissa.empty() && (mantissa.front() == '-' || mantissa.front() == '+')) {
    if (mantissa.front() == '-') {
      sign = "-";
    }
    mantissa.remove_prefix(1);
  }

  // Split the mantissa and concatenate its digits; `point` is then the index
  // within that digit string where the decimal point belongs.
  auto const  dot = mantissa.find('.');
  std::string digits{mantissa.substr(0, dot)};
  if (dot != std::string_view::npos) {
    digits += mantissa.substr(dot + 1);
  }
  auto const integral_digits = static_cast<int>(dot == std::string_view::npos ? mantissa.size() : dot);
  int const  point           = integral_digits + exponent;

  if (point <= 0) {
    return sign + "0." + std::string(static_cast<std::size_t>(-point), '0') + digits;
  }
  if (static_cast<std::size_t>(point) >= digits.size()) {
    return sign + digits + std::string(static_cast<std::size_t>(point) - digits.size(), '0');
  }
  return sign + digits.substr(0, static_cast<std::size_t>(point)) + "." + digits.substr(static_cast<std::size_t>(point));
}

} // namespace planar::engine::execute
