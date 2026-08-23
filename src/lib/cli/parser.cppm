/// @file parser.cppm
/// @brief `planar.cli.parser` — the runtime argv parser: subcommand-path
/// resolution, flag/positional coercion, and `--help` detection.
///
/// Behavior-preserving port of zig/vendor/etcli-zig/src/cli/parser.zig (D2,
/// D9). The two-pass shape survives intact: pass 1 walks argv, folding
/// recognized subcommand tokens into a resolved path and copying every
/// other token (flags, their values, positionals, everything after `--`)
/// into a "tail" sequence; pass 2 (`parse_leaf`) walks the tail against the
/// matched leaf's flag/positional specs. What does NOT survive is Zig's
/// module-static backing-buffer machinery (`rest_buf`, `list_buf`,
/// `help_path_buf`, …) — those exist only because etcli-zig returns slices
/// INTO a comptime-sized, single-threaded-by-construction buffer so the
/// zero-allocation contract holds across a `*const anyopaque`-typed args
/// struct. This port returns an ordinary owned `match_result` (a
/// `std::unordered_map<std::string, value>` — see flag.cppm's file
/// comment), so the equivalent storage is just `std::vector`/`std::string`
/// values living in that map; no shared mutable backing buffer, no
/// single-invocation-validity caveat.
module;

export module planar.cli.parser;

import std;
import planar.cli.flag;
import planar.cli.cmd;
import planar.cli.error;

namespace planar::cli {

/// @brief The parsed outcome of a successful, non-help match: the resolved
/// command path, every flag/positional value keyed by canonical long name
/// (`"--title"`) or positional name (`"title"`), and any `rest_field`
/// overflow tokens.
export struct match_result {
  std::vector<std::string>               path;        ///< The resolved subcommand path.
  std::unordered_map<std::string, value> flags;       ///< Flag values keyed by canonical long name.
  std::unordered_map<std::string, value> positionals; ///< Positional values keyed by positional name.
  std::vector<std::string>               rest;        ///< Overflow tokens when the leaf declared a `rest_field`.
};

/// @brief Outcome of `parse`: either a resolved `match`, or a help request
/// (etcli-zig's `.help` variant) carrying the path whose help page the caller
/// should render.
export struct parse_outcome {
  bool                     is_help = false; ///< True when `--help`/`-h` (or a bare parent verb) was seen.
  std::vector<std::string> help_path;       ///< The path whose help page should render, when `is_help`.
  match_result             match;           ///< The resolved match, valid when `!is_help`.
};

/// @brief Parse `argv` (argv[0] is the program name, ignored for matching)
/// against `root`.
/// @param root The command tree to match against.
/// @param argv The full argv, including argv[0].
/// @return The parsed outcome, or a structured `parse_error_detail` on
/// failure (render with `format_error`; map to an exit code with
/// `exit_code_for_parse_error_planar_binary` for the `planar` operator
/// binary's policy, or with `planar.cli.exit`'s binary-aware
/// `exit_code_for(domain_error_kind::parse_error, binary_kind)` for any
/// other binary — see error.cppm's file comment for why these are two
/// distinctly-named functions, not one overload set).
export auto parse(cmd const& root, std::span<std::string const> argv) -> std::expected<parse_outcome, parse_error_detail>;

} // namespace planar::cli
