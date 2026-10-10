/// @file src/cmd/planar/handlers/update/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.update`. See the
/// module interface for the step list and the ownership hand-off.

module;

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

module planar.cmd.planar.handlers.update;

import std;
import cli11;
import planar.cliapp.args;
import planar.db.migrate;
import planar.http;
import planar.process;
import planar.sha256;
import planar.cmd.planar.context;
import planar.cmd.planar.declare;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.update.lock;
import planar.cmd.planar.handlers.update.state;

/// @brief The process environment, copied into the installer's.
extern "C" char** environ; // NOLINT(readability-redundant-declaration)

namespace planar::cmd::handlers {

namespace {

namespace up = planar::cmd::update;

/// @brief The largest `VERSION` body read; the bootstrap refuses one over 64 bytes.
constexpr std::uint64_t k_version_cap = 4096;
/// @brief The largest `SHA256SUMS` read.
constexpr std::uint64_t k_sums_cap = 1024ULL * 1024ULL;
/// @brief The largest bundle read. A release bundle is tens of megabytes; the
/// whole body is held in memory once more by `planar.sha256`, so the bound also
/// bounds this process's peak memory.
constexpr std::uint64_t k_asset_cap = 512ULL * 1024ULL * 1024ULL;

/// @brief Why a fetch failed, classified as the bootstrap's `fetch` does.
enum class fault_kind : std::uint8_t { unreachable, missing, other };

struct fetch_fault {
  fault_kind  kind = fault_kind::other;
  std::string reason;
};

auto failure(std::string body) -> std::unexpected<domain_error> {
  return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::move(body)));
}

auto bad_input(std::string body) -> std::unexpected<domain_error> {
  return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::move(body)));
}

// ---------------------------------------------------------------------------
// SIGINT and SIGTERM.
//
// A plain run catches both from just before it takes ownership until the
// hand-off. The handler only records the first signal; the run notices it at
// its checkpoints and in the download's cancel hook, unwinds through
// `update_session`, which removes the download directory and releases
// ownership, and exits 128+signo. Immediately before the exec the previous
// dispositions are restored: from there a signal acts as it would on any
// process (before the exec it kills this one, which leaves what a KILL leaves
// and the next owner reclaims; after it the installer's own handling applies),
// and none is lost between the last check and the exec.
// ---------------------------------------------------------------------------

/// @brief The first SIGINT or SIGTERM a plain run caught, or 0.
///
/// The one piece of namespace-scope mutable state here: a signal handler can
/// reach nothing else. It is written by `note_interrupt` and reset by
/// `interrupt_guard`'s constructor; one run arms one guard at a time.
std::atomic<int> g_update_interrupt{0};
static_assert(std::atomic<int>::is_always_lock_free);

/// @brief The signals a plain run turns into a clean stop.
constexpr std::array<int, 2> k_interrupt_signals{SIGINT, SIGTERM};

/// @brief The handler: record the first signal. Async-signal-safe (one
/// lock-free atomic operation).
auto note_interrupt(int sig) -> void {
  int none = 0;
  g_update_interrupt.compare_exchange_strong(none, sig);
}

/// @brief SIGINT and SIGTERM blocked on this thread for its lifetime.
class signals_blocked {
public:
  signals_blocked() {
    sigset_t set{};
    sigemptyset(&set);
    for (auto const sig : k_interrupt_signals) {
      sigaddset(&set, sig);
    }
    _ok = ::pthread_sigmask(SIG_BLOCK, &set, &_old) == 0;
  }
  signals_blocked(const signals_blocked&)            = delete;
  signals_blocked& operator=(const signals_blocked&) = delete;
  signals_blocked(signals_blocked&&)                 = delete;
  signals_blocked& operator=(signals_blocked&&)      = delete;
  ~signals_blocked() {
    if (_ok) {
      ::pthread_sigmask(SIG_SETMASK, &_old, nullptr);
    }
  }

private:
  sigset_t _old{};
  bool     _ok = false;
};

