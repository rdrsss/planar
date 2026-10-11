/// @file src/cmd/planar/handlers/update/lock.cppm
/// @brief `planar.cmd.planar.handlers.update.lock` — the native port of the
/// common mutation ownership protocol (plan 1122 M3, task rel-update-verb;
/// tech spec 677, "Mutation ownership and cleanup"; decision 1328).
///
/// The protocol's specification is the header of
/// `scripts/install-lib/mutation-lock.sh`, and this module follows it exactly
/// so the two interoperate: an install or uninstall running the shell
/// library and an update running this code serialize on the same records.
///
///   - The coordination directory is `<canonical root>.lock`, a sibling of
///     the install root that no Planar program ever removes. It is created
///     with mode 0700 and refused when it is a symlink, not a directory, not
///     owned by the effective user, or writable by its group or others. Only
///     `st_mode`'s permission bits are read, never an ACL: a macOS ACL entry
///     granting another user write is not detected (a Linux POSIX ACL that
///     grants write shows in the group bits and is refused).
///   - Records are `owner.<G>`, `released.<G>`, `handoff.<G>` and
///     `cand.<pid>.<nonce>`. Ownership of generation G is taken by writing a
///     candidate and `link(2)`-ing it to `owner.<G>`: of any number of
///     racers exactly one link succeeds.
///   - Records are written in format 2 (`planar-mutation-lock 2`), which
///     names the host by its identity (`host=`, see host_key()) and keeps
///     `node=` (`uname -n`) for diagnostics only. Format 1, written by an
///     older build, has only `node=`; it is read, never written.
///   - The current owner is the highest generation. It is free when released
///     or when its owner is PROVEN dead on this host (`kill(pid, 0)` gives
///     `ESRCH`, or the pid exists with a different start token). A record is
///     on this host when its `host` equals host_key() and is not `none`, or,
///     in format 1, when its `node` equals `uname -n`: a renamed host keeps
///     its identity, so a crashed owner's format-2 record is still recovered,
///     while a format-1 record under another name refuses. A live owner is
///     never stolen from; anything ambiguous refuses and preserves every
///     file.
///   - The start token is `proc:<boot_id>:<starttime>` where
///     `/proc/<pid>/stat` is readable, otherwise `psu:<lstart>` from
///     `TZ=UTC LC_ALL=C ps -o lstart= -p <pid>` with blanks squeezed. The
///     `ps` form is computed by running `ps` exactly as the shell does, in UTC
///     because `lstart` renders in the caller's zone, so both sides and every
///     time zone read the same bytes for the same process. A legacy `ps:`
///     record (local-zone start, written by an older release) naming a live
///     pid is ambiguous and refuses; it is never reclaimed as a reused pid.
///
/// Refusal texts are the shell library's own, so an operator sees the same
/// diagnostic whichever program refused.
///
/// The updater acquires with operation `update` and its temporary
/// directory recorded as `tmp`, then execs the installer with
/// `PLANAR_MUTATION_HANDOFF=<G>:<nonce>`; `exec(2)` keeps the pid and start
/// time, which is what lets the installer's `planar_lock_adopt` accept the
/// record as its own. This module never adopts; only the installer does, and
/// it accepts an older updater's format-1 record in that record's own forms.
///
/// Thread safety: the functions hold no state between calls. Two acquisitions
/// in one process are distinct owners only by nonce; the protocol is
/// designed for one acquisition per process.
module;

export module planar.cmd.planar.handlers.update.lock;

import std;

