/// @file queue_store.cpp
/// @brief Implementation of `planar.cmd.planar_agent.queue_store`. See
/// queue_store.cppm for the contract.
module planar.cmd.planar_agent.queue_store;

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.hostqueue;
import planar.cmd.planar_agent.context;

namespace planar::cmd::agent {

namespace {

namespace hq = engine::hostqueue;

/// @brief The longest a statement waits for a lock the queue's own writers
/// hold (the same bound every read-write connection gets).
constexpr int k_busy_ms = 5000;

/// @brief SQLite's primary result codes for a lock held past the bound.
constexpr int k_sqlite_busy   = 5;
constexpr int k_sqlite_locked = 6;

auto refuse(std::string_view tag, std::string message) -> std::unexpected<store_refusal> {
  return std::unexpected(store_refusal{.tag = std::string{tag}, .message = std::move(message)});
}

/// @brief `message` on one line: SQLite occasionally ends a message with one.
auto one_line(std::string message) -> std::string {
  std::ranges::replace(message, '\n', ' ');
  while (!message.empty() && message.back() == ' ') {
    message.pop_back();
  }
  return message;
}

auto is_lock(const db::db_error& error) -> bool {
  auto const primary = error.code_ & 0xff;
  return primary == k_sqlite_busy || primary == k_sqlite_locked;
}

/// @brief Whether `path` names an existing file, as a refusal when it cannot
/// be reached.
auto check_reachable(const std::filesystem::path& path) -> std::expected<void, store_refusal> {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return {};
  }
  if (!ec || ec == std::errc::no_such_file_or_directory) {
    return refuse(k_tag_store_unreachable,
                  std::format("the Planar database {} does not exist; run `planar init` to create it", path.string()));
  }
  return refuse(k_tag_store_unreachable,
                std::format("cannot reach the Planar database {}: {}; check PLANAR_DB and the path's permissions", path.string(),
                            ec.message()));
}

/// @brief Reads the first page, so a file that is not a database is told apart
/// from one that is behind: the version handshake reads a missing
/// `schema_migrations` table as version 0.
auto check_readable(db::connection& conn, const std::filesystem::path& path) -> std::expected<void, store_refusal> {
  auto const fail = [&](const db::db_error& error) -> std::expected<void, store_refusal> {
    if (is_lock(error)) {
      return refuse(k_tag_store_unreadable, std::format("cannot read {}: it stayed locked past {} ms ({}); try again",
                                                        path.string(), k_busy_ms, one_line(error.message_)));
    }
    return refuse(k_tag_store_unreachable,
                  std::format("{} is not a usable database ({}); check PLANAR_DB", path.string(), one_line(error.message_)));
  };
  auto stmt = conn.prepare("select count(*) from sqlite_master");
  if (!stmt) {
    return fail(stmt.error());
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return fail(stepped.error());
  }
  return {};
}

} // namespace