/// @brief Catches SIGINT and SIGTERM for one plain run and restores the
/// previous dispositions when destroyed. A signal that was ignored when the
/// run started (a background job's SIGINT) stays ignored.
class interrupt_guard {
public:
  interrupt_guard() {
    g_update_interrupt.store(0);
    arm();
  }
  interrupt_guard(const interrupt_guard&)            = delete;
  interrupt_guard& operator=(const interrupt_guard&) = delete;
  interrupt_guard(interrupt_guard&&)                 = delete;
  interrupt_guard& operator=(interrupt_guard&&)      = delete;
  ~interrupt_guard() {
    restore();
  }

  /// @brief Install the handler for each signal not ignored.
  auto arm() -> void {
    for (std::size_t i = 0; i < k_interrupt_signals.size(); ++i) {
      struct sigaction current{};
      if (_armed[i] || ::sigaction(k_interrupt_signals[i], nullptr, &current) != 0 || current.sa_handler == SIG_IGN) {
        continue;
      }
      struct sigaction ours{};
      ours.sa_handler = note_interrupt;
      ours.sa_flags   = SA_RESTART;
      sigfillset(&ours.sa_mask);
      _armed[i] = ::sigaction(k_interrupt_signals[i], &ours, &_previous[i]) == 0;
    }
  }

  /// @brief Put back the dispositions `arm` replaced.
  auto restore() -> void {
    for (std::size_t i = 0; i < k_interrupt_signals.size(); ++i) {
      if (_armed[i]) {
        ::sigaction(k_interrupt_signals[i], &_previous[i], nullptr);
        _armed[i] = false;
      }
    }
  }

  /// @brief The signal caught, or 0.
  [[nodiscard]] static auto caught() -> int {
    return g_update_interrupt.load();
  }

  /// @brief Stand down for the exec: with both signals blocked, refuse when
  /// one was caught, else restore the previous dispositions; then unblock.
  /// @return Whether the exec may proceed.
  [[nodiscard]] auto release_for_exec() -> bool {
    signals_blocked const blocked;
    if (caught() != 0) {
      return false;
    }
    restore();
    return true;
  }

private:
  std::array<struct sigaction, 2> _previous{};
  std::array<bool, 2>             _armed{};
};

/// @brief GET `url` under the download policy with a size bound, abandoned
/// when a plain run catches SIGINT or SIGTERM.
auto fetch(const std::string& url, const up::base_parts& base, std::uint64_t cap) -> std::expected<std::string, fetch_fault> {
  auto got = http::download(
      url, http::download_policy{.max_body_bytes = cap, .cancelled = [] { return interrupt_guard::caught() != 0; }});
  if (!got) {
    auto const& e = got.error();
    if (base.scheme == "file" && e.kind == http::download_error_kind::transport_failed) {
      std::error_code ec;
      auto const      kind = std::filesystem::is_directory(base.path, ec) ? fault_kind::missing : fault_kind::unreachable;
      return std::unexpected(fetch_fault{.kind = kind, .reason = std::format("no such file {}", url.substr(7))});
    }
    switch (e.kind) {
    case http::download_error_kind::timeout:
    case http::download_error_kind::transport_failed:
    case http::download_error_kind::certificate_verification_failed:
      return std::unexpected(fetch_fault{.kind = fault_kind::unreachable, .reason = e.message});
    default:
      return std::unexpected(fetch_fault{.kind = fault_kind::other, .reason = e.message});
    }
  }
  if (got->status != 200) {
    auto const kind = (got->status == 404 || got->status == 410) ? fault_kind::missing : fault_kind::other;
    return std::unexpected(fetch_fault{.kind = kind, .reason = std::format("HTTP {}", got->status)});
  }
  return std::move(got->body);
}

