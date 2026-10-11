/// @file src/cmd/planar/handlers/update/state.cppm
/// @brief `planar.cmd.planar.handlers.update.state` — the installation records
/// and release-input grammars `planar update` reads (plan 1122 M3, task
/// rel-update-verb; tech spec 677, "The update verb" and "The bootstrap").
///
/// Every function here is a native reading of a rule the shell side already
/// enforces, kept byte-compatible with it so the verb and the bootstrap
/// (`scripts/get-planar.sh`) or installer (`install.sh` and
/// `scripts/install-lib/`) agree on the same input:
///
///   - `canonical_path` is `planar_canonical_path` (prefix-guard.sh): the
///     lock, the journal and the handoff are keyed on its exact bytes.
///   - `read_journal` is `recovery_journal_valid` (journal.sh) plus the keys
///     the verb reports from.
///   - `read_release` reads `release.json` with Glaze (decision 1334: the
///     release is recorded there only).
///   - `release_base_valid`, `normalize_base`, `version_valid`,
///     `select_checksum_record`, `glibc_refusal` and `printable` are the
///     bootstrap's `url_parse`/`base_valid`, `base_normalize`,
///     `version_valid`, `select_record`, `glibc_check` and `printable`, with
///     the bootstrap's refusal wording.
///
/// Nothing here writes, and nothing opens the database. Fallible readers
/// return `std::expected` with a one-line message; the caller decides the
/// exit code.
module;

export module planar.cmd.planar.handlers.update.state;

import std;

namespace planar::cmd::update {

/// @brief Resolve `input` the way `planar_canonical_path` does: absolute, every
/// symlink and `.`/`..` resolved, a missing tail appended lexically.
/// @param input An absolute path (a relative one is resolved against `cwd`).
/// @param cwd The directory a relative `input` is relative to.
/// @return The canonical path, or unset for an empty path or a symlink loop
/// (more than 40 links).
export auto canonical_path(std::string_view input, std::string_view cwd) -> std::optional<std::string>;

/// @brief The recovery journal's file name under the install root.
export inline constexpr std::string_view k_journal_name = ".planar-journal";

/// @brief A validated recovery journal (journal.sh, format version 1).
export struct journal {
  std::string phase; ///< prepared | mutating | complete | aborted-before-mutation | uninstalling.
  std::map<std::string, std::string, std::less<>> keys; ///< Every `key=value` line, `root` and `phase` included.
  /// @brief One key's value, or empty when absent.
  /// @param key The key.
  /// @return The value.
  [[nodiscard]] auto value(std::string_view key) const -> std::string;
};

/// @brief What `read_journal` found.
export enum class journal_status : std::uint8_t {
  absent,  ///< No journal file (and no symlink in its place).
  invalid, ///< Something is there but it is not a valid journal for this root.
  valid,   ///< A valid journal; see `journal_reading::record`.
};

/// @brief The outcome of `read_journal`.
export struct journal_reading {
  journal_status status = journal_status::absent; ///< What was found.
  journal        record;                          ///< The journal when `status` is `valid`.
};

/// @brief Read `<canonical_root>/.planar-journal` under journal.sh's validity
/// rules: a regular file owned by the effective user, at most 65536 bytes of
/// tab, newline, printable ASCII or bytes of 0x80 and above, line 1
/// `planar-journal 1`, unique `[a-z][a-z0-9_]*=` keys, `root` byte-equal to
/// `canonical_root`, and a known `phase`.
/// @param canonical_root The canonical install root.
/// @return What was found. Reading never changes the file.
export auto read_journal(std::string_view canonical_root) -> journal_reading;

/// @brief The fields of a `release.json` the verb uses.
export struct release_info {
  std::string                  version;        ///< The release tag, or `dev` for a source install.
  std::optional<std::uint64_t> schema_version; ///< The highest embedded migration, when recorded.
  std::string                  os_floor;       ///< The OS floor (`26.0`, or the glibc version on Linux).
  std::string                  sha;            ///< The commit, when recorded.
};

/// @brief Read a `release.json` with Glaze.
///
/// `schema_version` may be a JSON number or a string of digits (the format
/// keeps its serialized type); anything else leaves it unset.
/// @param path The file.
/// @return The fields; unset when the file does not exist; a message when it
/// exists but is not a JSON object with a string `version`.
export auto read_release(const std::filesystem::path& path) -> std::expected<std::optional<release_info>, std::string>;

/// @brief The release base used when `PLANAR_RELEASE_URL` is unset.
export inline constexpr std::string_view k_default_release_base = "https://github.com/rdrsss/planar/releases";

/// @brief The parts of a release base the verb needs after validation.
export struct base_parts {
  std::string scheme; ///< `https`, `http` or `file`.
  std::string path;   ///< The path, for a `file` base the directory on disk.
};

/// @brief `base` with every trailing `/` removed (the bootstrap's `base_normalize`).
/// @param base The release base as given.
/// @return The normalized base.
export auto normalize_base(std::string_view base) -> std::string;

/// @brief Validate a normalized release base with the bootstrap's grammar:
/// `https://HOST[:PORT]/...`, `file:///PATH`, or `http://127.0.0.1[:PORT]/...`
/// / `http://localhost[:PORT]/...`; only `[A-Za-z0-9._~:/%+-]`; no userinfo,
/// query or fragment; a file path without `%`, `.` or `..` segments.
/// @param base The normalized base.
/// @return Its scheme and path, or unset when it is refused.
export auto release_base_valid(std::string_view base) -> std::optional<base_parts>;

/// @brief The bootstrap's refusal of a `PLANAR_RELEASE_URL` value.
/// @param given The value as given.
/// @return The one-line message.
export auto release_base_refusal(std::string_view given) -> std::string;

/// @brief A release tag: `^v[0-9]+\.[0-9]+\.[0-9]+$`.
/// @param tag The candidate.
/// @return Whether it matches.
export auto version_valid(std::string_view tag) -> bool;

/// @brief `text` cut to printable ASCII and 200 bytes, for echoing untrusted data.
/// @param text The untrusted text.
/// @return The printable form.
export auto printable(std::string_view text) -> std::string;

/// @brief Select the one checksum record for `asset` from a `SHA256SUMS` body.
///
/// Exactly one line may end in `asset`; it must be exactly
/// `<64 lowercase hex>  <asset>` (no path, no binary marker).
/// @param sums The `SHA256SUMS` body.
/// @param asset The asset's bare file name.
/// @return The expected digest, or the bootstrap's refusal message.
export auto select_checksum_record(std::string_view sums, std::string_view asset) -> std::expected<std::string, std::string>;

/// @brief The bootstrap's glibc floor check, as a pure function.
/// @param floor The bundle's `os_floor`.
/// @param ldd_first_line The first line of `ldd --version`, or unset when
/// `ldd` is not installed.
/// @return The refusal message, or unset when the host is new enough.
export auto glibc_refusal(std::string_view floor, const std::optional<std::string>& ldd_first_line) -> std::optional<std::string>;

/// @brief Where the bootstrap points an unsupported host.
export inline constexpr std::string_view k_source_pointer =
    R"(build from source instead: https://github.com/rdrsss/planar (INSTALL.md, "Build from source"))";

/// @brief Map `uname` to a bundle platform the way the bootstrap does.
/// @param sysname `uname -s`.
/// @param machine `uname -m`.
/// @return `macos-arm64` or `linux-x86_64`, or the bootstrap's refusal message.
export auto platform_for(std::string_view sysname, std::string_view machine) -> std::expected<std::string, std::string>;

} // namespace planar::cmd::update
