// queue_rule.t.cpp: the agent rule text and `planar-agent queue rule` (plan
// 1080, tasks hq-rule-text and hq-queue-rule-verb; tech spec 647 § CLI surface
// and § The rule text has one authored source; test spec 649 scenarios citing
// either task).
//
// The rule text is one authored file, `src/lib/queuerule/queue-rule.md`,
// embedded into `planar-agent` at configure time. These cases read that file
// from the repository and hold the built binary to it: what `queue rule`
// prints is the file byte for byte, it is printed without opening
// `planar.db` (which holds the queue's tables, so a path that cannot be used
// changes neither its output nor the disk), the text carries every element the task names, and every command
// it shows exists in the binary's own command tree with the flags it uses.
//
// Nothing here compares the text to a copy of itself. The content cases name
// what an agent needs to find in the rule and look for those words; they are
// not tied to one phrasing.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.main;

#include "parity_harness.hpp"

namespace {

namespace parity = planar::cmd::parity;

using parity::capture;
using parity::pinned_var;

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief The authored rule file, read from the repository.
auto rule_source() -> std::string {
  std::ifstream in(PLANAR_QUEUE_RULE_SOURCE, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

auto contains(std::string_view text, std::string_view needle) -> bool {
  return text.find(needle) != std::string_view::npos;
}

/// @brief The text under one `###` heading that contains `key`, up to the next
/// heading of the same or higher level; empty when no heading matches.
auto section(std::string_view text, std::string_view key) -> std::string {
  std::size_t pos = 0;
  while (pos < text.size()) {
    auto const nl   = text.find('\n', pos);
    auto const line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    if (line.starts_with("### ") && contains(line, key)) {
      auto const start = pos;
      auto       next  = nl == std::string_view::npos ? text.size() : nl + 1;
      while (next < text.size()) {
        auto const e  = text.find('\n', next);
        auto const ln = text.substr(next, e == std::string_view::npos ? std::string_view::npos : e - next);
        if (ln.starts_with("## ") || ln.starts_with("### ")) {
          break;
        }
        next = e == std::string_view::npos ? text.size() : e + 1;
      }
      return std::string{text.substr(start, next - start)};
    }
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  return {};
}

/// @brief Lower-cased text with every run of white space collapsed to one
/// space, so a check does not depend on where the text wraps.
auto lower(std::string_view text) -> std::string {
  std::string out;
  bool        space = false;
  for (auto const c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(' ');
      space = false;
    }
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

/// @brief Every command the text shows: the lines of fenced blocks and the
/// inline code spans, each kept when it starts with `planar-agent`.
auto shown_commands(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> found;
  bool                     fenced = false;
  std::size_t              pos    = 0;
  while (pos <= text.size()) {
    auto const nl   = text.find('\n', pos);
    auto const line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    if (line.starts_with("```")) {
      fenced = !fenced;
    } else if (fenced) {
      if (line.starts_with("planar-agent ")) {
        found.emplace_back(line);
      }
    } else {
      std::size_t i = 0;
      while (i < line.size()) {
        if (line[i] != '`') {
          ++i;
          continue;
        }
        auto const j = line.find('`', i + 1);
        if (j == std::string_view::npos) {
          break;
        }
        auto const span = line.substr(i + 1, j - i - 1);
        if (span.starts_with("planar-agent ")) {
          found.emplace_back(span);
        }
        i = j + 1;
      }
    }
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  return found;
}

auto words(std::string_view line) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::istringstream       in{std::string{line}};
  for (std::string w; in >> w;) {
    out.push_back(w);
  }
  return out;
}

/// @brief The flag and positional names an `App` declares, plus `--help`.
auto declared_names(const CLI::App& app) -> std::set<std::string, std::less<>> {
  std::set<std::string, std::less<>> names;
  for (auto const* option : app.get_options()) {
    names.insert(option->get_name(false, true));
  }
  names.insert("--help");
  return names;
}

auto run_rule(const parity::arena& arena, std::string_view tag, const std::vector<pinned_var>& env,
              std::vector<std::string> args = {"queue", "rule"}) -> capture {
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag, env);
}

} // namespace

TEST_CASE("queue rule prints the embedded rule file byte for byte", "[cmd][agent][queue][rule]") {
  auto const arena  = parity::make_arena("queue-rule-bytes");
  auto const source = rule_source();
  REQUIRE_FALSE(source.empty());

  auto const got = run_rule(arena, "bytes", parity::pinned_env(arena.cpp_root));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == source);
}

TEST_CASE("queue rule has no --json and takes no argument", "[cmd][agent][queue][rule]") {
  auto const arena = parity::make_arena("queue-rule-args");
  auto const env   = parity::pinned_env(arena.cpp_root);

  auto const json = run_rule(arena, "json", env, {"queue", "rule", "--json"});
  CHECK(json.code != 0);
  CHECK(json.out.empty());

  auto const extra = run_rule(arena, "extra", env, {"queue", "rule", "now"});
  CHECK(extra.code != 0);
  CHECK(extra.out.empty());
}

TEST_CASE("queue rule opens no database: an unusable database path changes neither its output nor the disk",
          "[cmd][agent][queue][rule]") {
  auto const arena  = parity::make_arena("queue-rule-nodb");
  auto const source = rule_source();
  auto const root   = arena.cpp_root;

  SECTION("the database path names a place that does not exist") {
    auto env = parity::pinned_env(root);
    for (auto& var : env) {
      if (var.name == "PLANAR_DB") {
        var.value = (root / "absent" / "deeper" / "planar.db").string();
      }
    }
    auto const got = run_rule(arena, "absent", env);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 0);
    CHECK(got.out == source);
    // Opening the store would have refused it, and creating it would have made
    // its parent directory.
    CHECK_FALSE(std::filesystem::exists(root / "absent"));
  }

  SECTION("the database path sits in a directory that cannot be written") {
    auto const locked = root / "locked";
    std::filesystem::create_directories(locked);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    struct restore {
      std::filesystem::path path;
      ~restore() {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
      }
    } const guard{locked};

    auto env = parity::pinned_env(root);
    for (auto& var : env) {
      if (var.name == "PLANAR_DB") {
        var.value = (locked / "planar.db").string();
      }
    }
    auto const got = run_rule(arena, "locked", env);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 0);
    CHECK(got.out == source);
    CHECK_FALSE(std::filesystem::exists(locked / "planar.db"));
  }