namespace planar::cmd::update::lock {

/// @brief The update-temporaries namespace under the install root.
export inline constexpr std::string_view k_update_namespace = ".planar-update";

/// @brief One parsed ownership record.
export struct record {
  std::string version;   ///< The record format: "1" or "2".
  std::string gen;       ///< The generation, as written.
  std::string operation; ///< install | update | uninstall.
  std::string pid;       ///< The owning process id, as written.
  std::string start;     ///< The owner's start token.
  std::string host;      ///< The owner's host identity (format 2), else empty.
  std::string node;      ///< The owner's host node name (format 1: its identity).
  std::string nonce;     ///< 32 lowercase hex digits.
  std::string root;      ///< The canonical install root.
  std::string tmp;       ///< The updater's temporary directory, or empty.
};

/// @brief An acquired generation.
export struct ownership {
  std::string dir;           ///< The coordination directory.
  std::string root;          ///< The canonical install root.
  std::string gen;           ///< The generation this process owns.
  std::string nonce;         ///< This acquisition's nonce.
  std::string operation;     ///< The operation recorded.
  std::string tmp;           ///< The temporary directory recorded, or empty.
  std::string reclaimed;     ///< The reclaimed dead owner's `<operation> pid <pid>`, or empty.
  std::string reclaimed_tmp; ///< That owner's validated update temporary, or empty.
};

/// @brief The coordination directory for a canonical root: one trailing `/`
/// removed, then `.lock` appended.
/// @param canonical_root The canonical install root.
/// @return The directory path.
export auto lock_dir(std::string_view canonical_root) -> std::string;

/// @brief The start token of `pid` (see this module's header).
/// @param pid The process id.
/// @return The token, or unset when it cannot be read.
export auto start_token(std::int64_t pid) -> std::optional<std::string>;

/// @brief This host's node name (`uname -n`), or `unknown`.
/// @return The node name.
export auto node_name() -> std::string;

/// @brief This host's identity, as `scripts/install-lib/mutation-lock.sh`'s
/// `_pl_host_key` derives it, byte for byte: `machine-id:<id>` from a valid
/// `/etc/machine-id`, else `platform-uuid:<UUID>` from
/// `LC_ALL=C /usr/sbin/ioreg -rd1 -c IOPlatformExpertDevice`, else
/// `node:<uname -n>`, then `/pidns:<inode>` appended when
/// `/proc/self/ns/pid` reads `pid:[<inode>]`; `none` when none of the three
/// is available. Read afresh on every call.
/// @return The identity.
export auto host_key() -> std::string;

/// @brief Parse an ownership record file: a regular file (not a symlink) of
/// at most 32 lines, line 1 `planar-mutation-lock 1` or `planar-mutation-lock
/// 2`, then unique `key=value` lines known to that format with a numeric gen
/// and pid, a known operation, a 32-hex nonce and non-empty start, node and
/// root, and in format 2 a non-empty host.
/// @param path The record file.
/// @return The record, or unset when it is not well formed.
export auto parse_record(const std::filesystem::path& path) -> std::optional<record>;

/// @brief Acquire ownership of `canonical_root` for `operation`.
/// @param canonical_root The canonical install root.
/// @param operation install, update or uninstall.
/// @param tmp The update temporary to record (update only), or empty.
/// @return The ownership, or the shell library's one-line refusal.
export auto acquire(std::string_view canonical_root, std::string_view operation, std::string_view tmp)
    -> std::expected<ownership, std::string>;

/// @brief Whether this process still owns `held`'s generation: it is the
/// highest, not released, and the record carries this nonce and pid.
/// @param held The ownership.
/// @return Nothing when still owned, else the refusal.
export auto assert_owner(const ownership& held) -> std::expected<void, std::string>;

/// @brief Release `held` by linking `released.<G>` to its record. Only the
/// owner releases; a second release is a no-op.
/// @param held The ownership.
export auto release(const ownership& held) -> void;

/// @brief Whether `dir` is a valid update temporary of `canonical_root`:
/// byte for byte `<root>/.planar-update/<name>`, `<name>` matching
/// `[A-Za-z0-9][A-Za-z0-9._-]*`, and both it and the namespace real
/// directories owned by the effective user.
/// @param canonical_root The canonical install root.
/// @param dir The candidate.
/// @return Whether it is valid.
export auto update_tmp_valid(std::string_view canonical_root, std::string_view dir) -> bool;

} // namespace planar::cmd::update::lock
