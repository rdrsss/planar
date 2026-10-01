/// @file main.cpp
/// @brief `queue_contention_probe` — measures what the host queue's polling
/// costs the planning writers that now share its database (plan 1089, task
/// qp-contention-measure; tech spec 656 § Write contention, decision 1004).
///
/// The queue tables live in `planar.db`, so every live submitter's one short
/// `BEGIN IMMEDIATE` per poll interval competes with planning writes for the
/// single SQLite write lock. This tool runs the five measurement runs the
/// spec defines (R1 baseline, R2 fast poll, R3 long transaction, R4 heavy
/// migration, R5 checkpoint) against an operator-size database and emits one
/// JSON report per run. It never tunes anything: a breach is reported, and
/// the exit status says so.
///
/// Copy safety. The tool never opens an operator database read-write:
///  - the source is opened ONLY through `open_source_read_only`, a
///    `SQLITE_OPEN_READONLY` handle on a `mode=ro` URI, and is used only as
///    the origin of the online backup API;
///  - `vet_destination` runs before anything is opened and refuses a
///    destination that, after symlinks are resolved, is the source, lies
///    under `$HOME/.planar`, or equals `$PLANAR_DB`, or that already exists;
///  - `open_copy_read_write` is the single place a read-write handle is
///    opened, and it takes only a `vetted_copy`, a type `vet_destination`
///    alone produces;
///  - every worker, the rebuild, the `planar-watch feed --follow` child
///    (`PLANAR_DB` set to the copy) and the migration of the copy to the
///    head schema act on the copy.
///
/// The queue tables are reached only through the hostqueue engine's API, so
/// no SQL literal in this file names one (the separability test scans for
/// that).
///
/// Usage:
///   queue_contention_probe --source <db> --dest <new-copy.db> --run r1|r2|r3|r4|r5
///       [--duration-s N] [--submitters N] [--poll-ms N] [--slots N]
///       [--planning-interval-ms N] [--warmup-s N] [--holds a,b] [--gap-s N]
///       [--rebuild-table T] [--rebuild-at-s N] [--watch-bin PATH]
///       [--checkpoint-wait-s N] [--report FILE] [--keep]
///
/// Exit codes: 0 = run completed and every bound held (or the run is
/// informational), 1 = internal or environment error, 2 = usage error or a
/// refused source/destination, 3 = a bound was breached.

#include <csignal>
#include <cstdio>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.hostqueue;
import planar.json_text;
import planar.process;
import planar.process.identity;
import planar.process.runner;

