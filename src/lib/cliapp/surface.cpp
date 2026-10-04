/// @file surface.cpp
/// @brief Implementation of `planar.cliapp.surface`'s parser-agreement
/// primitives: `add_bool_flag`, `hide_negations_in_help`, and
/// `hoist_subcommands`.

module planar.cliapp.surface;

import std;
import cli11;
import planar.cliapp.walk;

namespace planar::cliapp {

auto add_bool_flag(CLI::App& app, std::string_view canonical, std::string_view description) -> CLI::Option* {
  // CLI11's comma-separated name list with a `!` prefix marks the second
  // name as a NEGATION: supplying it stores "false" in the option's
  // results where the plain name stores "true". Building the string here
  // rather than at ~350 call sites is what makes the rule unconditional.
  std::string names{canonical};
  names += ",!--no-";
  names += canonical.substr(2);
  CLI::Option* opt = app.add_flag(names);
  // etcli's `parseBoolValue` accepts the two literals `true` and `false`
  // and NOTHING else, answering `InvalidValue` (exit 2) for anything
  // further — `planar workbench list --json=bogus` is a parse error on the
  // oracle. CLI11 has no such check and passed `bogus` through as a
  // present flag (exit 0). The results string is `"true"` for a plain
  // `--X`, `"false"` for the negation, and the raw text for an
  // unrecognised `--X=value`, so validating the result reproduces the
  // oracle's refusal without touching either good case.
  opt->check(CLI::Validator(
      [](std::string& value) -> std::string {
        if (value == "true" || value == "false") {
          return {};
        }
        return "invalid boolean value: " + value;
      },
      "", "bool-literal"));
  if (!description.empty()) {
    opt->description(std::string{description});
  }
  return opt;
}

namespace {

/// @brief CLI11's stock formatter, minus the negation names.
///
/// `make_option_name` is the ONE hook that decides the left column of an
/// option's help row, so overriding it alone leaves column widths,
/// grouping, wrapping and every other rendering decision exactly as CLI11
/// makes them. The reimplementation below is the `else` branch of
/// `CLI::Option::get_name(false, true, …)` — short names then long names,
/// comma-joined — with `fnames_` members dropped; that branch is precisely
/// the one CLI11 itself takes for a flag with NO negations, so a page
/// rendered through this formatter is byte-identical to the page the same
/// tree rendered before `add_bool_flag` existed.
struct negation_hiding_formatter : CLI::Formatter {
  /// @brief Render an option's name column without its negation names.
  /// @param opt The option.
  /// @param is_positional Whether it is a positional.
  /// @return The rendered name column.
  [[nodiscard]] auto make_option_name(const CLI::Option* opt, bool is_positional) const -> std::string override {
    if (is_positional || opt->get_fnames().empty()) {
      return CLI::Formatter::make_option_name(opt, is_positional);
    }
    std::vector<std::string> names;
    for (auto const& sname : opt->get_snames()) {
      names.push_back("-" + sname);
    }
    auto const& negated = opt->get_fnames();
    for (auto const& lname : opt->get_lnames()) {
      if (std::ranges::find(negated, lname) != negated.end()) {
        continue;
      }
      names.push_back("--" + lname);
    }
    // `CLI::detail::join` is not reachable from the module wrapper, and
    // the separator is CLI11's own `", "` — the one it uses for every
    // other multi-name option on the page.
    std::string out;
    for (auto const& name : names) {
      if (!out.empty()) {
        out += ", ";
      }
      out += name;
    }
    return out;
  }
};

} // namespace

auto hide_negations_in_help(CLI::App& root) -> void {
  auto const shared = std::make_shared<negation_hiding_formatter>();
  root.formatter(shared);
  for (auto const& node : all_nodes(root)) {
    // `all_nodes` hands back const views of a tree the caller owns
    // mutably; installing a formatter mutates nothing an invocation reads.
    const_cast<CLI::App*>(node.node)->formatter(shared); // NOLINT(cppcoreguidelines-pro-type-const-cast)
  }
}

auto hoist_subcommands(const CLI::App& root, std::span<std::string const> argv) -> std::vector<std::string> {
  // True when `name` is declared somewhere in the tree and every declaration
  // takes a value (expected_max != 0, never a bare flag). The token after such
  // a flag is its value and must not be read as a verb, which is how
  // `schema --command task` stays a lookup. Resolved against the whole tree
  // rather than the node reached so far, so the value-before-verb spelling
  // (`planar --command task schema`) is protected too; a name declared as a
  // bare flag on any node falls back to the plain rule, because CLI11 would
  // not consume a value for it there either.
  auto const takes_value_everywhere = [&root](std::string const& name) -> bool {
    bool seen = false;
    for (auto const& node : all_nodes(root)) {
      const CLI::Option* opt = node.node->get_option_no_throw(name);
      if (opt == nullptr) {
        continue;
      }
      if (opt->get_expected_max() == 0) {
        return false;
      }
      seen = true;
    }
    return seen;
  };

  std::vector<std::string> path;
  std::vector<std::string> tail;
  const CLI::App*          node        = &root;
  bool                     passthrough = false;

  for (std::size_t i = 1; i < argv.size(); ++i) {
    auto const& token = argv[i];
    if (passthrough) {
      tail.push_back(token);
      continue;
    }
    if (token == "--") {
      // Everything after the terminator is a value, never a verb. etcli
      // sets the same latch (`passthrough`) at the same token.
      passthrough = true;
      tail.push_back(token);
      continue;
    }
    if (!token.empty() && token.front() == '-') {
      // A long flag that takes a value, written as `--flag value`, keeps its
      // value beside it in the tail so the verb scan below never sees it:
      // `schema --command task` looks `task` up instead of hoisting it. The
      // `--flag=value` form is one flag-shaped token and needs no such care,
      // and a flag given as the last token has no value to protect.
      if (token.starts_with("--") && !token.contains('=') && i + 1 < argv.size() && takes_value_everywhere(token)) {
        tail.push_back(token);
        tail.push_back(argv[++i]);
        continue;
      }
      tail.push_back(token);
      continue;
    }
    // A bare word. It is a subcommand only if the node reached SO FAR has
    // a child named for it — a strictly local lookup, never a search of
    // the whole tree. That locality is what keeps a leaf's positional
    // whose value happens to spell a sibling verb (`planar task add list`,
    // `outer inner other` in the tests) from being retargeted into the
    // path: once `node` is a leaf its subcommand list is empty, so no
    // token can match. An extra `children(*node).empty()` guard was
    // written here first and a break-probe proved it DEAD for exactly that
    // reason; it is gone rather than left in as reassurance.
    if (const CLI::App* next = node->get_subcommand_no_throw(token); next != nullptr) {
      path.push_back(token);
      node = next;
      continue;
    }
    tail.push_back(token);
  }

  std::vector<std::string> out;
  out.reserve(argv.size());
  if (!argv.empty()) {
    out.push_back(argv.front());
  }
  out.insert(out.end(), path.begin(), path.end());
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

} // namespace planar::cliapp
