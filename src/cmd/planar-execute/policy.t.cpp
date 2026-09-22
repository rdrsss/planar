// @file policy.t.cpp
// @brief `workflows/command-policy.json` covers every command a shipped
// workflow runs, and nothing more (plan 1033 M0, task 6707; tech-spec
// D2/D14).
//
// Under Centurion every `planar` / `planar-agent` / `planar-watch` / `git`
// invocation a workflow makes is a `command.exec` call that must match an
// entry of this closed policy; an unlisted verb path cannot run. The M3
// prelude maps the embedded host surface onto those calls one for one, so
// the call sites that matter today are the shipped workflows' own host
// calls: `cli.*({...})`, the `ctx.*` reads (each shells one `planar` verb),
// and `git.*`. This file extracts them from the Lua source and checks both
// directions:
//
//   - every call site resolves to an entry (longest path prefix wins);
//   - every entry is used, and its `used_by` names exactly the shipped
//     workflows that use it. An entry used only by a PLANNED workflow
//     (`planned_workflows`, e.g. the M4 claim-supervision workflow) is
//     exempt while that file does not exist, and must be used once it does.
//
// Every Planar entry a shipped workflow uses must also pass the embedded
// engine's own allowlist (`command_allowed`), so the two cannot drift apart
// before M5 retires the embedded runner. `make cli-usage-check` separately
// lints every entry's verb path against the live schema catalogs.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>

import std;
import planar.engine_execute;

namespace policy_wire {

/// @brief One policy entry, as `workflows/command-policy.json` declares it.
struct entry {
  std::string              binary;  ///< `planar`, `planar-agent`, `planar-watch` or `git`.
  std::vector<std::string> path;    ///< The verb path, e.g. `["plan","show"]`.
  std::string              effect;  ///< `idempotent` or `reconcilable`.
  std::string              cwd;     ///< `any` or `workspace`.
  std::vector<std::string> used_by; ///< Workflow file names.
};

/// @brief The policy document.
struct document {
  int                      version = 0;       ///< Format version.
  std::string              description;       ///< Prose.
  std::vector<std::string> planned_workflows; ///< Workflows not shipped yet.
  std::vector<entry>       entries;           ///< The entries.
};

} // namespace policy_wire

namespace {

/// @brief One command a workflow runs.
struct call_site {
  std::string              file;   ///< Workflow file name.
  std::size_t              line{}; ///< 1-based line of the call.
  std::string              binary; ///< The binary it runs.
  std::vector<std::string> argv;   ///< Its leading literal arguments.
};

/// @brief Read a whole file.
auto read_file(std::filesystem::path const& path) -> std::string {
  std::ifstream      in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

/// @brief `source` with every Lua comment blanked (to spaces, so offsets and
/// line numbers are preserved). String-aware: `"--json"` is data.
auto strip_comments(std::string source) -> std::string {
  std::size_t i = 0;
  while (i < source.size()) {
    char const c = source[i];
    if (c == '"' || c == '\'') {
      for (++i; i < source.size() && source[i] != c; ++i) {
        if (source[i] == '\\') {
          ++i;
        }
      }
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < source.size() && source[i + 1] == '-') {
      auto const end  = source.compare(i, 4, "--[[") == 0 ? source.find("]]", i) : source.find('\n', i);
      auto const stop = end == std::string::npos ? source.size() : (source[end] == ']' ? end + 2 : end);
      for (auto j = i; j < stop; ++j) {
        if (source[j] != '\n') {
          source[j] = ' ';
        }
      }
      i = stop;
      continue;
    }
    ++i;
  }
  return source;
}

/// @brief The leading string literals of the table constructor at `open`
/// (which must be `{`), stopping at the first non-literal element.
auto leading_literals(std::string_view text, std::size_t open) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::size_t              i = open + 1;
  while (i < text.size()) {
    while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) != 0 || text[i] == ',')) {
      ++i;
    }
    if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) {
      break;
    }
    char const  quote = text[i];
    std::string value;
    for (++i; i < text.size() && text[i] != quote; ++i) {
      value += text[i];
    }
    ++i;
    out.push_back(std::move(value));
  }
  return out;
}

