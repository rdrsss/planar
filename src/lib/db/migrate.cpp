/// @file migrate.cpp
/// @brief Implementation of `planar.db.migrate` (see migrate.cppm).

module;

module planar.db.migrate;

import std;
import planar.db;
import planar.db.migrations;

namespace planar::db {

auto current_version(connection& conn) -> std::expected<std::uint32_t, db_error> {
  auto stmt = conn.prepare(std::format("select coalesce(max(version), 0) from {}", k_main_version_table));
  if (!stmt) {
    // The version table does not exist yet on a fresh database — treat
    // that as "nothing applied yet" rather than surfacing the prepare
    // failure, matching zig/src/db/migrate.zig's applyAll/
    // assertSchemaCompatible fallback.
    return std::uint32_t{0};
  }

  auto step = stmt->step();
  if (!step) {
    return std::unexpected(step.error());
  }
  if (*step != step_result::row) {
    return std::uint32_t{0};
  }
  return static_cast<std::uint32_t>(stmt->column_int64(0));
}

namespace {

/// @brief SQLite's `SQLITE_MISUSE`. Spelled here rather than included:
/// this module has no `<sqlite3.h>` in its global module fragment, and
/// migrate.t.cpp sets the precedent of naming the constant locally. It is
/// the right code for the one failure below — a malformed migration chain
/// is the CALLER misusing the runner, not the database failing.
constexpr int k_sqlite_misuse = 21;

} // namespace

auto embedded_max(std::span<migration_record const> chain) -> std::uint32_t {
  std::uint32_t highest = 0;
  for (auto const& record : chain) {
    highest = std::max(highest, record.version_);
  }
  return highest;
}

auto embedded_max() -> std::uint32_t {
  return embedded_max(migrations());
}

auto require_contiguous(std::span<migration_record const> chain) -> std::expected<void, db_error> {
  for (std::size_t i = 0; i < chain.size(); ++i) {
    auto const expected_version = static_cast<std::uint32_t>(i + 1);
    if (chain[i].version_ != expected_version) {
      return std::unexpected(db_error{
          .code_    = k_sqlite_misuse,
          .message_ = std::format("migration chain is not contiguous: position {} holds version {} where version {} was "
                                  "expected (chain must be 1..n with no holes)",
                                  i, chain[i].version_, expected_version),
      });
    }
  }
  return {};
}

auto assert_schema_compatible(connection& conn, std::span<migration_record const> chain)
    -> std::expected<schema_state, db_error> {
  schema_state state{.live_ = 0, .embedded_max_ = embedded_max(chain), .verdict_ = schema_compatibility::current};

  // One query answers both questions: `max` is the live version, and
  // (`count`, `min`) is what distinguishes a healthy 1..max from a set
  // with a hole in it. `version` is `integer primary key` (migration
  // 00001 of either stream), so versions are unique — which is what makes
  // `count == max && min == 1` equivalent to "exactly 1..max".
  auto stmt =
      conn.prepare(std::format("select coalesce(max(version), 0), count(*), coalesce(min(version), 0) from {}", k_main_version_table));
  if (!stmt) {
    // No version table: a fresh database. Version 0, and the
    // verdict falls out of the comparison below (`behind` whenever the
    // binary embeds anything at all) — see this function's declaration
    // for why that is not reported as a read failure.
    state.verdict_ = state.embedded_max_ > 0 ? schema_compatibility::behind : schema_compatibility::current;
    return state;
  }

  auto step = stmt->step();
  if (!step) {
    return std::unexpected(step.error());
  }
  if (*step != step_result::row) {
    state.verdict_ = state.embedded_max_ > 0 ? schema_compatibility::behind : schema_compatibility::current;
    return state;
  }

  state.live_         = static_cast<std::uint32_t>(stmt->column_int64(0));
  auto const applied  = static_cast<std::uint32_t>(stmt->column_int64(1));
  auto const lowest   = static_cast<std::uint32_t>(stmt->column_int64(2));
  auto const has_hole = state.live_ > 0 && (applied != state.live_ || lowest != 1);

  // ORDER IS THE POLICY, and it is deliberate (see `schema_compatibility`):
  // `ahead` first because a newer schema is the case the binary can say
  // the most about and the operator can act on immediately; `gap` next
  // because a hole is a more specific and more actionable finding than
  // "behind"; only then the ordinary behind/current split.
  //
  // AN EARLIER VERSION OF THIS COMMENT justified gap-over-behind as
  // "applying the pending tail on top of a broken base would compound it".
  // That mechanism is not realized anywhere, and saying so was misleading
  // in two directions (task 6695):
  //
  //  - There is no pending tail left to apply when the verdict is read.
  //    `context.cpp` runs `db::apply_all` BEFORE
  //    `db::assert_schema_compatible`, so the tail is already applied by the
  //    time a verdict exists.
  //  - This ordering is not what the binaries observe anyway. All three
  //    consumer binaries test `stored < maximum` (and `stored > maximum`)
  //    against the raw `live_` version and never consult `verdict_` at all,
  //    so at the binary level BEHIND outranks GAP -- the inverse of the
  //    precedence declared here.
  //
  // The ordering below is still the right one for a caller that DOES read
  // `verdict_`; it is just not load-bearing for the shipped binaries today.
  if (state.live_ > state.embedded_max_) {
    state.verdict_ = schema_compatibility::ahead;
  } else if (has_hole) {
    state.verdict_ = schema_compatibility::gap;
  } else if (state.live_ < state.embedded_max_) {
    state.verdict_ = schema_compatibility::behind;
  } else {
    state.verdict_ = schema_compatibility::current;
  }
  return state;
}

auto assert_schema_compatible(connection& conn) -> std::expected<schema_state, db_error> {
  return assert_schema_compatible(conn, migrations());
}

auto apply_all(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error> {
  auto current = current_version(conn);
  if (!current) {
    return std::unexpected(current.error());
  }

  for (const auto& m : chain) {
    if (m.version_ <= *current) {
      continue;
    }

    // `lock_mode::immediate` (M1 boundary-review finding R1): Planar is a
    // five-binary system that shares one SQLite file, and every binary
    // migrates the database at startup. A plain `begin;` takes no lock —
    // two concurrent starters could both read the same current_version,
    // both open a deferred transaction, and both start executing this
    // migration's DDL before either acquires the write lock, so the
    // loser would hit SQLITE_BUSY partway through the script rather than
    // cleanly at BEGIN. Requesting the write lock synchronously here
    // makes the loser fail at this `begin_transaction` call instead —
    // before any DDL runs — restoring the clean serialization point that
    // zig/src/db/migrate.zig:53's `begin immediate` already provides on
    // the Zig side.
    auto txn = conn.begin_transaction(lock_mode::immediate);
    if (!txn) {
      return std::unexpected(txn.error());
    }

    // `execute` runs sqlite3_exec over the whole (possibly multi-
    // statement) script; on failure `txn` goes out of scope without a
    // commit and rolls back automatically — the failing migration never
    // partially lands.
    auto exec = conn.execute(m.up_sql_);
    if (!exec) {
      return std::unexpected(exec.error());
    }

    auto commit = txn->commit();
    if (!commit) {
      return std::unexpected(commit.error());
    }
  }

  return {};
}

auto apply_all(connection& conn) -> std::expected<void, db_error> {
  // Contiguity is checked HERE and not in the span overload on purpose.
  // The invariant belongs to the EMBEDDED chain — the thing
  // `cmake/generate_migrations.cmake` produces from `migrations/` — and
  // the span overload is documented as a test seam that deliberately
  // injects partial and synthetic chains (a lone version-34 record, a
  // `subspan(0, i + 1)` prefix). Enforcing it there would reject the
  // seam's whole purpose while adding nothing: a caller that hands over
  // its own chain already knows what it built.
  return apply_contiguous(conn, migrations());
}

auto apply_contiguous(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error> {
  if (auto const contiguous = require_contiguous(chain); !contiguous) {
    return std::unexpected(contiguous.error());
  }
  return apply_all(conn, chain);
}

auto rollback_all(connection& conn, std::span<migration_record const> chain) -> std::expected<void, db_error> {
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    auto exec = conn.execute(it->down_sql_);
    if (!exec) {
      return std::unexpected(exec.error());
    }
  }
  return {};
}

auto rollback_all(connection& conn) -> std::expected<void, db_error> {
  return rollback_all(conn, migrations());
}

} // namespace planar::db