/// @brief The bootstrap's `fetch_die`: an unreachable server is named by its
/// base; a missing first file under a tag means the tag does not exist.
auto fault_message(const fetch_fault& f, bool first_under_tag, std::string_view what, std::string_view base, std::string_view tag)
    -> std::string {
  if (f.kind == fault_kind::unreachable) {
    return std::format("cannot reach the release server at {}: {}", base, f.reason);
  }
  if (f.kind == fault_kind::missing && first_under_tag) {
    return std::format("release {} does not exist on the release server {}: {}", tag, base, f.reason);
  }
  return std::format("{}: {}", what, f.reason);
}

/// @brief Fetch and validate `<base>/latest/download/VERSION`.
auto fetch_latest(const std::string& base, const up::base_parts& parts) -> std::expected<std::string, domain_error> {
  auto const url  = std::format("{}/latest/download/VERSION", base);
  auto const body = fetch(url, parts, k_version_cap);
  if (!body) {
    return failure(fault_message(body.error(), false, std::format("cannot read the latest release from {}", url), base, ""));
  }
  if (body->size() > 64) {
    return failure(std::format("{} is too large to be a release tag", url));
  }
  std::string tag = body->substr(0, body->find('\n'));
  std::erase(tag, '\r');
  if (!up::version_valid(tag)) {
    return failure(
        std::format(R"({} holds '{}', which is not a release tag (^v[0-9]+\.[0-9]+\.[0-9]+$))", url, up::printable(tag)));
  }
  return tag;
}

/// @brief The incomplete-installation report for a journal that pins or
/// cancels the installation, or unset when the previous completed release is
/// authoritative.
auto pending_recovery(const std::string& canon) -> std::optional<domain_error> {
  auto const reading = up::read_journal(canon);
  auto const jf      = std::format("{}/{}", canon, up::k_journal_name);
  switch (reading.status) {
  case up::journal_status::absent:
    return std::nullopt;
  case up::journal_status::invalid:
    return error_from_body(domain_error_kind::generic_failure,
                           std::format("{} is not a valid recovery journal for {}; nothing was changed. Inspect it; remove it by "
                                       "hand only if no install was interrupted",
                                       jf, canon));
  case up::journal_status::valid:
    break;
  }
  auto const& j = reading.record;
  if (j.phase == "mutating") {
    auto retry = j.value("retry");
    if (retry.empty()) {
      retry = "re-run the install that was interrupted";
    }
    return error_from_body(domain_error_kind::generic_failure,
                           std::format("the Planar installation at {} is incomplete: an install of {} was interrupted after it "
                                       "changed the installation (recovery journal {}). Complete it with:\n  {}",
                                       canon, j.value("target_version"), jf, retry));
  }
  if (j.phase == "uninstalling") {
    auto retry = j.value("retry");
    if (retry.empty()) {
      retry = std::format("run {}/bin/planar-uninstall (or uninstall.sh from a release bundle)", canon);
    }
    return error_from_body(domain_error_kind::generic_failure,
                           std::format("an uninstall of {} was interrupted; an update never resumes a cancelled installation. "
                                       "Finish the uninstall first: {}. Nothing was changed",
                                       canon, retry));
  }
  return std::nullopt;
}

/// @brief 16 random lowercase hex digits.
auto random_hex() -> std::string {
  std::random_device                           rd;
  std::uniform_int_distribution<std::uint32_t> dist(0, 15);
  std::string                                  out;
  for (int i = 0; i < 16; ++i) {
    out += "0123456789abcdef"[dist(rd)];
  }
  return out;
}

