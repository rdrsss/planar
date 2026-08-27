/// @file extract.cppm
/// @brief `planar.engine.templates.extract` — write the embedded baseline
/// templates out to disk (plan 996, task 6190).
///
/// Behavior-preserving port (D2) of `zig/src/engine/templates/init.zig`'s
/// `initOnDisk`. Named `extract` rather than `init` because this bucket
/// sits beside `engine/config`, which already owns an `init` module for the
/// unrelated `planar config init` verb; two `planar.engine.*.init` modules
/// one directory apart is a coin-flip at every import site.
///
/// ## Idempotent, and NOT overridable
///
/// An existing file is never overwritten — not even with `--force`. The
/// leaf declares that flag and the oracle's handler explicitly discards it
/// (`_ = args.force;`), so `templates init --force` on a fully-populated
/// root prints `nothing to do` and touches nothing. Oracle-captured. That
/// is arguably wrong for a flag named `--force`, but it is the shipped
/// behaviour and this port reproduces it rather than deciding what the flag
/// should have meant; the honest fix is a follow-up task against the
/// oracle, not a unilateral divergence here.
///
/// ## Everything is an explicit path, and the template SET is a parameter
///
/// No function here calls `std::getenv`, and — the part D15 forced —
/// nothing here imports `planar.engine.config.templates_embed` either.
/// Both `engine_config` and `engine_templates` are LAYER 2, and
/// `cmake/architecture.cmake` FATALs at configure time on an engine→engine
/// edge; only layer 3 (`cmd`) may compose two engine buckets.
///
/// So the embedded set arrives as a span the caller fills from
/// `config::embedded_templates()`. That is not a workaround grafted on to
/// satisfy the check — it is the same posture `engine_local` adopts for the
/// sandbox root, and it has the same payoff: a test can hand this function
/// three synthetic entries and a `tmpdir` and exercise the skip-existing
/// logic without the real embedded set or a real home directory anywhere in
/// reach.

module;

export module planar.engine.templates.extract;

import std;

namespace planar::engine::templates {

/// @brief One template to write out: an external-system slug, a kind, and
/// the raw JSON body.
///
/// Structurally identical to `config::template_file` and deliberately NOT
/// that type — see this file's header on the layer rule. The caller maps
/// one onto the other; the set is ten entries, so the copy is free.
export struct embedded_file {
  std::string_view system; ///< External system slug (e.g. `"jira"`).
  std::string_view kind;   ///< Kind within the system (e.g. `"epic"`).
  std::string_view body;   ///< Verbatim JSON content.
};

/// @brief Why an extraction failed.
export enum class extract_error : std::uint8_t {
  invalid_input, ///< `root` was empty. Zig returns `error.InvalidInput` for this.
  write_failed,  ///< A directory could not be created or a file could not be written.
};

/// @brief Write every entry of `embedded` to
/// `<root>/default/<system>/<kind>.json`, skipping files that already
/// exist.
/// @param root The operator's templates directory (absolute; must be
/// non-empty).
/// @param embedded The template set to write, in the order to write it.
/// @return The paths NEWLY written, in `embedded`'s order — empty when
/// everything was already present.
export auto extract_defaults(const std::filesystem::path& root, std::span<const embedded_file> embedded)
    -> std::expected<std::vector<std::string>, extract_error>;

} // namespace planar::engine::templates
