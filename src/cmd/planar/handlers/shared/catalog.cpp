/// @file catalog.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.catalog`.

module planar.cmd.planar.handlers.catalog;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.completion;
import planar.cliapp.schema;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.surface;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

auto schema(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result {
  (void)args;
  // FRAGMENT renderer: `schema_json` documents itself as returning no
  // trailing newline, so the newline is appended here — matching
  // handlers/schema.zig's `writeAll(catalog)` + `writeAll("\n")`.
  //
  // The summaries table is what makes `"summary"` differ from
  // `"description"` on the 57 nodes where the oracle's does. See
  // `planar.cliapp.schema`'s header, divergence 1.
  ctx.out() << cliapp::schema_json(root, surface_summaries(), surface_empty_string_defaults()) << '\n';
  return {};
}

auto completion(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result {
  // The parser enforces `required = true` on <shell>, so an absent value
  // here is unreachable from argv — it is a parse error long before
  // dispatch. Treated as invalid input rather than asserted, since a direct
  // in-process caller (handlers.t.cpp) can construct one.
  auto const shell_name = cliapp::positional_string(args, "shell");
  if (!shell_name.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "unsupported shell ''; supported: bash, zsh, fish"));
  }

  std::optional<cliapp::shell> target;
  if (*shell_name == "bash") {
    target = cliapp::shell::bash;
  } else if (*shell_name == "zsh") {
    target = cliapp::shell::zsh;
  } else if (*shell_name == "fish") {
    target = cliapp::shell::fish;
  }
  if (!target.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("unsupported shell '{}'; supported: bash, zsh, fish", *shell_name)));
  }

  // `generate_script` returns the whole script; the Zig handler writes it
  // with `writeAll` and appends nothing, so neither does this.
  ctx.out() << cliapp::generate_script(root, *target);
  return {};
}

/// @brief Declare the `completion` leaf.
///
/// `completion` and `schema` are separate top-level verbs at
/// adjacent catalog positions (36th and 37th) sharing this module, so
/// each gets its own function — see `declare_promote`.

/// @brief Declare the `schema` leaf. See `declare_completion`.

} // namespace planar::cmd::handlers
