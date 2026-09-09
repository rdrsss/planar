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
/// repeatable flag; M11.3e (`scope`, `audit`, `config`, `local`, `links`,
/// `run`, `feedback`) added `set_allow_extras` for `scope use|pop|clear`,
/// the only three leaves in the tree that take one; M11.3f (the
/// thirty-three remaining singletons) added `add_positional_described`.
///
/// The only shape still UNBUILT is a REQUIRED BOOL flag, and it is
/// unbuilt because the surface contains none — `apply_surface`'s
/// `declare_flag` has the arm, and nothing in the tree ever took it.
/// M11.3f was the last wave, so a NEW primitive here now means a NEW
/// verb rather than one more transcription; add it here rather than
/// inlining the raw CLI11 call.
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

/// @brief A required positional argument carrying its own DESCRIPTION.
///
/// Seven of the surface's positionals declare help text of their own
/// (`promote`/`demote`/`link`'s `ref`, `unlink`'s `link-id`, `search`'s
/// `query`, `completion`'s `shell`, `test-spec status`'s `plan`); the
/// other twenty-odd declare none, and CLI11 renders an empty description
/// column for them. Split from `add_positional` rather than folded into
/// it with a defaulted parameter for the same reason `add_positional` and
/// `add_positional_optional` are separate: the call site should say which
/// shape it means.
///
/// Every described positional in the tree is REQUIRED, so there is no
/// optional counterpart. Add one only when a spec needs it.
/// @param app The node to declare it on.
/// @param name The positional's name.
/// @param desc The help line.
export auto add_positional_described(CLI::App& app, std::string_view name, std::string_view desc) -> void;

/// @brief Let a leaf accept ANY unrecognized flag or positional.
///
/// The one NODE-level primitive here; everything else declares an option.
/// It mirrors `apply_surface`'s `spec.allow_extras` arm, and it exists for
/// exactly three leaves: `scope use`, `scope pop` and `scope clear`, all
/// removed in plan 153 M5. Their handlers answer with the same fixed
/// refusal whatever the input, and the oracle tolerated the arbitrary
/// legacy flags and positionals a pre-M5 caller would still be passing —
/// so CLI11 must hand those through rather than refuse them at parse time,
/// which would change both the message and the exit code.
///
/// Named `set_` rather than `add_` deliberately: it toggles a property of
/// the node instead of appending to its option list, and every call site
/// should read as the exception it is.
///
/// THIS PROPERTY IS UNGUARDED, and it is the only thing M11.3e folded that
/// is. Two break-probes at task 6635 established it: ADDING
/// `set_allow_extras` to a leaf that has none (`config path`) left
/// `scripts/surface-snapshot.sh verify` clean at 312 points, and REMOVING
/// all three of `scope`'s left both that gate clean AND all 861
/// `cmd_planar` Catch2 cases green. The property appears in neither the
/// `schema` catalog nor any `--help` page, which is all the gate hashes,
/// and the difference it makes is a MESSAGE difference at the same exit
/// code — `error: use: The following arguments were not expected:
/// --stack-name bar` instead of the plan-153-M5 removal paragraph, both
/// exit 2. Treat a change to any of the three calls as unverifiable by the
/// gate and check `planar scope use <slug> --<stale-flag>` by hand.
/// @param app The leaf to relax.
export auto set_allow_extras(CLI::App& app) -> void;

} // namespace planar::cmd
