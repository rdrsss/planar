/// @file check.cppm
/// @brief planar.core.check — a runtime invariant guard that SURVIVES the
/// release build.
///
/// `assert()` from `<cassert>` compiles to nothing under `NDEBUG`, and the
/// `release` preset is `RelWithDebInfo` (`-O2 -g -DNDEBUG`). Every ported
/// `assert()` is therefore absent from the binary operators actually run —
/// exactly where the guard is worth the most, because the invariants it
/// protects are the ones no test can reach (they only hold false when state
/// is ALREADY corrupt). Decision 1040, task 6347.
///
/// `check()` is deliberately narrow. It is NOT an error-handling mechanism:
/// an expected failure — bad input, a missing row, a network error — belongs
/// in `std::expected<T, E>` at the module boundary, per the project's error
/// doctrine. `check()` states something the program believes cannot be false,
/// and terminates if it is, because continuing past a broken invariant is
/// worse than stopping.
///
/// Scoped to first-party code on purpose: this does not touch `NDEBUG`, so
/// the vendored SQLite / Lua / curl / Glaze / spdlog builds keep their own
/// assertion posture untouched and unaudited-by-us.
///
/// No exceptions cross this boundary — a failed check calls `std::abort()`
/// after writing a diagnostic to stderr.

export module planar.core.check;

import std;

/// @brief Abort with a diagnostic naming the failed invariant.
///
/// Separated from `check()` so the hot path is a single untaken branch and
/// the cold path is never inlined into it.
///
/// @param expr Source text of the failed expression.
/// @param loc Call site of the originating `check()`.
[[noreturn]] void fail(std::string_view expr, const std::source_location& loc);

/// @brief Assert an invariant that must hold in EVERY build configuration.
///
/// Unlike `assert()`, this is not compiled out under `NDEBUG`. Use it where a
/// false condition means internal state is already corrupt and continuing
/// would produce silently wrong output. For an expected failure, return
/// `std::expected` instead.
///
/// @param ok The invariant. Aborts when false.
/// @param expr Source text of the expression, for the diagnostic.
/// @param loc Defaulted call site; do not pass explicitly.
export inline void check(bool ok, std::string_view expr, const std::source_location& loc = std::source_location::current()) {
  if (!ok) [[unlikely]] {
    fail(expr, loc);
  }
}