/// @brief Write `body` to a new file at `path` (exclusive, mode 0600, never through a symlink).
auto write_new_file(const std::string& path, std::string_view body) -> bool {
  int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t done = 0;
  while (done < body.size()) {
    auto const n = ::write(fd, body.data() + done, body.size() - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      ::close(fd);
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return ::close(fd) == 0;
}

/// @brief Run `tar` with `args`; stdout captured, stderr discarded.
auto run_tar(std::span<const std::string_view> args) -> process::capture_result {
  return process::capture("tar", args);
}

/// @brief The ownership and temporary directory of one update, released and
/// removed on every exit except a successful hand-off.
class update_session {
public:
  update_session(std::string canon, up::lock::ownership held) : _canon(std::move(canon)), _held(std::move(held)) {
  }
  update_session(const update_session&)            = delete;
  update_session& operator=(const update_session&) = delete;
  update_session(update_session&&)                 = delete;
  update_session& operator=(update_session&&)      = delete;

  ~update_session() {
    if (_handed_off) {
      return;
    }
    // Removal and release finish even when SIGINT or SIGTERM arrives now; a
    // signal that does is delivered once both are done.
    signals_blocked const blocked;
    if (_tmp_created && up::lock::update_tmp_valid(_canon, _held.tmp)) {
      std::error_code ec;
      std::filesystem::remove_all(_held.tmp, ec);
      ::rmdir(std::format("{}/{}", _canon, up::lock::k_update_namespace).c_str());
    }
    up::lock::release(_held);
  }

  /// @brief The ownership held.
  [[nodiscard]] auto held() const -> const up::lock::ownership& {
    return _held;
  }

  /// @brief Record that the temporary directory now exists.
  auto mark_tmp_created() -> void {
    _tmp_created = true;
  }

  /// @brief Leave the ownership and directory to the installer.
  auto hand_off() -> void {
    _handed_off = true;
  }

private:
  std::string         _canon;
  up::lock::ownership _held;
  bool                _tmp_created = false;
  bool                _handed_off  = false;
};

/// @brief Create `<root>/.planar-update/` if needed and then the update
/// temporary itself, exclusively.
///
/// The namespace is checked before anything is created inside it: a
/// symlinked or foreign `.planar-update` would otherwise receive a stray
/// directory at the link's target before the temporary's own check refused.
auto make_update_tmp(const std::string& canon, const std::string& tmp) -> std::expected<void, std::string> {
  auto const ns = std::format("{}/{}", canon, up::lock::k_update_namespace);
  if (::mkdir(ns.c_str(), 0700) != 0 && errno != EEXIST) {
    return std::unexpected(std::format("cannot create {}: {}", ns, std::strerror(errno)));
  }
  struct stat ns_st{};
  if (::lstat(ns.c_str(), &ns_st) != 0 || !S_ISDIR(ns_st.st_mode) || ns_st.st_uid != ::geteuid()) {
    return std::unexpected(std::format("{} is not a directory of this user (a symlink?); refusing to download into it", ns));
  }
  if (::mkdir(tmp.c_str(), 0700) != 0) {
    return std::unexpected(std::format("cannot create the update directory {}: {}", tmp, std::strerror(errno)));
  }
  if (!up::lock::update_tmp_valid(canon, tmp)) {
    return std::unexpected(
        std::format("{} is not a private directory of this user under {}; refusing to download into it", tmp, ns));
  }
  return {};
}

/// @brief Every byte written so far reaches the terminal before the exec.
auto flush_everything(context& ctx) -> void {
  ctx.out().flush();
  ctx.err().flush();
  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);
}

/// @brief The install root the installer will use: `$PLANAR_HOME`, else
/// `$HOME/.planar`, canonicalized, never `/` or `$HOME`.
auto resolve_root(const context& ctx) -> std::expected<std::string, domain_error> {
  auto const  home = ctx.env()("HOME");
  std::string given;
  if (auto const ph = ctx.env()("PLANAR_HOME"); ph.has_value()) {
    given = *ph;
  } else {
    if (!home.has_value() || home->empty()) {
      return failure("HOME is not set");
    }
    given = std::format("{}/.planar", *home);
  }
  if (!given.starts_with('/')) {
    return bad_input(std::format("PLANAR_HOME='{}' must be an absolute path", up::printable(given)));
  }
  if (given.contains('\n') || given.contains('\t')) {
    return bad_input("the install root holds a newline or tab; refusing");
  }
  auto const canon = up::canonical_path(given, ctx.cwd().string());
  if (!canon.has_value()) {
    return bad_input(std::format("cannot resolve the install root {} (symlink loop?)", given));
  }
  auto const home_canon = (home.has_value() && !home->empty()) ? up::canonical_path(*home, ctx.cwd().string()) : std::nullopt;
  if (*canon == "/" || (home_canon.has_value() && *canon == *home_canon)) {
    return bad_input(
        std::format("refusing to update: the install root {} resolves to {}. Rule: the install root must never be /, "
                    "$HOME or empty",
                    given, *canon));
  }
  return *canon;
}

/// @brief `PLANAR_RELEASE_URL` (empty counts as unset), normalized and validated.
auto resolve_base(const context& ctx) -> std::expected<std::pair<std::string, up::base_parts>, domain_error> {
  std::string base{up::k_default_release_base};
  if (auto const given = ctx.env()("PLANAR_RELEASE_URL"); given.has_value() && !given->empty()) {
    base = up::normalize_base(*given);
    if (!up::release_base_valid(base).has_value()) {
      return bad_input(up::release_base_refusal(*given));
    }
  }
  return std::pair{base, *up::release_base_valid(base)};
}

/// @brief `--check`: compare the installed release with the latest one.
auto check(context& ctx, const std::string& canon, const std::string& base, const up::base_parts& parts) -> handler_result {
  if (auto pending = pending_recovery(canon)) {
    return std::unexpected(std::move(*pending));
  }
  auto const installed = up::read_release(std::filesystem::path{canon} / "release.json");
  if (!installed) {
    return failure(installed.error());
  }
  auto const latest = fetch_latest(base, parts);
  if (!latest) {
    return std::unexpected(latest.error());
  }
  auto const have = installed->has_value() ? (*installed)->version : std::string{"none"};
  ctx.out() << std::format("installed {} latest {}\n", have, *latest);
  if (have == *latest) {
    return {};
  }
  domain_error available     = error_from_rendered(domain_error_kind::generic_failure, "");
  available.passthrough_code = exit_update_available;
  return std::unexpected(std::move(available));
}

/// @brief Check the archive's entries the way the bootstrap does, then extract it.
auto extract(const std::string& archive, const std::string& asset, const std::string& bundle_name, const std::string& into)
    -> std::expected<void, std::string> {
  std::array<std::string_view, 2> const list_args{"-tzf", archive};
  auto const                            listing = run_tar(list_args);
  if (!listing.spawned) {
    return std::unexpected(std::string{"tar is not installed; it is needed to unpack the release bundle; nothing was installed"});
  }
  if (listing.exit_code != 0) {
    return std::unexpected(std::format("{} is not a readable tar archive; nothing was installed", asset));
  }
  for (auto const piece : std::views::split(std::string_view{listing.output}, '\n')) {
    std::string_view const entry{piece.begin(), piece.end()};
    if (entry.empty()) {
      continue;
    }
    bool const inside = entry == bundle_name || entry.starts_with(bundle_name + "/");
    bool       dotdot = false;
    for (auto const comp : std::views::split(entry, '/')) {
      dotdot = dotdot || std::string_view{comp.begin(), comp.end()} == "..";
    }
    if (!inside || dotdot) {
      return std::unexpected(std::format("{} holds entries outside {}/; nothing was extracted or installed", asset, bundle_name));
    }
  }
  std::array<std::string_view, 2> const verbose_args{"-tvzf", archive};
  auto const                            verbose = run_tar(verbose_args);
  for (auto const piece : std::views::split(std::string_view{verbose.output}, '\n')) {
    std::string_view const line{piece.begin(), piece.end()};
    if (line.starts_with('l') || line.starts_with('h')) {
      return std::unexpected(std::format("{} holds a symbolic or hard link; nothing was extracted or installed", asset));
    }
  }
  if (::mkdir(into.c_str(), 0700) != 0) {
    return std::unexpected(std::format("cannot create {}", into));
  }
  std::array<std::string_view, 4> const extract_args{"-xzf", archive, "-C", into};
  auto const                            unpacked = run_tar(extract_args);
  if (!unpacked.spawned || unpacked.exit_code != 0) {
    return std::unexpected(std::format("cannot extract {}; nothing was installed", asset));
  }
  return {};
}

/// @brief The placeholder a checkpoint returns once a signal was caught;
/// `run_update` replaces it with `interrupted`.
auto stopped() -> std::unexpected<domain_error> {
  return failure("interrupted");
}

/// @brief The report of a plain run stopped by `sig`: exit 128+`sig`, as a
/// shell reports a command a signal ended and as the bootstrap's own `INT`
/// and `TERM` traps exit.
auto interrupted(int sig) -> std::unexpected<domain_error> {
  auto err = error_from_body(domain_error_kind::generic_failure,
                             std::format("planar update was interrupted by {}; nothing was installed, its download directory "
                                         "was removed and the mutation lock released",
                                         sig == SIGINT ? "SIGINT" : "SIGTERM"));
  err.passthrough_code = 128 + sig;
  return std::unexpected(std::move(err));
}

/// @brief A plain run, from the platform check to the hand-off, under
/// `guard`. Every return except a successful exec removes the download
/// directory and releases ownership (`update_session`).
auto replace_installation(context& ctx, const std::string& canon, const std::string& base, const up::base_parts& parts,
                          const std::optional<std::string>& want_tag, const update_host& host, interrupt_guard& guard)
    -> handler_result {
  auto const platform = host.platform();
  if (!platform) {
    return failure(platform.error());
  }
  auto const      asset       = std::format("planar-{}.tar.gz", *platform);
  auto const      bundle_name = std::format("planar-{}", *platform);
  std::error_code ec;
  if (!std::filesystem::is_directory(canon, ec)) {
    return failure(std::format("there is no Planar installation at {} to update; install one with: curl -fsSL "
                               "{}/latest/download/get-planar.sh | sh",
                               canon, up::k_default_release_base));
  }

  // 1. Ownership, with the temporary directory recorded before it exists.
  auto const tmp  = std::format("{}/{}/update-{}", canon, up::lock::k_update_namespace, random_hex());
  auto       held = up::lock::acquire(canon, "update", tmp);
  if (!held) {
    return failure(held.error());
  }
  update_session session(canon, std::move(*held));
  if (!session.held().reclaimed.empty()) {
    ctx.out() << std::format("planar update: reclaimed the mutation lock from an abandoned {}\n", session.held().reclaimed);
    if (!session.held().reclaimed_tmp.empty()) {
      std::filesystem::remove_all(session.held().reclaimed_tmp, ec);
      ctx.out() << std::format("planar update: removed the abandoned updater's temporary directory {}\n",
                               session.held().reclaimed_tmp);
    }
  }

  if (interrupt_guard::caught() != 0) {
    return stopped();
  }

  // 2. Pending recovery outranks any verdict about the current release.
  if (auto pending = pending_recovery(canon)) {
    return std::unexpected(std::move(*pending));
  }
  auto const installed = up::read_release(std::filesystem::path{canon} / "release.json");
  if (!installed) {
    return failure(installed.error());
  }

  // 3. The tag, and the no-change verdict over a completed install.
  std::string tag;
  if (want_tag.has_value()) {
    tag = *want_tag;
  } else {
    auto latest = fetch_latest(base, parts);
    if (!latest) {
      return std::unexpected(latest.error());
    }
    tag = std::move(*latest);
  }
  bool const stamped = std::filesystem::is_regular_file(std::filesystem::path{canon} / ".planar-install", ec);
  if (installed->has_value() && (*installed)->version == tag && stamped) {
    ctx.out() << std::format("planar update: Planar {} is installed in {} and current; no changes\n", tag, canon);
    return {};
  }

  // 4. Download and verify. A caught signal abandons a transfer in flight
  // through the download's cancel hook.
  if (interrupt_guard::caught() != 0) {
    return stopped();
  }
  if (auto made = make_update_tmp(canon, tmp); !made) {
    return failure(made.error());
  }
  session.mark_tmp_created();
  ctx.out() << std::format("planar update: installing Planar {} for {} from {}\n", tag, *platform, base);
  auto const assets = std::format("{}/download/{}", base, tag);
  auto const sums   = fetch(std::format("{}/SHA256SUMS", assets), parts, k_sums_cap);
  if (!sums) {
    return failure(fault_message(sums.error(), true,
                                 std::format("cannot download SHA256SUMS for {} from {}/SHA256SUMS", tag, assets), base, tag));
  }
  auto const body = fetch(std::format("{}/{}", assets, asset), parts, k_asset_cap);
  if (!body) {
    return failure(fault_message(body.error(), false,
                                 std::format("cannot download {} for {} from {}/{}", asset, tag, assets, asset), base, tag));
  }
  if (interrupt_guard::caught() != 0) {
    return stopped();
  }
  auto const expected = up::select_checksum_record(*sums, asset);
  if (!expected) {
    return failure(expected.error());
  }
  if (sha256::hex(*body) != *expected) {
    return failure(std::format(
        "checksum mismatch for {}: the download does not match SHA256SUMS; nothing was extracted or installed", asset));
  }
  auto const archive = std::format("{}/{}", tmp, asset);
  if (!write_new_file(archive, *body)) {
    return failure(std::format("cannot write {}; nothing was installed", archive));
  }

  // 5. Unpack and check the bundle.
  if (interrupt_guard::caught() != 0) {
    return stopped();
  }
  if (auto unpacked = extract(archive, asset, bundle_name, std::format("{}/x", tmp)); !unpacked) {
    return failure(unpacked.error());
  }
  auto const bundle         = std::format("{}/x/{}", tmp, bundle_name);
  auto const bundle_release = std::filesystem::is_regular_file(bundle + "/install.sh", ec)
                                  ? up::read_release(bundle + "/release.json")
                                  : std::expected<std::optional<up::release_info>, std::string>{};
  if (!bundle_release || !bundle_release->has_value()) {
    return failure(std::format("{} is not a release bundle (no install.sh or release.json); nothing was installed", asset));
  }
  auto const& release = **bundle_release;
  if (release.version != tag) {
    return failure(std::format("{} holds Planar '{}' but was fetched as {}; nothing was installed", asset,
                               up::printable(release.version), tag));
  }
  if (*platform == "linux-x86_64") {
    if (auto refused = up::glibc_refusal(release.os_floor, host.ldd_first_line())) {
      return failure(std::move(*refused));
    }
  }
  auto const own_schema = static_cast<std::uint64_t>(::planar::db::embedded_max());
  if (!release.schema_version.has_value()) {
    return failure(std::format("the {} bundle's release.json records no schema_version, so it cannot be checked against database "
                               "schema version {} of this planar; nothing was installed",
                               tag, own_schema));
  }
  if (*release.schema_version < own_schema) {
    return failure(
        std::format("the {} bundle carries database schema version {}, older than schema version {} of this planar; an "
                    "older release is never installed over a newer database, so nothing was installed",
                    tag, *release.schema_version, own_schema));
  }

  // 6. Hand off. Ownership is checked, never released, before the exec.
  // The shadow check (`command -v planar` versus `<root>/bin/planar`) is not made
  // here: the exec below never returns control, and a check before it would run
  // before the new binaries are placed. The bundled installer makes it after
  // placement, with the environment this process hands it, and names
  // `~/.local/bin/planar` when that is the shadowing path. Printing it here too
  // would print it twice.
  if (auto owned = up::lock::assert_owner(session.held()); !owned) {
    return failure(owned.error());
  }
  auto const bash = process::resolve_program(ctx.env(), "bash");
  if (!bash.has_value()) {
    return failure("bash is required to run the installer; nothing was installed");
  }
  exec_request const request{
      .program = *bash,
      .argv    = {"bash", bundle + "/install.sh", "--prebuilt", bundle, "--cleanup", session.held().tmp},
      .env     = {{"PLANAR_MUTATION_HANDOFF", std::format("{}:{}", session.held().gen, session.held().nonce)},
                  {"PLANAR_RELEASE_URL", base},
                  {"PLANAR_EXPECT_RECOVERY", std::nullopt}},
  };
  ctx.out() << std::format("planar update: handing over to {}/install.sh\n", bundle);
  flush_everything(ctx);
  // The boundary: from here SIGINT and SIGTERM act as they would without this
  // run (see `interrupt_guard`), and after the exec the installer owns the
  // directory and ownership.
  if (!guard.release_for_exec()) {
    return stopped();
  }
  auto const ran = host.exec(request);
  if (ran) {
    session.hand_off();
    return {};
  }
  guard.arm();
  return failure(
      std::format("cannot run the installer ({} {}/install.sh): {}; nothing was installed and the download was removed", *bash,
                  bundle, std::strerror(ran.error())));
}

} // namespace

