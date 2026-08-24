/// @file surface.cpp
/// @brief Implementation of `planar.cliapp.surface::apply_surface`.

module planar.cliapp.surface;

import std;
import cli11;
import planar.cliapp.args;

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
    CLI::Option* opt = app.add_flag(name);
    if (!spec.description.empty()) {
      opt->description(std::string{spec.description});
    }
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

auto apply_surface(CLI::App& root, std::span<node_spec const> nodes) -> std::vector<std::string> {
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
    for (auto const& flag : spec.flags) {
      declare_flag(*node, flag);
    }
    for (auto const& positional : spec.positionals) {
      declare_positional(*node, positional);
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