namespace {

namespace fs = std::filesystem;
namespace hq = planar::engine::hostqueue;
using clk    = std::chrono::steady_clock;

constexpr int          k_exit_ok               = 0;
constexpr int          k_exit_error            = 1;
constexpr int          k_exit_refused          = 2;
constexpr int          k_exit_breached         = 3;
constexpr int          k_busy_ms               = 5000;      // The runtime's busy timeout (decision 1004).
constexpr std::int64_t k_stale_after           = 30'000;    // The default [queue] stale_after.
constexpr std::int64_t k_run_limit             = 3'600'000; // Long enough that no simulated command times out.
constexpr double       k_planning_p99_bound_ms = 250.0;
constexpr std::int64_t k_wal_bound_bytes       = 64LL * 1024 * 1024;
constexpr double       k_rebuild_report_only_s = 25.0;

// ---------------------------------------------------------------------------
// Options.
// ---------------------------------------------------------------------------

struct options {
  std::string               source;
  std::string               dest;
  std::string               run;
  std::string               report;
  std::string               watch_bin;
  std::string               rebuild_table;
  std::int64_t              duration_s           = -1;
  std::int64_t              submitters           = 16;
  std::int64_t              poll_ms              = -1;
  std::int64_t              slots                = -1;
  std::int64_t              planning_interval_ms = 50;
  std::int64_t              warmup_s             = 10;
  std::int64_t              gap_s                = 10;
  std::int64_t              rebuild_at_s         = 15;
  std::int64_t              checkpoint_wait_s    = 60;
  std::vector<std::int64_t> holds_s{10, 25};
  bool                      keep = false;
};

auto parse_int(std::string_view text) -> std::optional<std::int64_t> {
  std::int64_t value   = 0;
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

/// @brief Parses argv; returns an error message on a usage problem.
auto parse_options(std::span<char const* const> args) -> std::expected<options, std::string> {
  options opts;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view const key = args[i];
    if (key == "--keep") {
      opts.keep = true;
      continue;
    }
    if (i + 1 >= args.size()) {
      return std::unexpected(std::format("{} needs a value", key));
    }
    std::string_view const value  = args[++i];
    auto                   number = [&](std::int64_t& into) -> std::expected<void, std::string> {
      auto parsed = parse_int(value);
      if (!parsed || *parsed < 0) {
        return std::unexpected(std::format("{} needs a non-negative integer, got '{}'", key, value));
      }
      into = *parsed;
      return {};
    };
    std::expected<void, std::string> status;
    if (key == "--source") {
      opts.source = value;
    } else if (key == "--dest") {
      opts.dest = value;
    } else if (key == "--run") {
      opts.run = value;
    } else if (key == "--report") {
      opts.report = value;
    } else if (key == "--watch-bin") {
      opts.watch_bin = value;
    } else if (key == "--rebuild-table") {
      opts.rebuild_table = value;
    } else if (key == "--duration-s") {
      status = number(opts.duration_s);
    } else if (key == "--submitters") {
      status = number(opts.submitters);
    } else if (key == "--poll-ms") {
      status = number(opts.poll_ms);
    } else if (key == "--slots") {
      status = number(opts.slots);
    } else if (key == "--planning-interval-ms") {
      status = number(opts.planning_interval_ms);
    } else if (key == "--warmup-s") {
      status = number(opts.warmup_s);
    } else if (key == "--gap-s") {
      status = number(opts.gap_s);
    } else if (key == "--rebuild-at-s") {
      status = number(opts.rebuild_at_s);
    } else if (key == "--checkpoint-wait-s") {
      status = number(opts.checkpoint_wait_s);
    } else if (key == "--holds") {
      opts.holds_s.clear();
      std::string_view rest = value;
      while (!rest.empty()) {
        auto const comma  = rest.find(',');
        auto const piece  = rest.substr(0, comma);
        auto       parsed = parse_int(piece);
        if (!parsed || *parsed <= 0) {
          return std::unexpected(std::format("--holds needs positive integers separated by commas, got '{}'", value));
        }
        opts.holds_s.push_back(*parsed);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
      }
    } else {
      return std::unexpected(std::format("unknown argument {}", key));
    }
    if (!status) {
      return std::unexpected(status.error());
    }
  }
  if (opts.source.empty() || opts.dest.empty()) {
    return std::unexpected("--source and --dest are required");
  }
  if (opts.run != "r1" && opts.run != "r2" && opts.run != "r3" && opts.run != "r4" && opts.run != "r5") {
    return std::unexpected("--run must be one of r1, r2, r3, r4, r5");
  }
  if (opts.submitters < 1) {
    return std::unexpected("--submitters must be at least 1");
  }
  // Per-run defaults, from the spec's table.
  if (opts.poll_ms < 0) {
    opts.poll_ms = opts.run == "r2" ? 100 : 1000;
  }
  if (opts.poll_ms < 1) {
    return std::unexpected("--poll-ms must be at least 1");
  }
  if (opts.duration_s < 0) {
    opts.duration_s = opts.run == "r5" ? 600 : (opts.run == "r3" ? 80 : 60);
  }
  if (opts.run == "r5" && opts.watch_bin.empty()) {
    return std::unexpected("--run r5 needs --watch-bin (the planar-watch binary, run as `<bin> feed --follow`)");
  }
  return opts;
}

// ---------------------------------------------------------------------------
// Copy safety.
// ---------------------------------------------------------------------------

/// @brief A destination `vet_destination` accepted. The only value
/// `open_copy_read_write` takes, so no read-write handle can be opened on a
/// path that did not pass the checks.
class vetted_copy {
private:
  fs::path _path;
  explicit vetted_copy(fs::path path) : _path(std::move(path)) {
  }
  friend auto vet_destination(std::string_view, std::string_view) -> std::expected<vetted_copy, std::string>;

public:
  [[nodiscard]] auto path() const -> const fs::path& {
    return _path;
  }
};

auto is_under(const fs::path& path, const fs::path& dir) -> bool {
  auto const rel = path.lexically_relative(dir);
  return !rel.empty() && *rel.begin() != "..";
}

auto env_text(std::string_view name) -> std::optional<std::string> {
  auto const* value = std::getenv(std::string(name).c_str());
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

/// @brief Decides whether `dest` may receive a copy of `source`. Opens
/// nothing. Refuses when the source is not an existing file, when, after
/// resolving symlinks in both, the destination is the source, lies under
/// `$HOME/.planar`, or equals `$PLANAR_DB`, and when the destination already
/// exists (as a file, directory or symlink).
auto vet_destination(std::string_view source, std::string_view dest) -> std::expected<vetted_copy, std::string> {
  std::error_code ec;
  auto const      source_real = fs::canonical(fs::path(source), ec);
  if (ec || !fs::is_regular_file(source_real, ec)) {
    return std::unexpected(std::format("source '{}' is not an existing file", source));
  }
  fs::path const dest_path(dest);
  auto const     dest_real = fs::weakly_canonical(dest_path, ec);
  if (ec) {
    return std::unexpected(std::format("destination '{}' cannot be resolved: {}", dest, ec.message()));
  }
  if (dest_real == source_real) {
    return std::unexpected(std::format("destination '{}' resolves to the source '{}'", dest, source_real.string()));
  }
  if (auto const home = env_text("HOME"); home) {
    auto const planar_home = fs::weakly_canonical(fs::path(*home) / ".planar", ec);
    if (!ec && is_under(dest_real, planar_home)) {
      return std::unexpected(std::format("destination '{}' is under {}", dest_real.string(), planar_home.string()));
    }
  }
  if (auto const live = env_text("PLANAR_DB"); live) {
    auto const live_real = fs::weakly_canonical(fs::path(*live), ec);
    if (!ec && dest_real == live_real) {
      return std::unexpected(std::format("destination '{}' equals $PLANAR_DB", dest_real.string()));
    }
  }
  // Last, so a destination that IS the source or the live database is named as
  // such rather than as merely existing.
  if (fs::symlink_status(dest_path, ec).type() != fs::file_type::not_found) {
    return std::unexpected(std::format("destination '{}' already exists; a copy is always a new file", dest));
  }
  return vetted_copy(dest_real);
}

auto percent_encode(std::string_view path) -> std::string {
  std::string out;
  for (char const c : path) {
    auto const u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0 || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
      out += c;
    } else {
      out += std::format("%{:02X}", static_cast<unsigned>(u));
    }
  }
  return out;
}

/// @brief Takes the online backup of `source` into the vetted destination.
/// The source handle is `SQLITE_OPEN_READONLY` on a `mode=ro` URI and is
/// used for nothing but the backup's read side.
auto backup_source_to_copy(std::string_view source, const vetted_copy& copy) -> std::expected<void, std::string> {
  fs::create_directories(copy.path().parent_path());
  sqlite3*          src = nullptr;
  std::string const uri = std::format("file:{}?mode=ro", percent_encode(source));
  if (int const rc = sqlite3_open_v2(uri.c_str(), &src, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr); rc != SQLITE_OK) {
    std::string const message = src != nullptr ? sqlite3_errmsg(src) : "out of memory";
    sqlite3_close_v2(src);
    return std::unexpected(std::format("cannot open the source read-only: {}", message));
  }
  // Both layers must hold: a handle that SQLite reports as anything but
  // read-only is refused, because a read-write flag on a file the process may
  // not write silently degrades to read-only, and one it may write would not.
  if (sqlite3_db_readonly(src, "main") != 1) {
    sqlite3_close_v2(src);
    return std::unexpected("the source handle is not read-only; refusing to use it");
  }
  sqlite3_busy_timeout(src, k_busy_ms);
  // The destination handle: the one raw read-write open in this tool, and it
  // is on the vetted copy.
  sqlite3*          dst  = nullptr;
  std::string const dest = copy.path().string();
  if (int const rc = sqlite3_open_v2(dest.c_str(), &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr); rc != SQLITE_OK) {
    std::string const message = dst != nullptr ? sqlite3_errmsg(dst) : "out of memory";
    sqlite3_close_v2(dst);
    sqlite3_close_v2(src);
    return std::unexpected(std::format("cannot create the copy: {}", message));
  }
  std::expected<void, std::string> result;
  if (sqlite3_backup* backup = sqlite3_backup_init(dst, "main", src, "main"); backup == nullptr) {
    result = std::unexpected(std::format("backup init failed: {}", sqlite3_errmsg(dst)));
  } else {
    int rc = SQLITE_OK;
    for (int attempt = 0; attempt < 50; ++attempt) {
      rc = sqlite3_backup_step(backup, -1);
      if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
        break;
      }
      sqlite3_sleep(100);
    }
    sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE) {
      result = std::unexpected(std::format("backup failed with result code {}", rc));
    }
  }
  sqlite3_close_v2(dst);
  sqlite3_close_v2(src);
  return result;
}

/// @brief The one read-write open of a database in this tool, on a vetted copy.
auto open_copy_read_write(const vetted_copy& copy) -> std::expected<planar::db::connection, planar::db::db_error> {
  return planar::db::connection::open(copy.path().string());
}