auto native_host() -> update_host {
  return update_host{
      .platform = []() -> std::expected<std::string, std::string> {
        struct utsname u{};
        if (::uname(&u) != 0) {
          return up::platform_for("", "");
        }
        return up::platform_for(u.sysname, u.machine);
      },
      .ldd_first_line = []() -> std::optional<std::string> {
        std::array<std::string_view, 1> const args{"--version"};
        auto const                            got = process::capture("ldd", args);
        if (!got.spawned) {
          return std::nullopt;
        }
        return got.output.substr(0, got.output.find('\n'));
      },
      .exec = [](const exec_request& req) -> std::expected<void, int> {
        std::vector<std::string> env;
        for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
          std::string_view const entry{*e};
          auto const             name = entry.substr(0, entry.find('='));
          if (std::ranges::none_of(req.env, [&](const env_change& c) { return c.first == name; })) {
            env.emplace_back(entry);
          }
        }
        for (auto const& [name, value] : req.env) {
          if (value.has_value()) {
            env.push_back(std::format("{}={}", name, *value));
          }
        }
        std::vector<char*>       argv;
        std::vector<char*>       envp;
        std::vector<std::string> args = req.argv;
        for (auto& a : args) {
          argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        for (auto& v : env) {
          envp.push_back(v.data());
        }
        envp.push_back(nullptr);
        ::execve(req.program.c_str(), argv.data(), envp.data());
        return std::unexpected(errno);
      },
  };
}

