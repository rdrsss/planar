/// @file declare.cppm
/// @brief `planar.cmd.planar.declare` — the primitives every hand-written
/// `planar` command declaration is built from (plan 1051, M11.3).
///
/// ## Why this module exists
///
/// M11 moves each command's CLI declaration next to its handler
/// (decision 1068). Before it, `planar` declared its surface twice: seven
/// verbs by hand in `tree.cpp` and the rest as a generated `node_spec`
/// table in `surface.cpp` that `planar.cliapp.surface::apply_surface`
/// walked. `apply_surface`'s `declare_flag` / `declare_positional` are the
/// exact CLI11 shapes that produced the pinned `schema` catalog, and they
/// live in that module's anonymous namespace where a hand-written
/// declaration cannot reach them.
///
/// So this module re-states those shapes as named, readable primitives —
/// one per (kind, default, required) combination — and every folded
/// declaration is written in terms of them. The point is that the mapping
/// from "what the catalog says" to "what CLI11 is told" is stated ONCE
/// here rather than re-derived at each of the ~246 declaration sites,
/// which is what makes the fold reviewable and what keeps
/// `scripts/surface-snapshot.sh verify` byte-clean across the six waves.
///
/// ## THE SET IS INCOMPLETE AND GROWS PER WAVE
///
/// What is exported below covers the 42 specs folded by M11.3a
/// (`plan` + `task`) and nothing more. It is NOT the surface's full
/// primitive vocabulary. The remaining 204 specs are already known to need
/// at least: list-valued flags (`expected(1, -1)`), OPTIONAL positionals
/// (`k_pos_243`'s `workspace` is `.required = false`), positionals
/// carrying their own description, groups declaring `allow_extras`
/// (`scope use|pop|clear`), and REQUIRED bool flags. Each later wave adds
/// the primitives its own specs need; a wave that finds no primitive for a
/// shape should add one here rather than inline the raw CLI11 call.
///
/// ## The shapes are transcribed, not invented
///
/// Each function below mirrors one branch of `apply_surface`'s
/// `declare_flag`:
///
///   * a bool flag is `cliapp::add_bool_flag`, never `CLI::App::add_flag`,
///     because every bool carries the `--no-X` negation the oracle's
///     parser synthesized;
///   * an int flag carries `cliapp::zig_int_validator()`, which is both
///     what makes the catalog report `"int"` and what keeps Zig's
///     `std.fmt.parseInt` semantics enforced at PARSE time;
///   * a declared default is `Option::default_str`, not `default_val` —
///     the catalog reports the declared string, and the handler layer, not
///     CLI11, applies it.
///
/// Nothing here opens a database or reaches an engine: this is
/// declaration only, and every function returns `void` because no call
/// site needs the `CLI::Option*` back.
module;

export module planar.cmd.planar.declare;

import std;
import cli11;

namespace planar::cmd {

/// @brief The `--json` flag, the single most common declaration in the tree.
/// @param app The node to declare it on.
export auto add_json(CLI::App& app) -> void;

/// @brief A boolean flag, with the `--no-X` negation attached.
/// @param app The node to declare it on.
/// @param name The canonical long name, `--` included.
/// @param desc The help line, or empty for none.
export auto add_bool(CLI::App& app, std::string_view name, std::string_view desc = {}) -> void;

/// @brief A boolean flag whose DECLARED default is true.
///
/// The one case where CLI11 needs a default string on a flag: it is what
/// makes the catalog report `"default":true` rather than a blanket false.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param desc The help line, or empty for none.
export auto add_bool_default_true(CLI::App& app, std::string_view name, std::string_view desc = {}) -> void;

/// @brief A string-valued flag with no declared default.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param desc The help line, or empty for none.
export auto add_string(CLI::App& app, std::string_view name, std::string_view desc = {}) -> void;

/// @brief A string-valued flag carrying a declared default.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param default_value The default the catalog reports.
/// @param desc The help line, or empty for none.
export auto add_string_default(CLI::App& app, std::string_view name, std::string_view default_value, std::string_view desc = {})
    -> void;

/// @brief An integer flag.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param desc The help line, or empty for none.
export auto add_int(CLI::App& app, std::string_view name, std::string_view desc = {}) -> void;

/// @brief An integer flag carrying a declared default.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param default_value The default the catalog reports.
/// @param desc The help line, or empty for none.
export auto add_int_default(CLI::App& app, std::string_view name, std::string_view default_value, std::string_view desc = {})
    -> void;

/// @brief A REQUIRED integer flag.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param desc The help line, or empty for none.
export auto add_int_required(CLI::App& app, std::string_view name, std::string_view desc = {}) -> void;

/// @brief A required positional argument.
///
/// Every positional in M11.3a's 42 specs is required and carries no help
/// line of its own, so this is the only positional primitive so far. The
/// surface as a whole DOES have optional and described positionals (see
/// the module header); each needs its own primitive rather than a
/// defaulted parameter here, so that the call site keeps saying which
/// shape it means.
/// @param app The node to declare it on.
/// @param name The positional's name.
export auto add_positional(CLI::App& app, std::string_view name) -> void;

} // namespace planar::cmd