// ---------------------------------------------------------------------------
// Statistics and JSON.
// ---------------------------------------------------------------------------

auto millis(clk::time_point from, clk::time_point to) -> double {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

auto seconds(clk::time_point from, clk::time_point to) -> double {
  return std::chrono::duration<double>(to - from).count();
}

struct series_summary {
  std::size_t count = 0;
  double      mean  = 0;
  double      p50   = 0;
  double      p90   = 0;
  double      p99   = 0;
  double      max   = 0;
};

auto summarize(std::vector<double> samples) -> series_summary {
  series_summary out;
  out.count = samples.size();
  if (samples.empty()) {
    return out;
  }
  std::ranges::sort(samples);
  auto rank = [&](double p) {
    auto const index = static_cast<std::size_t>(std::ceil(p * static_cast<double>(samples.size()))) - 1;
    return samples[std::min(index, samples.size() - 1)];
  };
  out.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
  out.p50  = rank(0.50);
  out.p90  = rank(0.90);
  out.p99  = rank(0.99);
  out.max  = samples.back();
  return out;
}

auto summary_json(const series_summary& s) -> std::string {
  using planar::json_text::json_double;
  return std::format(R"({{"n":{},"mean_ms":{},"p50_ms":{},"p90_ms":{},"p99_ms":{},"max_ms":{}}})", s.count, json_double(s.mean),
                     json_double(s.p50), json_double(s.p90), json_double(s.p99), json_double(s.max));
}

auto quote(std::string_view text) -> std::string {
  return planar::json_text::json_string(text);
}

// ---------------------------------------------------------------------------
// Shared state and workers.
// ---------------------------------------------------------------------------

struct shared_state {
  std::string       copy_path;
  std::int64_t      slots          = 1;
  std::int64_t      poll_ms        = 1000;
  std::int64_t      stale_after_ms = k_stale_after;
  std::int64_t      run_limit_ms   = k_run_limit;
  std::string       host_id;
  std::int64_t      pid         = 0;
  std::int64_t      pid_started = 0;
  clk::time_point   origin      = clk::now();
  std::atomic<bool> stop{false};
};

void nap(const shared_state& sh, std::int64_t ms) {
  auto const until = clk::now() + std::chrono::milliseconds(ms);
  while (!sh.stop.load() && clk::now() < until) {
    auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(until - clk::now());
    std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds(20)));
  }
}

enum class poll_kind : std::uint8_t { completed, skipped, busy_error, error };

struct poll_record {
  double    start_s = 0; // Seconds since the run began.
  double    end_s   = 0;
  poll_kind kind    = poll_kind::completed;
};

struct submitter_result {
  int                      index = 0;
  bool                     late  = false;
  std::int64_t             seq   = 0;
  std::vector<double>      enqueue_ms;
  std::vector<double>      poll_ms;
  std::vector<poll_record> polls;
  std::int64_t             enqueue_busy  = 0;
  std::int64_t             entry_missing = 0;
  bool                     gave_up       = false;
  bool                     ever_running  = false;
  std::string              fatal;
  std::string              first_error;
};

auto is_busy_code(int code) -> bool {
  return (code & 0xff) == 5;
}

void run_submitter(shared_state& sh, int index, std::int64_t start_delay_ms, bool late, submitter_result& out) {
  out.index = index;
  out.late  = late;
  nap(sh, start_delay_ms);
  if (sh.stop.load()) {
    return;
  }
  auto opened = planar::db::connection::open_existing(sh.copy_path, k_busy_ms);
  if (!opened) {
    out.fatal = opened.error().message_;
    return;
  }
  auto&                                   conn = *opened;
  planar::process::identity::system_clock clock;
  auto const                              probe = hq::system_process_probe();

  hq::enqueue_request request;
  request.host_id     = sh.host_id;
  request.pid         = sh.pid;
  request.pid_started = sh.pid_started;
  request.cwd         = "/";
  request.argv        = {"true"};
  request.label       = std::format("contention-probe-{}", index);
  std::optional<clk::time_point> busy_since;
  while (!sh.stop.load()) {
    request.enqueued_at    = clock.wall_ms();
    request.refreshed_mono = clock.monotonic_ms().value_or(0);
    auto const t0          = clk::now();
    auto const inserted    = hq::enqueue(conn, request);
    auto const t1          = clk::now();
    out.enqueue_ms.push_back(millis(t0, t1));
    if (inserted) {
      out.seq = *inserted;
      break;
    }
    if (!is_busy_code(inserted.error().sqlite_code)) {
      out.fatal = inserted.error().message;
      return;
    }
    ++out.enqueue_busy;
    if (!busy_since) {
      busy_since = t1;
    } else if (millis(*busy_since, t1) > static_cast<double>(sh.stale_after_ms)) {
      out.gave_up = true;
      return;
    }
    nap(sh, sh.poll_ms);
  }
  if (out.seq == 0) {
    return;
  }

  hq::poll_request const poll_request{.seq            = out.seq,
                                      .host_id        = sh.host_id,
                                      .slots          = sh.slots,
                                      .stale_after_ms = sh.stale_after_ms,
                                      .run_limit_ms   = sh.run_limit_ms};
  while (!sh.stop.load()) {
    auto const  t0     = clk::now();
    auto        polled = hq::poll(conn, poll_request, clock, probe);
    auto const  t1     = clk::now();
    poll_record rec{.start_s = seconds(sh.origin, t0), .end_s = seconds(sh.origin, t1), .kind = poll_kind::completed};
    out.poll_ms.push_back(millis(t0, t1));
    bool missing = false;
    if (!polled) {
      rec.kind = is_busy_code(polled.error().sqlite_code) ? poll_kind::busy_error : poll_kind::error;
      if (out.first_error.empty()) {
        out.first_error = polled.error().message;
      }
    } else if (polled->status == hq::poll_status::skipped) {
      rec.kind = poll_kind::skipped;
    } else {
      if (polled->entry_missing) {
        ++out.entry_missing;
        missing = true;
      }
      if (polled->running) {
        out.ever_running = true;
      }
    }
    out.polls.push_back(rec);
    if (missing) {
      return;
    }
    nap(sh, sh.poll_ms);
  }
}

struct planning_result {
  std::vector<double> acquire_ms;
  std::vector<double> work_ms;
  std::int64_t        busy_failures  = 0;
  std::int64_t        other_failures = 0;
  std::string         first_error;
};