auto run_update(context& ctx, const cliapp::parsed_args& args, const update_host& host) -> handler_result {
  auto const want_check = cliapp::flag_bool(args, "--check");
  auto const want_tag   = cliapp::flag_string(args, "--version");
  if (want_check && want_tag.has_value()) {
    return bad_input("--check compares with the latest release and takes no --version");
  }
  if (want_tag.has_value() && !up::version_valid(*want_tag)) {
    return bad_input(
        std::format(R"(--version '{}' is not a release tag; it must match ^v[0-9]+\.[0-9]+\.[0-9]+$ (for example v1.2.3))",
                    up::printable(*want_tag)));
  }
  auto const canon = resolve_root(ctx);
  if (!canon) {
    return std::unexpected(canon.error());
  }
  auto const base_and_parts = resolve_base(ctx);
  if (!base_and_parts) {
    return std::unexpected(base_and_parts.error());
  }
  auto const& [base, parts] = *base_and_parts;
  if (want_check) {
    return check(ctx, *canon, base, parts);
  }

  interrupt_guard guard;
  auto            result = replace_installation(ctx, *canon, base, parts, want_tag, host, guard);
  if (auto const sig = interrupt_guard::caught(); sig != 0) {
    // Whatever the run was doing, the operator asked it to stop, and
    // `update_session` has already removed the download and released ownership.
    return interrupted(sig);
  }
  return result;
}

auto update(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_update(ctx, args, native_host());
}

auto declare_update(CLI::App& root) -> void {
  auto* leaf = root.add_subcommand("update", "Update the Planar installation to a published release.");
  add_bool(*leaf, "--check", "Report the installed and latest release; exit 10 when an update is available");
  add_string(*leaf, "--version", "Install this release tag (vMAJOR.MINOR.PATCH) instead of the latest");
}

} // namespace planar::cmd::handlers