/// @brief Shares the existing queue handshake with optional observer deadline checks.
/// @param env The invocation environment that locates the database.
/// @param access Whether this opening needs read-only or read-write access.
/// @param budget An optional remaining-budget callback for observation.
/// @return The checked queue store or a tagged refusal.
auto open_queue_store_impl(const env_lookup& env, store_access access, const queue_store_budget& budget)
    -> std::expected<queue_store, store_refusal> {
  auto remaining = [&]() -> std::expected<int, store_refusal> {
    if (budget) {
      return budget();
    }
    return k_busy_ms;
  };
  auto allowance = remaining();
  if (!allowance) {
    return std::unexpected(std::move(allowance.error()));
  }
  auto const path = resolve_db_path(env);
  if (!path) {
    return refuse(k_tag_store_unreachable,
                  "neither PLANAR_DB nor HOME is set, so the Planar database cannot be located; set one of them");
  }
  if (auto reachable = check_reachable(*path); !reachable) {
    return std::unexpected(std::move(reachable.error()));
  }

  allowance = remaining();
  if (!allowance) {
    return std::unexpected(std::move(allowance.error()));
  }

  auto opened = access == store_access::read_only ? db::connection::open_read_only(path->string(), *allowance)
                                                  : db::connection::open_existing(path->string(), k_busy_ms);
  if (!opened) {
    return refuse(k_tag_store_unreachable, std::format("cannot open the Planar database {}: {}; check PLANAR_DB and the path's "
                                                       "permissions",
                                                       path->string(), one_line(opened.error().message_)));
  }
  db::connection& conn = *opened;
  if (budget) {
    auto budget_copy = budget;
    conn.set_busy_retry([budget_copy] {
      auto left = budget_copy();
      if (!left || *left <= 0) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{std::min(*left, 10)});
      return true;
    });
  }
  allowance = remaining();
  if (!allowance) {
    return std::unexpected(std::move(allowance.error()));
  }
  if (!budget) {
    conn.set_busy_timeout(*allowance);
  }
  if (auto readable = check_readable(conn, *path); !readable) {
    if (auto after = remaining(); !after) {
      return std::unexpected(std::move(after.error()));
    }
    return std::unexpected(std::move(readable.error()));
  }

  // The `schema_migrations` handshake comes first: a behind database is
  // refused before the queue's own marker is consulted, because the queue
  // tables may not exist yet and "foreign" would send the operator the wrong
  // way. The handshake's own diagnostics are not written (see
  // `database_policy::verify`, which does write them); what it found is folded
  // into the one refusal line below.
  allowance = remaining();
  if (!allowance) {
    return std::unexpected(std::move(allowance.error()));
  }
  if (!budget) {
    conn.set_busy_timeout(*allowance);
  }
  auto const state = db::assert_schema_compatible(conn);
  if (!state) {
    if (auto after = remaining(); !after) {
      return std::unexpected(std::move(after.error()));
    }
    return refuse(k_tag_store_unreadable, std::format("cannot read the schema version of {}: {}; try again", path->string(),
                                                      one_line(state.error().message_)));
  }
  auto const live     = state->live_;
  auto const embedded = state->embedded_max_;
  if (live < embedded) {
    return refuse(
        k_tag_schema_version_behind,
        std::format("schema version {} in {} is older than this binary's {}; run `planar init`", live, path->string(), embedded));
  }

  allowance = remaining();
  if (!allowance) {
    return std::unexpected(std::move(allowance.error()));
  }
  if (!budget) {
    conn.set_busy_timeout(*allowance);
  }
  if (auto checked = hq::check_queue_schema(conn); !checked) {
    if (auto after = remaining(); !after) {
      return std::unexpected(std::move(after.error()));
    }
    auto const& why = checked.error();
    if (why.kind == hq::queue_schema_failure::query_failed) {
      return refuse(k_tag_store_unreadable,
                    std::format("cannot read the queue tables of {}: {}; try again", path->string(), one_line(why.message)));
    }
    // `live >= embedded` here, so the tag is always present.
    auto const tag = hq::queue_schema_refusal_tag(live, embedded).value_or(hq::k_tag_queue_schema_incompatible);
    if (tag == hq::k_tag_queue_schema_foreign) {
      return refuse(tag, std::format("{} is at schema version {}, the same as this binary's, but its queue tables do not match: "
                                     "{}; this database's migration {} is not this binary's migration {} (two branches used "
                                     "the same number), so a newer build alone will not fix it; correct the database or the "
                                     "build",
                                     path->string(), live, one_line(why.message), live, live));
    }
    return refuse(tag, std::format("{} is at schema version {}, ahead of this binary's {}, and its queue tables are not usable "
                                   "by this binary: {}; install a newer build",
                                   path->string(), live, embedded, one_line(why.message)));
  }
  allowance = remaining();
  if (!allowance) {
    return std::unexpected(std::move(allowance.error()));
  }
  return queue_store{.conn = std::move(*opened), .path = *path};
}

auto open_queue_store(const env_lookup& env, store_access access) -> std::expected<queue_store, store_refusal> {
  return open_queue_store_impl(env, access, {});
}

auto open_queue_store_for_wait(const env_lookup& env, const queue_store_budget& budget)
    -> std::expected<queue_store, store_refusal> {
  return open_queue_store_impl(env, store_access::read_only, budget);
}

auto queue_log_directory(const std::filesystem::path& db_path) -> std::filesystem::path {
  auto const name = db_path.filename().string();
  auto const base = name == "planar.db" ? std::string{"queue-logs"} : std::format("{}.queue-logs", db_path.stem().string());
  return db_path.parent_path() / base;
}

} // namespace planar::cmd::agent
