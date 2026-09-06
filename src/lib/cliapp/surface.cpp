/// @file surface.cpp
/// @brief Implementation of `planar.cliapp.surface::apply_surface`.

module planar.cliapp.surface;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.walk;

namespace planar::cliapp {

namespace {

/// @brief Declare one flag on `app` per its spec.
///
/// `kind` selects the CLI11 shape, and the mapping is what makes the
/// emitted catalog agree with the oracle's: `bool` must be a `add_flag`
/// (expected-max 0, which is how `planar.cliapp.schema::kind_of` reads a
/// flag back as `bool`), and `int` must carry `zig_int_validator` so the
/// catalog reports `int` AND so a malformed value fails at parse time with
/// Zig's `std.fmt.parseInt` semantics rather than degrading to "absent".
/// @param app The node to declare it on.
/// @param spec The flag's description.
auto declare_flag(CLI::App& app, flag_spec const& spec) -> void {
  std::string const name{spec.name};
  if (spec.kind == "bool") {
    // `add_bool_flag`, not `add_flag`: every bool flag carries the
    // `--no-X` negation the oracle's parser synthesizes. See its header.
    CLI::Option* opt = add_bool_flag(app, spec.name, spec.description);
    if (!spec.default_value.empty()) {
      // Only ever `"true"` — a bool flag defaulting to true (`planar task
      // add --editor`, `planar artifact add --editor`) is the one case
      // where CLI11 needs a default STRING on a flag, and it is what makes
      // the catalog report `"default":true` rather than a blanket false.
      opt->default_str(std::string{spec.default_value});
    }
    if (spec.required) {
      opt->required();
    }
    return;
  }
  CLI::Option* opt = app.add_option(name);
  if (spec.kind == "int") {
    opt->check(zig_int_validator());
  }
  if (spec.list) {
    // `expected(min, -1)` is CLI11's unbounded form (Option_inl.hpp: a
    // negative max becomes `expected_max_vector_size`), which is also what
    // the schema emitter reads back as `"list":true`.
    opt->expected(1, -1);
  }
  if (!spec.description.empty()) {
    opt->description(std::string{spec.description});
  }
  if (!spec.default_value.empty()) {
    opt->default_str(std::string{spec.default_value});
  }
  if (spec.required) {
    opt->required();
  }
}

/// @brief Declare one positional on `app` per its spec.
/// @param app The node to declare it on.
/// @param spec The positional's description.
auto declare_positional(CLI::App& app, positional_spec const& spec) -> void {
  CLI::Option* opt = app.add_option(std::string{spec.name});
  if (!spec.description.empty()) {
    opt->description(std::string{spec.description});
  }
  if (spec.required) {
    opt->required();
  }
}

/// @brief Reorder `parent`'s children into `order`.
///
/// ## Why this is needed, and why it is a remove-and-re-add
///
/// `apply_surface` finds-or-creates, so a group whose children are PARTLY
/// hand-written in `tree.cpp` ends up with the hand-written ones first and
/// the generated ones appended — `planar capture` listed `commits` last
/// where the oracle lists it second, and the root listed all eleven
/// hand-written verbs before the other thirty-six. CLI11 renders help and
/// this repo's catalog both in INSERTION order, so that is visible in
/// `<bin> --help` and in `schema`'s `subcommands` array.
///
/// CLI11 exposes no reordering API, but it does expose the two halves of
/// one: `get_subcommand_ptr(name)` hands back the owning `App_p` and
/// `remove_subcommand` erases it from the vector without destroying it (the
/// local `App_p` holds it alive), so re-adding appends it at the end.
/// Walking `order` front to back therefore rotates the whole child vector
/// into exactly that sequence.
///
/// A child NOT named in `order` is never re-added and so drifts to the
/// front. That cannot happen for a generated surface — `order` is derived
/// from the same spec list that created the nodes — and if a hand-written
/// node ever names a verb the oracle does not, `catalog_parity.hpp` fails
/// on it first.
/// @param parent The node whose children are being ordered.
/// @param order The child names, in the order they should appear.
auto reorder_children(CLI::App& parent, std::vector<std::string> const& order) -> void {
  for (auto const& name : order) {
    CLI::App* child = parent.get_subcommand_no_throw(name);
    if (child == nullptr) {
      continue;
    }
    CLI::App_p held = parent.get_subcommand_ptr(child);
    parent.remove_subcommand(child);
    parent.add_subcommand(std::move(held));
  }
}

} // namespace

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

auto apply_surface(CLI::App& root, std::span<node_spec const> nodes) -> std::vector<std::string> {
  // Each node's LOCAL flags, keyed by its space-joined path, so a generated
  // child can pick up its ancestors' flags below. Built up front because
  // `nodes` is pre-order and a child could otherwise only see ancestors
  // already visited -- which is true today and is not a property worth
  // depending on.
  std::map<std::string, std::span<flag_spec const>, std::less<>> flags_by_key;
  for (auto const& spec : nodes) {
    std::string key;
    for (auto const& segment : spec.path) {
      if (!key.empty()) {
        key += ' ';
      }
      key += segment;
    }
    flags_by_key.emplace(std::move(key), spec.flags);
  }

  std::vector<std::string> created;
  for (auto const& spec : nodes) {
    CLI::App* parent = &root;
    bool      ok     = true;
    for (std::size_t i = 0; i + 1 < spec.path.size(); ++i) {
      CLI::App* next = parent->get_subcommand_no_throw(std::string{spec.path[i]});
      if (next == nullptr) {
        ok = false;
        break;
      }
      parent = next;
    }
    auto const key = [&] {
      std::string out;
      for (auto const& segment : spec.path) {
        if (!out.empty()) {
          out += ' ';
        }
        out += segment;
      }
      return out;
    }();
    if (!ok) {
      created.push_back("!" + key);
      continue;
    }

    std::string const leaf{spec.path.back()};
    if (parent->get_subcommand_no_throw(leaf) != nullptr) {
      // Already declared by hand in `tree.cpp`. That declaration is
      // authoritative — see this module's interface header.
      continue;
    }

    CLI::App* node = parent->add_subcommand(leaf, std::string{spec.description});
    if (spec.group) {
      // A bare group renders its own help page and exits 0, which is what
      // the oracle does for every group node except the three duals.
      node->require_subcommand(0);
    }
    if (spec.allow_extras) {
      // See node_spec::allow_extras: this leaf's handler always answers
      // with a fixed refusal, so any unrecognized flag or positional must
      // reach it rather than being rejected by CLI11 first.
      node->allow_extras();
    }
    std::set<std::string_view, std::less<>> declared;
    for (auto const& flag : spec.flags) {
      declare_flag(*node, flag);
      declared.insert(flag.name);
    }
    for (auto const& positional : spec.positionals) {
      declare_positional(*node, positional);
    }

    // ANCESTOR FLAGS, REDECLARED (task 6139).
    //
    // etcli inherits a parent's flags into every child AT PARSE TIME; CLI11
    // does not, so a flag declared on a group was simply unavailable on its
    // generated children. Both oracle catalogs already report those flags on
    // the child with `"source":"inherited"`, so the surface CLAIMED them
    // while the parser refused them:
    //
    //     $ planar health hygiene --json
    //     error: hygiene: The following argument was not expected: --json
    //
    // A sweep over every `"source":"inherited"` flag in all three oracle
    // catalogs found exactly ONE reachable site -- `health` is the only
    // generated parent that both carries a flag and has children -- but the
    // hole is structural, so this closes it for any parent, not for `health`.
    //
    // Two constraints, and both are load-bearing:
    //
    //   FLAGS ONLY, never positionals. `tree.cpp`'s `add_handoff` header
    //   records what the other choice costs: a shared POSITIONAL name
    //   (`resume validate`) aborted every invocation of the binary at
    //   tree-build time, not just the affected verb.
    //
    //   SKIP A NAME THE CHILD ALREADY DECLARES. The child's own declaration
    //   is authoritative -- it may differ in kind, default or description --
    //   and redeclaring it would throw `OptionAlreadyAdded` from CLI11.
    //
    // The emitted catalog is unaffected: `render_flags` dedupes a redeclared
    // parent flag and labels it `inherited`, which is what it already
    // emitted. `catalog_parity.hpp` holds that byte-for-byte.
    {
      std::string ancestor_key;
      for (std::size_t i = 0; i + 1 < spec.path.size(); ++i) {
        if (!ancestor_key.empty()) {
          ancestor_key += ' ';
        }
        ancestor_key += spec.path[i];
        auto const found = flags_by_key.find(ancestor_key);
        if (found == flags_by_key.end()) {
          continue;
        }
        for (auto const& flag : found->second) {
          if (declared.contains(flag.name)) {
            continue;
          }
          declare_flag(*node, flag);
          declared.insert(flag.name);
        }
      }
    }

    created.push_back(key);
  }

  // Second pass: put every group's children back into the order the spec
  // lists them, which find-or-create cannot preserve on its own. `nodes` is
  // depth-stable-sorted out of a pre-order catalog walk, so iterating it in
  // order and appending each name under its parent key reconstructs the
  // oracle's sibling order exactly.
  std::map<std::string, std::vector<std::string>, std::less<>> order;
  for (auto const& spec : nodes) {
    std::string parent_key;
    for (std::size_t i = 0; i + 1 < spec.path.size(); ++i) {
      if (!parent_key.empty()) {
        parent_key += ' ';
      }
      parent_key += spec.path[i];
    }
    order[parent_key].emplace_back(spec.path.back());
  }
  for (auto const& [parent_key, names] : order) {
    CLI::App* parent = &root;
    bool      ok     = true;
    if (!parent_key.empty()) {
      for (auto const& segment : std::views::split(parent_key, ' ')) {
        CLI::App* next = parent->get_subcommand_no_throw(std::string{segment.begin(), segment.end()});
        if (next == nullptr) {
          ok = false;
          break;
        }
        parent = next;
      }
    }
    if (ok) {
      reorder_children(*parent, names);
    }
  }
  return created;
}

} // namespace planar::cliapp