/// @brief Every command `source` (comments stripped) runs.
auto extract_calls(std::string const& file, std::string const& raw) -> std::vector<call_site> {
  auto const text    = strip_comments(raw);
  auto const line_of = [&](std::size_t at) { return static_cast<std::size_t>(std::ranges::count(text.substr(0, at), '\n')) + 1; };
  std::vector<call_site> out;

  // cli.* — the argv is a literal table, or a local assigned one.
  static std::regex const k_cli{R"(cli\.(planar_agent|planar_watch|planar)(?:_json)?\s*\(\s*(\{|[A-Za-z_][A-Za-z0-9_]*))"};
  for (auto it = std::sregex_iterator(text.begin(), text.end(), k_cli); it != std::sregex_iterator{}; ++it) {
    auto const& m      = *it;
    std::string binary = m[1].str();
    std::ranges::replace(binary, '_', '-');
    auto open = static_cast<std::size_t>(m.position(2));
    if (m[2].str() != "{") {
      auto const assign = text.find(std::format("local {} = {{", m[2].str()));
      REQUIRE(assign != std::string::npos);
      open = text.find('{', assign);
    }
    out.push_back({file, line_of(static_cast<std::size_t>(m.position(0))), binary, leading_literals(text, open)});
  }

  // ctx.* reads and git.* — each shells one fixed command (see host.cpp).
  static std::map<std::string, std::pair<std::string, std::vector<std::string>>> const k_fixed{
      {"ctx.plan_show", {"planar", {"plan", "show"}}},
      {"ctx.task_show", {"planar", {"task", "show"}}},
      {"ctx.task_touches", {"planar", {"task", "touches", "list"}}},
      {"ctx.recommend_strategy", {"planar", {"plan", "recommend-strategy"}}},
      {"git.reset_hard", {"git", {"reset", "--hard"}}},
      {"git.head_sha", {"git", {"rev-parse"}}},
      {"git.diff_name_only", {"git", {"diff"}}},
      {"git.checkout", {"git", {"checkout"}}},
      {"git.clean", {"git", {"clean"}}},
  };
  static std::regex const k_host{R"(\b(ctx|git)\.([a-z_]+)\s*\()"};
  for (auto it = std::sregex_iterator(text.begin(), text.end(), k_host); it != std::sregex_iterator{}; ++it) {
    auto const name = std::format("{}.{}", (*it)[1].str(), (*it)[2].str());
    if (name == "ctx.brief" || name == "ctx.context") {
      FAIL(std::format("{} uses {}, which no policy mapping covers yet; add one before shipping it", file, name));
    }
    auto const fixed = k_fixed.find(name);
    REQUIRE(fixed != k_fixed.end());
    out.push_back({file, line_of(static_cast<std::size_t>(it->position(0))), fixed->second.first, fixed->second.second});
  }
  return out;
}

/// @brief The policy entry `call` resolves to: same binary, longest path
/// that prefixes its argv.
auto resolve(policy_wire::document const& policy, call_site const& call) -> policy_wire::entry const* {
  policy_wire::entry const* best = nullptr;
  for (auto const& e : policy.entries) {
    if (e.binary != call.binary || e.path.size() > call.argv.size() ||
        !std::ranges::equal(e.path, std::span{call.argv}.first(e.path.size()))) {
      continue;
    }
    if (best == nullptr || e.path.size() > best->path.size()) {
      best = &e;
    }
  }
  return best;
}

auto load_policy() -> policy_wire::document {
  policy_wire::document doc;
  auto const            text = read_file(std::filesystem::path{PLANAR_WORKFLOWS_DIR} / "command-policy.json");
  REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = true}>(doc, text));
  return doc;
}

auto join(std::vector<std::string> const& parts) -> std::string {
  std::string out;
  for (auto const& p : parts) {
    out += out.empty() ? p : " " + p;
  }
  return out;
}

} // namespace

TEST_CASE("command policy: entries are well-formed and unique", "[cmd][execute][policy][6707]") {
  auto const policy = load_policy();
  CHECK(policy.version == 1);
  std::set<std::string> seen;
  for (auto const& e : policy.entries) {
    auto const id = std::format("{} {}", e.binary, join(e.path));
    INFO(id);
    CHECK((e.binary == "planar" || e.binary == "planar-agent" || e.binary == "planar-watch" || e.binary == "git"));
    CHECK_FALSE(e.path.empty());
    CHECK((e.effect == "idempotent" || e.effect == "reconcilable"));
    CHECK((e.cwd == "any" || e.cwd == "workspace"));
    CHECK_FALSE(e.used_by.empty());
    CHECK(seen.insert(id).second);
  }
}