/// @brief The planning-write shape the spec names: a task insert and update
/// and an artifact update in one `BEGIN IMMEDIATE` transaction, on a row set
/// read from the copy.
void run_planning(shared_state& sh, std::int64_t interval_ms, planning_result& out) {
  auto opened = planar::db::connection::open_existing(sh.copy_path, k_busy_ms);
  if (!opened) {
    out.first_error = opened.error().message_;
    ++out.other_failures;
    return;
  }
  auto&        conn        = *opened;
  std::int64_t plan_id     = 0;
  std::int64_t scope_id    = 0;
  std::int64_t artifact_id = 0;
  if (auto stmt =
          conn.prepare("select plan_id, scope_id from tasks where plan_id is not null and scope_kind = 'association' limit 1");
      stmt && stmt->step() && stmt->column_int64(0) != 0) {
    plan_id  = stmt->column_int64(0);
    scope_id = stmt->column_int64(1);
  }
  if (auto stmt = conn.prepare("select id from artifacts limit 1"); stmt && stmt->step()) {
    artifact_id = stmt->column_int64(0);
  }
  std::int64_t counter = 0;
  while (!sh.stop.load()) {
    ++counter;
    // The transaction lives in its own scope: a failed one rolls back before
    // the nap, so the planning writer never holds the lock between attempts.
    {
      auto const t0 = clk::now();
      auto       tx = conn.begin_transaction(planar::db::lock_mode::immediate);
      auto const t1 = clk::now();
      out.acquire_ms.push_back(millis(t0, t1));
      if (!tx) {
        if (planar::db::is_busy(tx.error())) {
          ++out.busy_failures;
        } else {
          ++out.other_failures;
        }
        if (out.first_error.empty()) {
          out.first_error = tx.error().message_;
        }
      } else {
        bool ok   = true;
        auto fail = [&](const planar::db::db_error& error) {
          ok = false;
          ++out.other_failures;
          if (out.first_error.empty()) {
            out.first_error = error.message_;
          }
        };
        std::string const scope = scope_id == 0 ? "'global', null" : std::format("'association', {}", scope_id);
        if (auto r = conn.execute(std::format("insert into tasks (scope_kind, scope_id, plan_id, title, status) values "
                                              "({}, {}, 'contention probe {}', 'todo')",
                                              scope, plan_id == 0 ? "null" : std::to_string(plan_id), counter));
            !r) {
          fail(r.error());
        }
        if (ok) {
          if (auto r = conn.execute("update tasks set priority = priority + 1, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                                    "where id = last_insert_rowid()");
              !r) {
            fail(r.error());
          }
        }
        if (ok && artifact_id != 0) {
          if (auto r = conn.execute(std::format(
                  "update artifacts set updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = {}", artifact_id));
              !r) {
            fail(r.error());
          }
        }
        if (ok) {
          if (auto c = tx->commit(); !c) {
            fail(c.error());
          }
        }
        out.work_ms.push_back(millis(t1, clk::now()));
      }
    }
    nap(sh, interval_ms);
  }
}

struct hold_record {
  double      requested_s = 0;
  double      start_s     = 0; // When the lock was acquired.
  double      end_s       = 0; // When it was released.
  bool        done        = false;
  std::string error;
};

/// @brief Holds a planning write transaction for `hold_s` seconds, beginning
/// at `at_s` seconds into the run.
void run_hold(shared_state& sh, double at_s, double hold_s, hold_record& out) {
  out.requested_s     = hold_s;
  auto const begin_at = sh.origin + std::chrono::milliseconds(static_cast<std::int64_t>(at_s * 1000));
  while (!sh.stop.load() && clk::now() < begin_at) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (sh.stop.load()) {
    return;
  }
  auto opened = planar::db::connection::open_existing(sh.copy_path, k_busy_ms);
  if (!opened) {
    out.error = opened.error().message_;
    return;
  }
  auto tx = opened->begin_transaction(planar::db::lock_mode::immediate);
  if (!tx) {
    out.error = tx.error().message_;
    return;
  }
  if (auto r =
          opened->execute("insert into tasks (scope_kind, title, status) values ('global', 'contention probe hold', 'todo')");
      !r) {
    out.error = r.error().message_;
    return;
  }
  auto const held_from = clk::now();
  out.start_s          = seconds(sh.origin, held_from);
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<std::int64_t>(hold_s * 1000)));
  if (auto c = tx->commit(); !c) {
    out.error = c.error().message_;
    return;
  }
  out.end_s = seconds(sh.origin, clk::now());
  out.done  = true;
}

struct rebuild_result {
  std::atomic<int>                            phase{0}; // 0 pending, 1 running, 2 finished (set by the thread wrapper).
  std::string                                 table;
  bool                                        started         = false;
  bool                                        done            = false;
  double                                      at_s            = 0;
  double                                      lock_acquire_ms = 0;
  double                                      total_ms        = 0; // Lock held from begin to commit, ms.
  std::vector<std::pair<std::string, double>> steps;
  std::string                                 error;
};

auto find_ci(std::string_view text, std::string_view needle) -> std::size_t {
  auto const it = std::ranges::search(text, needle, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  });
  return it.empty() ? std::string_view::npos : static_cast<std::size_t>(it.begin() - text.begin());
}

auto ident(std::string_view name) -> std::string {
  return std::format("\"{}\"", name);
}