  SECTION("no database path can be resolved at all") {
    auto env = parity::pinned_env(root);
    for (auto& var : env) {
      if (var.name == "PLANAR_DB" || var.name == "HOME") {
        var.unset = true;
      }
    }
    auto const got = run_rule(arena, "unresolved", env);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 0);
    CHECK(got.out == source);
    CHECK_FALSE(std::filesystem::exists(root / "planar.db"));
  }
}

TEST_CASE("the rule text carries every element an agent needs", "[cmd][agent][queue][rule]") {
  auto const source = rule_source();
  auto const text   = lower(source);

  SECTION("the definition of a build or test command, with examples") {
    CHECK(contains(text, "compiles or links"));
    CHECK(contains(text, "test suite"));
    CHECK(contains(text, "more than one core"));
    for (auto const* example : {"`make`", "`make test`", "`cmake --build`", "`ninja`", "`ctest`", "`cargo test`", "`go test`",
                                "`npm test`", "`pytest`"}) {
      INFO("example: " << example);
      CHECK(contains(text, example));
    }
    // What is not queued, and what to do when unsure.
    CHECK(contains(text, "not queued"));
    CHECK(contains(text, "unsure"));
  }

  SECTION("the detached submit and finite observation procedure") {
    CHECK(contains(source, "planar-agent queue run --detach"));
    CHECK(contains(source, "planar-agent queue wait <seq> --timeout 3h --json"));
    CHECK(contains(text, "backlog"));
    CHECK(contains(text, "runtime"));
    CHECK(contains(text, "same sequence number"));
    CHECK(contains(text, "unbounded retry loop"));
    CHECK_FALSE(contains(text, "every 30 seconds"));
    CHECK(contains(text, "sequence number"));
    CHECK(contains(text, "output file"));
  }

  SECTION("what a non-zero exit from the submit means") {
    // No ticket exists then, and each code has its own route.
    CHECK(contains(text, "a non-zero exit from the submit means no ticket was issued"));
    CHECK(contains(text, "126 or 127"));
    CHECK(contains(text, "1 or 2"));
    CHECK(contains(text, "with 125"));
  }

  SECTION("the instruction to pass vendor and role") {
    CHECK(contains(source, "--vendor <vendor>"));
    CHECK(contains(source, "--role <role>"));
    CHECK(contains(text, "always pass"));
  }

  SECTION("what to do on each outcome") {
    for (auto const* outcome : {"exited", "signaled", "timeout", "cancelled", "wait_timeout", "not_started", "abandoned"}) {
      INFO("outcome: " << outcome);
      // A table row names it; the bare word also appears in prose.
      CHECK(contains(source, std::format("| `{}`", outcome)));
    }
    CHECK(contains(text, "| `abandoned` |"));
    CHECK(contains(text, "never assume completion or resubmit automatically"));
  }

  SECTION("the submitter renews a caller-supervised claim while the observer does not") {
    CHECK(contains(source, "`--claim <token>`"));
    CHECK(contains(text, "queue wait` is read-only and never renews a claim"));
    CHECK(contains(text, "renews a caller-supervised claim"));
    std::size_t mentions = 0;
    for (std::size_t at = source.find("`--claim <token>`"); at != std::string::npos;
         at             = source.find("`--claim <token>`", at + 1)) {
      ++mentions;
    }
    CHECK(mentions >= 1);
    // The flag it names is a real flag of `queue run`.
    auto const  root = planar::cmd::agent::root_app();
    auto const* run  = root->get_subcommand_no_throw("queue")->get_subcommand_no_throw("run");
    REQUIRE(run != nullptr);
    CHECK(declared_names(*run).contains("--claim"));
  }

  SECTION("the exit codes of the foreground form") {
    for (auto const* code : {"124", "125", "126", "127", "128"}) {
      INFO("code: " << code);
      CHECK(contains(source, code));
    }
  }
}