TEST_CASE("command policy: every shipped workflow command resolves, and every entry is used by exactly its used_by",
          "[cmd][execute][policy][6707]") {
  auto const                  policy = load_policy();
  std::filesystem::path const dir{PLANAR_WORKFLOWS_DIR};

  std::vector<call_site> calls;
  std::set<std::string>  shipped;
  for (auto const& f : std::filesystem::directory_iterator{dir}) {
    if (f.path().extension() != ".lua") {
      continue;
    }
    auto const name = f.path().filename().string();
    shipped.insert(name);
    std::ranges::move(extract_calls(name, read_file(f.path())), std::back_inserter(calls));
  }
  // The extractor's own sanity: today's four workflows make this many
  // distinct host calls. A regex that silently stopped matching would
  // otherwise pass on an empty list.
  CHECK(calls.size() >= 20);

  std::map<policy_wire::entry const*, std::set<std::string>> users;
  for (auto const& call : calls) {
    INFO(std::format("{}:{}: {} {}", call.file, call.line, call.binary, join(call.argv)));
    auto const* e = resolve(policy, call);
    REQUIRE(e != nullptr); // a command no entry covers could not run under Centurion
    users[e].insert(call.file);
  }

  std::set<std::string> const planned{policy.planned_workflows.begin(), policy.planned_workflows.end()};
  for (auto const& e : policy.entries) {
    INFO(std::format("{} {}", e.binary, join(e.path)));
    std::set<std::string> declared_shipped;
    bool                  only_planned_absent = true;
    for (auto const& w : e.used_by) {
      if (shipped.contains(w)) {
        declared_shipped.insert(w);
        only_planned_absent = false;
      } else {
        // Naming a workflow that neither ships nor is planned is a typo.
        CHECK(planned.contains(w));
      }
    }
    // used_by names exactly the shipped workflows that use the entry.
    CHECK(declared_shipped == users[&e]);
    if (only_planned_absent) {
      continue; // e.g. claim_supervise.lua until M4 ships it
    }
    CHECK_FALSE(users[&e].empty());
  }
}

TEST_CASE("command policy: every Planar entry a shipped workflow uses passes the embedded allowlist too",
          "[cmd][execute][policy][6707]") {
  // Until M5 retires the embedded runner, the two allowlists must agree on
  // everything the shipped workflows run.
  auto const policy = load_policy();
  for (auto const& e : policy.entries) {
    bool const shipped_use = std::ranges::any_of(e.used_by, [&](auto const& w) {
      return std::ranges::find(policy.planned_workflows, w) == policy.planned_workflows.end();
    });
    if (e.binary == "git" || !shipped_use) {
      continue;
    }
    INFO(std::format("{} {}", e.binary, join(e.path)));
    CHECK(planar::engine::execute::command_allowed(e.binary, e.path));
  }
}

TEST_CASE("command policy: a verb path outside the policy does not resolve", "[cmd][execute][policy][6707]") {
  // The unit half of the test-spec scenario "a verb path outside the policy
  // cannot run" (the admission half is centuriond's, M3).
  auto const policy = load_policy();
  auto const calls =
      extract_calls("rogue.lua", "function p()\n  -- cli.planar({\"plan\", \"show\"}) in a comment is not a call\n"
                                 "  cli.planar({\"init\"})\n  cli.planar_json({\"task\", \"done\", \"1\"})\nend\n");
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].line == 3);
  CHECK(resolve(policy, calls[0]) == nullptr);
  CHECK(resolve(policy, calls[1]) == nullptr);
  // And the same shapes that ARE listed do resolve, by longest prefix.
  auto const listed = extract_calls("ok.lua", "function p() cli.planar({\"plan\", \"show\", \"7\", \"--json\"}) end\n");
  REQUIRE(listed.size() == 1);
  auto const* e = resolve(policy, listed[0]);
  REQUIRE(e != nullptr);
  CHECK(e->path == std::vector<std::string>{"plan", "show"});
}
