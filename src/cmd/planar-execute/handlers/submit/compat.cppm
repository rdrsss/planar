/// @file compat.cppm
/// @brief The compatibility tuple a client and the daemon it joins must agree on
///        (plan 1033 M2, task 6503; tech-spec D7).
///
/// A profile's daemon outlives the client that started it. The next client to
/// come along may be a different build, pointed at a different Planar
/// database, carrying different workflows — and it would join that daemon
/// silently, because a Unix socket says nothing about who is behind it.
///
/// The tuple is what makes that joinable-or-not decision explicit. The client
/// that starts a daemon RECORDS the tuple beside the profile's state; every
/// later client COMPUTES its own and compares. A mismatch is refused, naming
/// the fields that differ — never "incompatible host", which sends an operator
/// looking in the wrong place.
///
/// Every fallible boundary returns `std::expected`; no exception crosses it.
export module planar.cmd.planar_execute.compat;

import std;
import planar.cmd.planar_execute.host;
import planar.cmd.planar_execute.profile;

export namespace planar::cmd::execute {

/// @brief One side's identity: what this client is, and what it expects.
///
/// Every field is something that changes what a run MEANS, which is the test
/// for belonging here. The daemon's build and the wire version decide whether
/// the two can speak at all; the bundle and policy digests decide what work it
/// will admit; the database, workbench and sibling-binary paths decide which
/// Planar the workflows will act on — a daemon serving one operator's database
/// must not quietly execute another's claim.
struct compatibility_tuple {
  std::string protocol_version_;      ///< Wire contract expected, e.g. `centurion.v1`.
  std::string daemon_build_;          ///< Installed daemon's build identity (tag + binary digest).
  std::string bundle_digest_;         ///< Digest over the profile's bundle directory; empty when it has none.
  std::string command_policy_digest_; ///< Digest of the command-policy file; empty when unset.
  std::string planar_db_;             ///< Canonical (real) path of the Planar database.
  std::string workbench_root_;        ///< Canonical workbench root.
  std::string sibling_bin_dir_;       ///< Directory the Planar binaries were invoked from.

  /// @brief Field-by-field equality. @param other The other side. @return True when every field matches.
  [[nodiscard]] auto operator==(const compatibility_tuple& other) const -> bool = default;
};

/// @brief Why two tuples cannot serve each other.
struct compatibility_mismatch {
  std::string field_;    ///< The tuple field that differs.
  std::string recorded_; ///< What the running daemon was started with.
  std::string current_;  ///< What this client carries.
};

/// @brief Inputs the tuple is computed from that do not come from the profile.
struct compatibility_inputs {
  std::filesystem::path daemon_;                           ///< The installed `centuriond`.
  std::filesystem::path sibling_bin_dir_;                  ///< Where this binary's siblings live.
  std::filesystem::path workbench_root_;                   ///< Workbench root for this operator.
  std::string           protocol_version_{"centurion.v1"}; ///< Wire contract this client speaks.
};

/// @brief Digest one directory's contents, path and bytes together.
///
/// Over the SORTED relative paths and the digest of each file's bytes, so the
/// result is stable across filesystems and changes when a file is added,
/// removed, renamed or edited. An absent directory digests to the empty
/// string, which is a distinct value from any directory's digest.
/// @param directory The directory to digest; may be absent.
/// @return The digest, or empty when the directory does not exist.
[[nodiscard]] auto directory_digest(const std::filesystem::path& directory) -> std::string;

/// @brief Compute this client's tuple for a profile.
/// @param resolved The resolved profile.
/// @param inputs The non-profile inputs.
/// @return The tuple.
[[nodiscard]] auto compute_tuple(const profile& resolved, const compatibility_inputs& inputs) -> compatibility_tuple;

/// @brief Record the tuple beside the profile's state, for later clients.
/// @param layout The profile's layout.
/// @param tuple The tuple to record.
/// @return Nothing, or a diagnostic.
[[nodiscard]] auto record_tuple(const host_layout& layout, const compatibility_tuple& tuple) -> std::expected<void, std::string>;

/// @brief Read the tuple a running daemon was started with.
/// @param layout The profile's layout.
/// @return The recorded tuple, or nullopt when none was recorded.
[[nodiscard]] auto recorded_tuple(const host_layout& layout) -> std::optional<compatibility_tuple>;

/// @brief Compare what a daemon was started with against what this client carries.
///
/// An ABSENT record is compatible. A daemon started before this check existed,
/// or by a client that never recorded one, is not evidence of a conflict — and
/// refusing on absence would make every upgrade a hard failure on first run.
/// @param recorded What the daemon was started with, when known.
/// @param current This client's tuple.
/// @return Every field that differs, in tuple order; empty when compatible.
[[nodiscard]] auto compare_tuples(const std::optional<compatibility_tuple>& recorded, const compatibility_tuple& current)
    -> std::vector<compatibility_mismatch>;

/// @brief The refusal an operator reads, naming every field that differs.
/// @param mismatches The differing fields.
/// @param profile_name The profile the daemon serves.
/// @return One diagnostic, newline-separated per field.
[[nodiscard]] auto mismatch_text(std::span<const compatibility_mismatch> mismatches, std::string_view profile_name)
    -> std::string;

} // namespace planar::cmd::execute
