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
/// What is exported below covers exactly the shapes the waves folded SO
/// FAR needed, and nothing more; it is NOT the surface's full primitive
/// vocabulary. Deliberately stated without a folded/remaining COUNT: two
/// such counts went stale within one wave of being written, and the live
/// number is a `git grep -c` away (`surface_nodes()`'s entries, minus its
/// stripped ordering anchors, are what remain).
///
/// Wave by wave: M11.3a (`plan`, `task`) needed only the bool / int /
/// string / positional shapes; M11.3b (`models`, `annotate`, `workbench`)
/// added `add_string_required` and `add_positional_optional`; M11.3c (the
/// drafting quartet) needed no new primitive at all; M11.3d (`capture`,
/// `assoc`, `workspace`, `handoff`, `templates`, `bench`) added
/// `add_string_list` for `bench start --task`, the surface's first
/// repeatable flag.
///
/// Shapes that are still UNBUILT because no folded spec has needed one
/// yet: positionals carrying their own description, groups declaring
/// `allow_extras` (`scope use|pop|clear`), and REQUIRED bool flags. Each
/// later wave adds the primitives its own specs need; a wave that finds no
/// primitive for a shape should add one here rather than inline the raw
/// CLI11 call.
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

/// @brief A REQUIRED string flag.
///
/// `models registry`'s ten leaves are where this shape concentrates: eight
/// of its ten leaves declare their selectors required (`list` and `export`
/// declare only `--json` and carry no required flag), so the refusal for a
/// missing `--candidate` / `--role` is CLI11's parse error rather than a
/// handler-level check.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param desc The help line, or empty for none.
export auto add_string_required(CLI::App& app, std::string_view name, std::string_view desc = {}) -> void;

/// @brief A REPEATABLE string flag.
///
/// `expected(1, -1)` is CLI11's unbounded form, and it is what makes the
/// catalog report `"list":true` as well as what lets the flag be supplied
/// more than once. `bench start --task` is the only site today; the shape
/// carries no default and is never required, so those two parameters are
/// deliberately absent rather than defaulted.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param desc The help line, or empty for none.
export auto add_string_list(CLI::App& app, std::string_view name, std::string_view desc = {}) -> void;

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
/// Kept SEPARATE from `add_positional_optional` rather than collapsed into
/// one function with a defaulted `required` parameter, so that every call
/// site keeps saying which shape it means. The distinction is visible
/// behaviour: `workbench pull <plan>` refuses a bare invocation at parse
/// time where `workbench status` renders the whole workbench.
/// @param app The node to declare it on.
/// @param name The positional's name.
export auto add_positional(CLI::App& app, std::string_view name) -> void;

/// @brief An OPTIONAL positional argument.
///
/// See `add_positional` for why the two are separate primitives.
/// @param app The node to declare it on.
/// @param name The positional's name.
export auto add_positional_optional(CLI::App& app, std::string_view name) -> void;

} // namespace planar::cmd