/// @brief The table-rebuild shape of a real migration, in one write
/// transaction on `table`: create the replacement from the table's own
/// definition, copy every row, drop the original, rename the replacement,
/// recreate the original's indexes and triggers.
void run_rebuild(shared_state& sh, std::string_view table, double at_s, rebuild_result& out) {
  out.table           = table;
  out.at_s            = at_s;
  auto const begin_at = sh.origin + std::chrono::milliseconds(static_cast<std::int64_t>(at_s * 1000));
  while (!sh.stop.load() && clk::now() < begin_at) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (sh.stop.load()) {
    return;
  }
  auto opened = planar::db::connection::open_existing(sh.copy_path, k_busy_ms);
  if (!opened) {
    out.error = opened.error().message_;
    return;
  }
  auto& conn = *opened;
  // Foreign keys off before the transaction, as SQLite's table-rebuild recipe
  // prescribes: dropping a parent table must not cascade.
  // `legacy_alter_table` keeps the rename from re-validating triggers on OTHER
  // tables that mention this one, as a migration that drops them first would.
  if (auto r = conn.execute("pragma foreign_keys = off"); !r) {
    out.error = r.error().message_;
    return;
  }
  if (auto r = conn.execute("pragma legacy_alter_table = on"); !r) {
    out.error = r.error().message_;
    return;
  }
  std::string              create_sql;
  std::vector<std::string> dependents;
  {
    auto stmt = conn.prepare("select sql from sqlite_master where type = 'table' and name = ?1");
    if (!stmt || !stmt->bind_text(1, table) || !stmt->step() || stmt->is_null(0)) {
      out.error = std::format("table '{}' not found in the copy", table);
      return;
    }
    create_sql = stmt->column_text(0);
  }
  {
    auto stmt =
        conn.prepare("select sql from sqlite_master where tbl_name = ?1 and type in ('index', 'trigger') and sql is not null");
    if (!stmt || !stmt->bind_text(1, table)) {
      out.error = "cannot read the table's indexes";
      return;
    }
    while (true) {
      auto row = stmt->step();
      if (!row || *row == planar::db::step_result::done) {
        break;
      }
      dependents.push_back(stmt->column_text(0));
    }
  }
  auto const header = find_ci(create_sql, "create table");
  if (header == std::string::npos) {
    out.error = "unrecognized table definition";
    return;
  }
  auto name_pos = header + std::string_view("create table").size();
  while (name_pos < create_sql.size() && std::isspace(static_cast<unsigned char>(create_sql[name_pos])) != 0) {
    ++name_pos;
  }
  if (name_pos < create_sql.size() &&
      (create_sql[name_pos] == '"' || create_sql[name_pos] == '`' || create_sql[name_pos] == '[')) {
    ++name_pos;
  }
  if (create_sql.compare(name_pos, table.size(), table) != 0) {
    out.error = "the table name does not follow CREATE TABLE as expected";
    return;
  }
  std::string const staging    = std::format("{}__rebuild", table);
  std::string       staged_sql = create_sql;
  staged_sql.replace(name_pos, table.size(), staging);

  out.started         = true;
  out.phase           = 1;
  auto const t0       = clk::now();
  auto       tx       = conn.begin_transaction(planar::db::lock_mode::immediate);
  auto const t1       = clk::now();
  out.lock_acquire_ms = millis(t0, t1);
  if (!tx) {
    out.error = tx.error().message_;
    return;
  }
  auto step = [&](std::string_view label, const std::string& sql) -> bool {
    auto const s0 = clk::now();
    auto       r  = conn.execute(sql);
    out.steps.emplace_back(std::string(label), millis(s0, clk::now()));
    if (!r) {
      out.error = std::format("{}: {}", label, r.error().message_);
      return false;
    }
    return true;
  };
  bool ok = step("create", staged_sql) &&
            step("copy", std::format("insert into {} select * from {}", ident(staging), ident(table))) &&
            step("drop", std::format("drop table {}", ident(table))) &&
            step("rename", std::format("alter table {} rename to {}", ident(staging), ident(table)));
  for (std::size_t i = 0; ok && i < dependents.size(); ++i) {
    ok = step("recreate", dependents[i]);
  }
  if (ok) {
    if (auto c = tx->commit(); !c) {
      out.error = std::format("commit: {}", c.error().message_);
      ok        = false;
    }
  }
  out.total_ms = millis(t1, clk::now());
  out.done     = ok;
}

void run_rebuild_thread(shared_state& sh, std::string_view table, double at_s, rebuild_result& out) {
  run_rebuild(sh, table, at_s, out);
  out.phase = 2;
}

// ---------------------------------------------------------------------------
// Process helpers.
// ---------------------------------------------------------------------------

auto file_sha256(const fs::path& path) -> std::string {
  std::array<std::string_view, 2> const args{"-a", "256"};
  std::vector<std::string_view>         argv(args.begin(), args.end());
  std::string const                     text = path.string();
  argv.push_back(text);
  auto const result = planar::process::capture("shasum", argv);
  if (!result.spawned || result.exit_code != 0 || result.output.size() < 64) {
    return "unavailable";
  }
  return result.output.substr(0, 64);
}

struct file_stat {
  std::int64_t size     = -1;
  std::int64_t mtime_ns = -1;
};

auto stat_of(const fs::path& path) -> file_stat {
  struct stat info{};
  if (::stat(path.c_str(), &info) != 0) {
    return {};
  }
#if defined(__APPLE__)
  auto const ns = static_cast<std::int64_t>(info.st_mtimespec.tv_sec) * 1'000'000'000 + info.st_mtimespec.tv_nsec;
#else
  auto const ns = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1'000'000'000 + info.st_mtim.tv_nsec;
#endif
  return {.size = static_cast<std::int64_t>(info.st_size), .mtime_ns = ns};
}

auto file_size_or_zero(const fs::path& path) -> std::int64_t {
  std::error_code ec;
  auto const      size = fs::file_size(path, ec);
  return ec ? 0 : static_cast<std::int64_t>(size);
}

struct checkpoint_result {
  bool         ok           = false;
  std::int64_t busy         = -1;
  std::int64_t log          = -1;
  std::int64_t checkpointed = -1;
};

auto passive_checkpoint(planar::db::connection& conn) -> checkpoint_result {
  checkpoint_result out;
  auto              stmt = conn.prepare("pragma wal_checkpoint(PASSIVE)");
  if (!stmt) {
    return out;
  }
  auto row = stmt->step();
  if (!row || *row != planar::db::step_result::row) {
    return out;
  }
  out.ok           = true;
  out.busy         = stmt->column_int64(0);
  out.log          = stmt->column_int64(1);
  out.checkpointed = stmt->column_int64(2);
  return out;
}

auto checkpoint_json(const checkpoint_result& c) -> std::string {
  return std::format(R"({{"ok":{},"busy":{},"log":{},"checkpointed":{}}})", c.ok, c.busy, c.log, c.checkpointed);
}

// ---------------------------------------------------------------------------
// The experiment.
// ---------------------------------------------------------------------------

struct check {
  std::string name;
  std::string bound;
  std::string observed;
  bool        pass = false;
};

auto checks_json(const std::vector<check>& checks) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < checks.size(); ++i) {
    out += std::format(R"({}{{"name":{},"bound":{},"observed":{},"pass":{}}})", i == 0 ? "" : ",", quote(checks[i].name),
                       quote(checks[i].bound), quote(checks[i].observed), checks[i].pass);
  }
  return out + "]";
}