TEST_CASE("the rule text cannot be read as permission to bypass the queue on exit 125", "[cmd][agent][queue][rule]") {
  auto const source = rule_source();

  auto const refusal = lower(section(source, "exit 125"));
  REQUIRE_FALSE(refusal.empty());
  CHECK(contains(refusal, "stop"));
  CHECK(contains(refusal, "report"));
  CHECK(contains(refusal, "must not be run directly"));

  auto const fallback = lower(section(source, "has no queue"));
  REQUIRE_FALSE(fallback.empty());
  CHECK(contains(fallback, "verifying the installed planar lacks the `queue` verb"));
  CHECK(contains(fallback, "nonzero `queue rule` alone does not establish absence"));
  CHECK(contains(fallback, "needs upgrading"));
  CHECK(contains(fallback, "finite compatibility path"));

  // The refusal names no `queue rule` check, so it cannot be read as the fallback.
  CHECK_FALSE(contains(refusal, "queue rule"));
  CHECK_FALSE(contains(refusal, "--help"));
  // It covers a command that itself exits 125, and tells how to tell the two apart.
  CHECK(contains(refusal, "--notices"));
  // A wait limit or a cancellation is an outcome of a ticket, not this case.
  CHECK(contains(refusal, "`wait_timeout`"));

  // The check is never `--help`: an older Planar answers an unknown command's
  // `--help` with its general help and exits 0.
  for (auto const& command : shown_commands(source)) {
    INFO("command: " << command);
    CHECK(command != "planar-agent queue --help");
  }

  // The two are also set side by side in one labelled table.
  auto const confused = lower(section(source, "Do not confuse"));
  REQUIRE_FALSE(confused.empty());
  CHECK(contains(confused, "refuses"));
  CHECK(contains(confused, "catalog"));
}