/// @brief Runs the experiment on the vetted copy and returns the report JSON
/// and whether every bound held.
auto run_experiment(const options& opts, const vetted_copy& copy, const std::function<std::string()>& source_report)
    -> std::expected<std::pair<std::string, bool>, std::string> {
  shared_state sh;
  sh.copy_path = copy.path().string();
  sh.poll_ms   = opts.poll_ms;
  sh.pid       = static_cast<std::int64_t>(::getpid());
  sh.host_id   = planar::process::identity::host_identity(planar::process::identity::native_identity_source());
  if (auto started = planar::process::identity::process_start_time(sh.pid); started && *started) {
    sh.pid_started = static_cast<std::int64_t>(**started);
  }
  bool const is_r3 = opts.run == "r3";
  bool const is_r4 = opts.run == "r4";
  bool const is_r5 = opts.run == "r5";
  // Late submitters (R3): two arrive during each hold, to prove a command
  // submitted into a held lock still runs once it is released.
  std::int64_t const late_per_hold = is_r3 ? 2 : 0;
  std::int64_t const late_total    = late_per_hold * static_cast<std::int64_t>(opts.holds_s.size());
  sh.slots                         = opts.slots >= 0 ? opts.slots : (is_r3 ? opts.submitters + late_total : 1);

  // The copy is migrated to the head schema (the live database may be behind
  // the queue migration) and any queue state it carries is discarded, through
  // the hostqueue API only.
  std::uint32_t version_before = 0;
  std::uint32_t version_after  = 0;
  {
    auto conn = open_copy_read_write(copy);
    if (!conn) {
      return std::unexpected(std::format("cannot open the copy: {}", conn.error().message_));
    }
    if (auto v = planar::db::current_version(*conn); v) {
      version_before = *v;
    }
    if (auto applied = planar::db::apply_all(*conn); !applied) {
      return std::unexpected(std::format("cannot migrate the copy to head: {}", applied.error().message_));
    }
    if (auto v = planar::db::current_version(*conn); v) {
      version_after = *v;
    }
    auto leftovers = hq::list(*conn);
    if (!leftovers) {
      return std::unexpected(std::format("cannot read the copy's queue: {}", leftovers.error().message));
    }
    for (auto const& e : *leftovers) {
      static_cast<void>(hq::discard_entry(*conn, e.seq));
    }
  }

  // Spawn the reader before any thread exists (the runner forks).
  std::optional<planar::process::runner::child> watcher;
  if (is_r5) {
    std::vector<std::string> const argv{"/bin/sh", "-c", "exec \"$0\" feed --follow >/dev/null 2>&1", opts.watch_bin};
    auto const                     env   = [](std::string_view name) -> std::optional<std::string> { return env_text(name); };
    auto                           child = planar::process::runner::start(
        env, argv, {.working_directory = std::nullopt, .env_name = "PLANAR_DB", .env_value = sh.copy_path});
    if (!child) {
      return std::unexpected("cannot start the feed --follow reader");
    }
    watcher = *child;
  }

  sh.origin                                   = clk::now();
  auto const                    total_workers = static_cast<std::size_t>(opts.submitters + late_total);
  std::vector<submitter_result> submitters(total_workers);
  std::vector<std::thread>      threads;
  std::mt19937_64               rng(0x9e3779b97f4a7c15ULL);
  for (std::int64_t i = 0; i < opts.submitters; ++i) {
    auto const phase = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(opts.poll_ms));
    threads.emplace_back(run_submitter, std::ref(sh), static_cast<int>(i), phase, false,
                         std::ref(submitters[static_cast<std::size_t>(i)]));
  }
  planning_result planning;
  threads.emplace_back(run_planning, std::ref(sh), opts.planning_interval_ms, std::ref(planning));

  std::vector<hold_record> holds(is_r3 ? opts.holds_s.size() : 0);
  double                   cursor_s   = static_cast<double>(opts.warmup_s);
  std::size_t              late_index = static_cast<std::size_t>(opts.submitters);
  for (std::size_t h = 0; h < holds.size(); ++h) {
    auto const hold_s = static_cast<double>(opts.holds_s[h]);
    threads.emplace_back(run_hold, std::ref(sh), cursor_s, hold_s, std::ref(holds[h]));
    for (std::int64_t k = 0; k < late_per_hold; ++k) {
      auto const delay_ms = static_cast<std::int64_t>((cursor_s + 1.0 + static_cast<double>(k)) * 1000);
      threads.emplace_back(run_submitter, std::ref(sh), static_cast<int>(late_index), delay_ms, true,
                           std::ref(submitters[late_index]));
      ++late_index;
    }
    cursor_s += hold_s + static_cast<double>(opts.gap_s);
  }
  rebuild_result rebuild;
  if (is_r4) {
    if (opts.rebuild_table.empty()) {
      sh.stop = true;
      for (auto& t : threads) {
        t.join();
      }
      return std::unexpected("--run r4 needs --rebuild-table (the largest planning table; the vendored SQLite has no dbstat, so "
                             "find it on the backup copy with the system sqlite3's dbstat)");
    }
    threads.emplace_back(run_rebuild_thread, std::ref(sh), std::string_view(opts.rebuild_table),
                         static_cast<double>(opts.rebuild_at_s), std::ref(rebuild));
  }

  // Main thread: sample the WAL once a second until the run's end.
  fs::path const            wal_path = copy.path().string() + "-wal";
  std::int64_t              wal_max  = 0;
  std::vector<std::int64_t> wal_samples;
  auto                      sample_wal = [&] {
    auto const size = file_size_or_zero(wal_path);
    wal_samples.push_back(size);
    wal_max = std::max(wal_max, size);
  };
  auto const run_end = sh.origin + std::chrono::seconds(opts.duration_s);
  while (clk::now() < run_end || (is_r4 && rebuild.phase.load() != 2 && clk::now() < run_end + std::chrono::seconds(600))) {
    sample_wal();
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (is_r4) {
    // Ten seconds of load after the rebuild commits, so a waiter's recovery is observed.
    auto const tail_until = clk::now() + std::chrono::seconds(10);
    while (clk::now() < tail_until) {
      sample_wal();
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  // R5: stop the reader, then watch for a PASSIVE checkpoint that completes.
  bool                  reader_alive_at_stop = true;
  std::optional<double> reader_exit_s;
  std::optional<double> checkpoint_complete_after_s;
  checkpoint_result     last_checkpoint;
  if (is_r5 && watcher) {
    // A reader that died early would make the run vacuous, so whether it was
    // still running when asked to stop is part of the verdict.
    if (auto alive = planar::process::runner::poll(*watcher); !alive || alive->kind != planar::process::runner::state::running) {
      reader_alive_at_stop = false;
    }
    static_cast<void>(planar::process::runner::signal(*watcher, SIGINT));
    bool reaped = false;
    for (int i = 0; i < 100 && !reaped; ++i) {
      auto status = planar::process::runner::poll(*watcher);
      reaped      = !status || status->kind != planar::process::runner::state::running;
      if (!reaped) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    if (!reaped) {
      static_cast<void>(planar::process::runner::signal(*watcher, SIGKILL));
      static_cast<void>(planar::process::runner::poll(*watcher));
    }
    auto const exited = clk::now();
    reader_exit_s     = seconds(sh.origin, exited);
    auto monitor      = open_copy_read_write(copy);
    if (monitor) {
      auto const limit = exited + std::chrono::seconds(opts.checkpoint_wait_s);
      while (true) {
        sample_wal();
        last_checkpoint = passive_checkpoint(*monitor);
        if (last_checkpoint.ok && last_checkpoint.log == last_checkpoint.checkpointed) {
          checkpoint_complete_after_s = seconds(exited, clk::now());
          break;
        }
        if (clk::now() >= limit) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  }

  sh.stop = true;
  for (auto& t : threads) {
    t.join();
  }
  double const run_seconds = seconds(sh.origin, clk::now());

  // Account: nobody ended an entry during the run, so every history row for a
  // worker's entry is a reap by another poll, and a poll that found its own
  // entry missing says the same.
  std::int64_t      abandoned    = 0;
  std::int64_t      entries_left = 0;
  checkpoint_result final_checkpoint;
  {
    auto conn = open_copy_read_write(copy);
    if (!conn) {
      return std::unexpected(std::format("cannot reopen the copy: {}", conn.error().message_));
    }
    std::set<std::int64_t> seqs;
    for (auto const& s : submitters) {
      if (s.seq != 0) {
        seqs.insert(s.seq);
      }
    }
    auto history = hq::list_history(*conn);
    if (!history) {
      return std::unexpected(std::format("cannot read the copy's history: {}", history.error().message));
    }
    for (auto const& row : *history) {
      if (seqs.contains(row.seq) && row.outcome == hq::history_outcome::abandoned) {
        ++abandoned;
      }
    }
    if (auto left = hq::list(*conn); left) {
      entries_left = static_cast<std::int64_t>(left->size());
    }
    final_checkpoint = passive_checkpoint(*conn);
  }

  // Merge.
  std::vector<double> enqueue_ms;
  std::vector<double> poll_ms;
  std::int64_t        polls_total = 0, completed = 0, skipped = 0, busy_errors = 0, other_errors = 0, enqueue_busy = 0;
  std::int64_t        missing = 0, gave_up = 0, fatal = 0, late_started = 0, late_total_seen = 0;
  std::string         first_error;
  for (auto const& s : submitters) {
    enqueue_ms.insert(enqueue_ms.end(), s.enqueue_ms.begin(), s.enqueue_ms.end());
    poll_ms.insert(poll_ms.end(), s.poll_ms.begin(), s.poll_ms.end());
    enqueue_busy += s.enqueue_busy;
    missing += s.entry_missing;
    gave_up += s.gave_up ? 1 : 0;
    fatal += s.fatal.empty() ? 0 : 1;
    if (first_error.empty()) {
      first_error = !s.fatal.empty() ? s.fatal : s.first_error;
    }
    if (s.late) {
      ++late_total_seen;
      late_started += s.ever_running ? 1 : 0;
    }
    for (auto const& p : s.polls) {
      ++polls_total;
      switch (p.kind) {
      case poll_kind::completed:
        ++completed;
        break;
      case poll_kind::skipped:
        ++skipped;
        break;
      case poll_kind::busy_error:
        ++busy_errors;
        break;
      case poll_kind::error:
        ++other_errors;
        break;
      }
    }
  }

  std::vector<check> checks;
  auto               add = [&](std::string name, std::string bound, std::string observed, bool pass) {
    checks.push_back({std::move(name), std::move(bound), std::move(observed), pass});
  };
  auto const  planning_acquire = summarize(planning.acquire_ms);
  std::string extra;

  if (opts.run == "r1") {
    add("planning_busy_failures", "== 0", std::to_string(planning.busy_failures), planning.busy_failures == 0);
    add("skipped_polls", "== 0", std::to_string(skipped), skipped == 0);
    add("planning_acquire_p99_ms", std::format("<= {}", k_planning_p99_bound_ms),
        planar::json_text::json_double(planning_acquire.p99), planning_acquire.p99 <= k_planning_p99_bound_ms);
  }
  if (is_r3) {
    std::int64_t inside = 0, inside_skipped = 0, inside_other = 0, straddle = 0;
    for (auto const& h : holds) {
      for (auto const& s : submitters) {
        if (s.late) {
          continue;
        }
        for (auto const& p : s.polls) {
          if (p.start_s >= h.start_s && p.end_s <= h.end_s) {
            ++inside;
            (p.kind == poll_kind::skipped ? inside_skipped : inside_other) += 1;
          } else if (p.start_s < h.end_s && p.end_s > h.start_s) {
            ++straddle;
          }
        }
      }
    }
    add("hold_completed", "every hold acquired and released",
        std::format("{} of {}", std::ranges::count_if(holds, [](auto const& h) { return h.done; }), holds.size()),
        std::ranges::all_of(holds, [](auto const& h) { return h.done; }));
    add("abandonments", "== 0", std::to_string(abandoned + missing), abandoned + missing == 0);
    add("polls_inside_a_hold_are_skipped", "inside > 0 and every one skipped",
        std::format("{} inside, {} skipped, {} other", inside, inside_skipped, inside_other), inside > 0 && inside_other == 0);
    add("late_submitters_started", "every late submitter's command started",
        std::format("{} of {}", late_started, late_total_seen), late_started == late_total_seen);
    add("submitters_gave_up", "== 0", std::to_string(gave_up), gave_up == 0);
    extra = std::format(
        R"(,"r3":{{"polls_inside_holds":{},"inside_skipped":{},"inside_other":{},"polls_straddling_a_hold":{},"holds":[)", inside,
        inside_skipped, inside_other, straddle);
    for (std::size_t h = 0; h < holds.size(); ++h) {
      extra += std::format(R"({}{{"requested_s":{},"start_s":{},"end_s":{},"done":{},"error":{}}})", h == 0 ? "" : ",",
                           planar::json_text::json_double(holds[h].requested_s), planar::json_text::json_double(holds[h].start_s),
                           planar::json_text::json_double(holds[h].end_s), holds[h].done, quote(holds[h].error));
    }
    extra += "]}";
  }
  if (is_r4) {
    double const total_s = rebuild.total_ms / 1000.0;
    add("rebuild_completed", "the rebuild transaction committed", rebuild.done ? "yes" : ("no: " + rebuild.error), rebuild.done);
    if (rebuild.done && total_s < k_rebuild_report_only_s) {
      add("abandonments", "== 0 (the rebuild took under 25s)", std::to_string(abandoned + missing), abandoned + missing == 0);
    } else if (rebuild.done) {
      add("rebuild_duration_reported", "reported, not failed, at 25s or more", planar::json_text::json_double(total_s) + "s",
          true);
    }
    extra =
        std::format(R"(,"r4":{{"table":{},"started":{},"done":{},"lock_acquire_ms":{},"lock_held_ms":{},"error":{},"steps":[)",
                    quote(rebuild.table), rebuild.started, rebuild.done, planar::json_text::json_double(rebuild.lock_acquire_ms),
                    planar::json_text::json_double(rebuild.total_ms), quote(rebuild.error));
    for (std::size_t i = 0; i < rebuild.steps.size(); ++i) {
      extra += std::format(R"({}{{"step":{},"ms":{}}})", i == 0 ? "" : ",", quote(rebuild.steps[i].first),
                           planar::json_text::json_double(rebuild.steps[i].second));
    }
    extra += "]}";
  }
  if (is_r5) {
    add("wal_max_bytes", std::format("<= {}", k_wal_bound_bytes), std::to_string(wal_max), wal_max <= k_wal_bound_bytes);
    add("reader_alive_at_stop", "the feed --follow reader ran for the whole run",
        reader_alive_at_stop ? "yes" : "no: it had exited", reader_alive_at_stop);
    add("checkpoint_completes_within_wait",
        std::format("log == checkpointed within {}s of the reader exiting", opts.checkpoint_wait_s),
        checkpoint_complete_after_s ? planar::json_text::json_double(*checkpoint_complete_after_s) + "s" : "never",
        checkpoint_complete_after_s.has_value());
    extra = std::format(
        R"(,"r5":{{"reader_alive_at_stop":{},"reader_exit_s":{},"checkpoint_complete_after_s":{},"last_checkpoint":{}}})",
        reader_alive_at_stop, reader_exit_s ? planar::json_text::json_double(*reader_exit_s) : "null",
        checkpoint_complete_after_s ? planar::json_text::json_double(*checkpoint_complete_after_s) : "null",
        checkpoint_json(last_checkpoint));
  }
  // Failures that are not lock waits are never "within bounds".
  add("planning_other_failures", "== 0",
      std::to_string(planning.other_failures) +
          (planning.other_failures == 0 || planning.first_error.empty() ? "" : ": " + planning.first_error),
      planning.other_failures == 0);
  add("queue_other_errors", "== 0",
      std::to_string(other_errors) + (other_errors == 0 || first_error.empty() ? "" : ": " + first_error), other_errors == 0);
  add("queue_polled", "polls > 0", std::to_string(polls_total), polls_total > 0);
  add("workers_ran", "no worker failed to open or enqueue", std::format("{} fatal", fatal), fatal == 0);

  bool const bounded  = opts.run != "r2";
  bool       all_pass = true;
  for (auto const& c : checks) {
    all_pass = all_pass && c.pass;
  }
  std::string const verdict = !bounded ? (fatal == 0 ? "informational" : "error") : (all_pass ? "within_bounds" : "breached");

  std::string report;
  report += std::format(R"({{"run":{},"verdict":{})", quote(opts.run), quote(verdict));
  report += std::format(
      R"(,"config":{{"submitters":{},"late_submitters":{},"poll_ms":{},"slots":{},"stale_after_ms":{},"busy_timeout_ms":{},"duration_s":{},"planning_interval_ms":{}}})",
      opts.submitters, late_total, opts.poll_ms, sh.slots, sh.stale_after_ms, k_busy_ms, opts.duration_s,
      opts.planning_interval_ms);
  report += std::format(R"(,"host":{{"cpus":{}}})", std::thread::hardware_concurrency());
  report += std::format(R"(,"source":{},"copy":{{"path":{},"schema_before":{},"schema_after":{}}})", source_report(),
                        quote(copy.path().string()), version_before, version_after);
  report += std::format(R"(,"run_seconds":{})", planar::json_text::json_double(run_seconds));
  report += std::format(
      R"(,"planning":{{"transactions":{},"busy_failures":{},"other_failures":{},"acquire":{},"work":{},"first_error":{}}})",
      planning.acquire_ms.size(), planning.busy_failures, planning.other_failures, summary_json(planning_acquire),
      summary_json(summarize(planning.work_ms)), quote(planning.first_error));
  report += std::format(
      R"(,"queue":{{"polls":{},"completed":{},"skipped":{},"busy_errors":{},"other_errors":{},"enqueue_busy":{},"enqueue":{},"poll":{},"abandoned":{},"entry_missing":{},"entries_left":{},"first_error":{}}})",
      polls_total, completed, skipped, busy_errors, other_errors, enqueue_busy, summary_json(summarize(enqueue_ms)),
      summary_json(summarize(poll_ms)), abandoned, missing, entries_left, quote(first_error));
  report += std::format(R"(,"wal":{{"max_bytes":{},"samples":{}}},"final_checkpoint":{})", wal_max, wal_samples.size(),
                        checkpoint_json(final_checkpoint));
  report += extra;
  report += std::format(R"(,"checks":{}}})", checks_json(checks));
  return std::pair{report, !bounded || all_pass};
}

} // namespace

auto main(int argc, char** argv) -> int {
  auto parsed = parse_options(std::span<char const* const>(argv + 1, static_cast<std::size_t>(argc - 1)));
  if (!parsed) {
    std::println(stderr, "queue_contention_probe: {}", parsed.error());
    return k_exit_refused;
  }
  auto const& opts = *parsed;

  // Refuse before anything is opened.
  auto vetted = vet_destination(opts.source, opts.dest);
  if (!vetted) {
    std::println(stderr, "queue_contention_probe: refused: {}", vetted.error());
    return k_exit_refused;
  }

  // Source identity before the copy, for the report and the unchanged proof.
  fs::path const source_real = fs::canonical(fs::path(opts.source));
  auto const     stat_before = stat_of(source_real);
  auto const     hash_before = file_sha256(source_real);

  if (auto copied = backup_source_to_copy(source_real.string(), *vetted); !copied) {
    std::println(stderr, "queue_contention_probe: {}", copied.error());
    return k_exit_error;
  }

  // Cleanup of the copy and its sidecars unless --keep.
  auto cleanup = [&] {
    if (opts.keep) {
      return;
    }
    std::error_code ec;
    for (std::string const suffix : {"", "-wal", "-shm", "-journal"}) {
      fs::remove(vetted->path().string() + suffix, ec);
    }
  };

  auto source_block = [&]() -> std::string {
    auto const stat_after = stat_of(source_real);
    auto const hash_after = file_sha256(source_real);
    return std::format(
        R"({{"path":{},"size_before":{},"size_after":{},"mtime_ns_before":{},"mtime_ns_after":{},"sha256_before":{},"sha256_after":{},"unchanged":{}}})",
        quote(source_real.string()), stat_before.size, stat_after.size, stat_before.mtime_ns, stat_after.mtime_ns,
        quote(hash_before), quote(hash_after),
        hash_before == hash_after && stat_before.size == stat_after.size && stat_before.mtime_ns == stat_after.mtime_ns);
  };
  auto result = run_experiment(opts, *vetted, source_block);
  if (!result) {
    std::println(stderr, "queue_contention_probe: {}", result.error());
    cleanup();
    return k_exit_error;
  }
  auto& [report, passed] = *result;
  std::println("{}", report);
  if (!opts.report.empty()) {
    std::ofstream out(opts.report, std::ios::binary | std::ios::trunc);
    out << report << '\n';
  }
  cleanup();
  return passed ? k_exit_ok : k_exit_breached;
}