TEST_CASE("the no-queue check discriminates: it passes on this binary and fails where the verb is unknown",
          "[cmd][agent][queue][rule]") {
  // The rule's check is `planar-agent queue rule >/dev/null`. It is sound only
  // if an unknown domain followed by `rule` fails, and if `--help` could not
  // serve instead. An older Planar is modelled by an unknown domain: to it,
  // `queue` is exactly as unknown as `nosuch-domain`.
  auto const arena = parity::make_arena("queue-rule-check");
  auto const env   = parity::pinned_env(arena.cpp_root);

  auto const present = run_rule(arena, "present", env, {"queue", "rule"});
  CHECK(present.code == 0);

  auto const absent = run_rule(arena, "absent", env, {"nosuch-domain", "rule"});
  INFO("stderr:\n" << absent.err);
  CHECK(absent.code != 0);
  CHECK(absent.out.empty());

  // The check the TEXT gives is the one held to that: take the command from
  // "Before the first submission", run it as written, then run it with the
  // `queue` domain made unknown, which is what an older Planar is to it.
  auto const before = section(rule_source(), "Before the first submission");
  REQUIRE_FALSE(before.empty());
  auto const commands = shown_commands(before);
  REQUIRE(commands.size() == 1);
  std::vector<std::string> as_written;
  for (auto const& word : words(commands.front())) {
    if (word == "planar-agent") {
      continue;
    }
    if (word.starts_with(">")) {
      break; // the redirection to /dev/null, not an argument
    }
    as_written.push_back(word);
  }
  REQUIRE(as_written.size() >= 2);
  REQUIRE(as_written.front() == "queue");
  auto const checked = run_rule(arena, "as-written", env, as_written);
  CHECK(checked.code == 0);

  auto old_planar    = as_written;
  old_planar.front() = "nosuch-domain";
  auto const fails   = run_rule(arena, "as-written-old", env, old_planar);
  INFO("the check as written, on a Planar that does not know `queue`: exit " << fails.code);
  CHECK(fails.code != 0);

  // The reason `--help` is not the check: an unknown command's `--help` is
  // answered with the general help and exit 0, so it would pass with no queue.
  auto const help = run_rule(arena, "help", env, {"nosuch-domain", "--help"});
  CHECK(help.code == 0);
  CHECK_FALSE(help.out.empty());
}

TEST_CASE("every planar-agent command the rule shows exists, with the flags it uses", "[cmd][agent][queue][rule]") {
  auto const source   = rule_source();
  auto const commands = shown_commands(source);
  // A parser that found nothing would pass every check below.
  REQUIRE(commands.size() >= 5);

  auto const  root  = planar::cmd::agent::root_app();
  auto const* queue = root->get_subcommand_no_throw("queue");
  REQUIRE(queue != nullptr);

  std::size_t verbs_checked = 0;
  for (auto const& command : commands) {
    INFO("command: " << command);
    auto const tokens = words(command);
    REQUIRE(tokens.size() >= 2);
    REQUIRE(tokens[0] == "planar-agent");
    REQUIRE(tokens[1] == "queue");

    const CLI::App* target = queue;
    std::size_t     next   = 2;
    if (tokens.size() > 2 && !tokens[2].starts_with("-")) {
      target = queue->get_subcommand_no_throw(tokens[2]);
      INFO("verb: " << tokens[2]);
      REQUIRE(target != nullptr);
      next = 3;
      ++verbs_checked;
    }
    auto const names = declared_names(*target);
    for (std::size_t i = next; i < tokens.size(); ++i) {
      if (tokens[i] == "--") {
        break; // what follows belongs to the queued command
      }
      if (tokens[i].starts_with("--")) {
        auto const name = tokens[i].substr(0, tokens[i].find('='));
        INFO("flag: " << name);
        CHECK(names.contains(name));
      }
    }
  }
  CHECK(verbs_checked >= 2);
}
