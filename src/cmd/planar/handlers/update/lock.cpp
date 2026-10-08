/// @file src/cmd/planar/handlers/update/lock.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.update.lock`. Each
/// function names the `mutation-lock.sh` function it ports; the comparisons,
/// orderings and messages are kept the same on purpose.

module;

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

module planar.cmd.planar.handlers.update.lock;

import std;
import planar.process;

namespace planar::cmd::update::lock {

namespace {

auto is_digits(std::string_view s) -> bool {
  return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

/// @brief A decimal string as a number; unset when it is not 1..18 digits.
auto as_number(std::string_view s) -> std::optional<std::uint64_t> {
  if (!is_digits(s) || s.size() > 18) {
    return std::nullopt;
  }
  std::uint64_t n = 0;
  std::from_chars(s.data(), s.data() + s.size(), n);
  return n;
}

auto own_pid() -> std::string {
  return std::to_string(::getpid());
}

/// @brief `[ -e p ] || [ -L p ]`.
auto present(const std::string& p) -> bool {
  struct stat st{};
  return ::lstat(p.c_str(), &st) == 0;
}

/// @brief The whole file, or unset.
auto slurp(const std::string& path) -> std::optional<std::string> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream body;
  body << in.rdbuf();
  return std::move(body).str();
}

/// @brief The first line of a file (`IFS= read -r line`), or unset.
auto first_line(const std::string& path) -> std::optional<std::string> {
  auto body = slurp(path);
  if (!body.has_value()) {
    return std::nullopt;
  }
  if (auto const nl = body->find('\n'); nl != std::string::npos) {
    body->resize(nl);
  }
  return body;
}

/// @brief Words split on blanks and newlines (the shell's default IFS).
auto words(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  std::size_t                   i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) {
      ++i;
    }
    auto const start = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n') {
      ++i;
    }
    if (i > start) {
      out.push_back(text.substr(start, i - start));
    }
  }
  return out;
}

/// @brief `_pl_nonce`: 32 lowercase hex digits from /dev/urandom.
auto make_nonce() -> std::optional<std::string> {
  std::ifstream                 in("/dev/urandom", std::ios::binary);
  std::array<unsigned char, 16> bytes{};
  if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
    return std::nullopt;
  }
  std::string out;
  for (auto const b : bytes) {
    out += std::format("{:02x}", b);
  }
  return out;
}

/// @brief What `kill -0 <pid>` tells about a recorded pid.
enum class liveness : std::uint8_t { exists, gone, unknown };

auto probe(std::string_view pid_text, std::string& why) -> liveness {
  auto const n = as_number(pid_text);
  if (!n.has_value() || *n > static_cast<std::uint64_t>(std::numeric_limits<::pid_t>::max())) {
    why = "the pid is out of range";
    return liveness::unknown;
  }
  if (::kill(static_cast<::pid_t>(*n), 0) == 0) {
    return liveness::exists;
  }
  auto const err = errno;
  if (err == EPERM) {
    return liveness::exists;
  }
  if (err == ESRCH) {
    return liveness::gone;
  }
  why = std::strerror(err);
  return liveness::unknown;
}

/// @brief `_pl_validate_dir`.
auto validate_dir(const std::string& l) -> std::expected<void, std::string> {
  struct stat st{};
  if (::lstat(l.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
    return std::unexpected(std::format("the mutation lock directory {} is a symlink; refusing to use it (remove it only if no "
                                       "Planar install, update or uninstall can be running)",
                                       l));
  }
  if (::stat(l.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    return std::unexpected(std::format("the mutation lock path {} exists and is not a directory; refusing to use it", l));
  }
  if (st.st_uid != ::geteuid()) {
    return std::unexpected(std::format("the mutation lock directory {} is not owned by the current user; refusing to use it", l));
  }
  if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    // The shell names the mode as `ls -ld` prints it.
    std::string mode = "d";
    for (int shift = 6; shift >= 0; shift -= 3) {
      auto const bits = (st.st_mode >> shift) & 7U;
      mode += (bits & 4U) != 0 ? 'r' : '-';
      mode += (bits & 2U) != 0 ? 'w' : '-';
      mode += (bits & 1U) != 0 ? 'x' : '-';
    }
    return std::unexpected(std::format("the mutation lock directory {} is writable by its group or by others (mode {}); refusing "
                                       "to use it. Make it private (chmod 700 {}) once no Planar install, update or uninstall "
                                       "is running.",
                                       l, mode, l));
  }
  return {};
}

/// @brief `_pl_ensure_dir`.
auto ensure_dir(const std::string& l) -> std::expected<void, std::string> {
  if (!present(l) && ::mkdir(l.c_str(), 0700) != 0 && !present(l)) {
    auto const parent = std::filesystem::path{l}.parent_path().string();
    return std::unexpected(std::format("cannot create the mutation lock directory {} (is {} writable?)", l, parent));
  }
  return validate_dir(l);
}

/// @brief `_pl_max_gen`: the highest `owner.<digits>` (at most 18 digits),
/// as written; "0" when there is none.
auto max_gen(const std::string& l) -> std::string {
  std::vector<std::string> names;
  std::error_code          ec;
  for (auto const& entry : std::filesystem::directory_iterator(l, ec)) {
    auto name = entry.path().filename().string();
    if (name.starts_with("owner.")) {
      names.push_back(std::move(name));
    }
  }
  std::ranges::sort(names);
  std::string   best = "0";
  std::uint64_t max  = 0;
  for (auto const& name : names) {
    auto const g = std::string_view{name}.substr(6);
    auto const n = as_number(g);
    if (n.has_value() && *n > max) {
      max  = *n;
      best = g;
    }
  }
  return best;
}

/// @brief `_pl_owner_state`'s verdicts.
enum class state : std::uint8_t { free, vanished, released, held, dead, ambiguous };

struct verdict {
  state       kind = state::ambiguous;
  std::string why;
  record      rec;
};

auto same_file(const std::string& a, const std::string& b) -> bool {
  struct stat sa{};
  struct stat sb{};
  return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/// @brief `_pl_owner_state`.
auto owner_state(const std::string& l, const std::string& g) -> verdict {
  if (g == "0") {
    return {.kind = state::free};
  }
  auto const rec_path = std::format("{}/owner.{}", l, g);
  if (!present(rec_path)) {
    return {.kind = state::vanished};
  }
  auto const parsed = parse_record(rec_path);
  if (!parsed.has_value()) {
    return {.why = std::format("the ownership record {} is malformed or not a regular file", rec_path)};
  }
  verdict     out{.rec = *parsed};
  auto const& r = out.rec;
  if (r.gen != g) {
    out.why = std::format("the ownership record {} names generation {}", rec_path, r.gen);
    return out;
  }
  auto const rel = std::format("{}/released.{}", l, g);
  if (present(rel)) {
    struct stat st{};
    if (::lstat(rel.c_str(), &st) == 0 && S_ISREG(st.st_mode) && same_file(rel, rec_path)) {
      out.kind = state::released;
      return out;
    }
    out.why = std::format("the release marker {} is not a link to its ownership record", rel);
    return out;
  }
  if (r.node != node_name()) {
    out.why = std::format("the owner ({} pid {}) is recorded on host {}, not this host, so its liveness cannot be checked",
                          r.operation, r.pid, r.node);
    return out;
  }
  std::string why;
  switch (probe(r.pid, why)) {
  case liveness::gone:
    out.kind = state::dead;
    out.why  = std::format("{} pid {} is no longer running", r.operation, r.pid);
    return out;
  case liveness::unknown:
    out.why = std::format("cannot tell whether {} pid {} is running ({})", r.operation, r.pid, why);
    return out;
  case liveness::exists:
    break;
  }
  auto const tok = start_token(static_cast<std::int64_t>(*as_number(r.pid)));
  if (!tok.has_value()) {
    out.why = std::format("{} pid {} exists but its start time cannot be read, so it cannot be told apart from a reused pid",
                          r.operation, r.pid);
    return out;
  }
  if (r.start.starts_with("ps:")) {
    out.why =
        std::format("{} pid {} exists and its record holds a local-time start (written by an older Planar), which cannot be "
                    "compared across time zones, so it cannot be told apart from a reused pid",
                    r.operation, r.pid);
    return out;
  }
  if (*tok == r.start) {
    out.kind = state::held;
    out.why  = std::format("{} pid {}", r.operation, r.pid);
    return out;
  }
  out.kind = state::dead;
  out.why  = std::format("{} pid {} has exited and the pid was reused", r.operation, r.pid);
  return out;
}

/// @brief `_pl_record_is_mine`.
auto record_is_mine(const std::string& path, std::string_view nonce) -> bool {
  auto const r = parse_record(path);
  return r.has_value() && r->nonce == nonce && r->pid == own_pid();
}

/// @brief `_pl_write_cand`: write this process's candidate for generation `g`.
auto write_candidate(const std::string& l, std::string_view g, std::string_view op, std::string_view root, std::string_view tmp,
                     std::string_view nonce) -> std::expected<std::string, std::string> {
  auto const start = start_token(::getpid());
  if (!start.has_value()) {
    return std::unexpected(
        std::format("cannot read this process's own start time (pid {}), so ownership could not be recorded", own_pid()));
  }
  auto const cand = std::format("{}/cand.{}.{}", l, own_pid(), nonce);
  ::unlink(cand.c_str());
  auto const body =
      std::format("planar-mutation-lock 1\ngen={}\noperation={}\npid={}\nstart={}\nnode={}\nnonce={}\nroot={}\ntmp={}\n", g, op,
                  own_pid(), *start, node_name(), nonce, root, tmp);
  int const fd = ::open(cand.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    return std::unexpected(std::format("cannot write a candidate ownership record in {}", l));
  }
  std::size_t done = 0;
  while (done < body.size()) {
    auto const n = ::write(fd, body.data() + done, body.size() - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      ::close(fd);
      ::unlink(cand.c_str());
      return std::unexpected(std::format("cannot write a candidate ownership record in {}", l));
    }
    done += static_cast<std::size_t>(n);
  }
  if (::close(fd) != 0) {
    ::unlink(cand.c_str());
    return std::unexpected(std::format("cannot write a candidate ownership record in {}", l));
  }
  return cand;
}

/// @brief `_pl_housekeep`: remove records of generations below `g`, and
/// candidates of exited processes.
auto housekeep(const std::string& l, std::uint64_t g) -> void {
  std::vector<std::string> names;
  std::error_code          ec;
  for (auto const& entry : std::filesystem::directory_iterator(l, ec)) {
    names.push_back(entry.path().filename().string());
  }
  for (auto const& name : names) {
    std::string_view const n{name};
    auto const             full = std::format("{}/{}", l, name);
    if (n.starts_with("owner.") || n.starts_with("released.") || n.starts_with("handoff.")) {
      auto const k = as_number(n.substr(n.rfind('.') + 1));
      if (k.has_value() && *k < g) {
        ::unlink(full.c_str());
      }
      continue;
    }
    if (n.starts_with("cand.")) {
      auto const rest = n.substr(5);
      auto const cpid = rest.substr(0, rest.find('.'));
      if (!is_digits(cpid) || cpid == own_pid()) {
        continue;
      }
      std::string why;
      if (probe(cpid, why) == liveness::gone) {
        ::unlink(full.c_str());
      }
    }
  }
}

} // namespace

auto lock_dir(std::string_view canonical_root) -> std::string {
  if (canonical_root.ends_with('/')) {
    canonical_root.remove_suffix(1);
  }
  return std::format("{}.lock", canonical_root);
}

auto start_token(std::int64_t pid) -> std::optional<std::string> {
  if (pid <= 0) {
    return std::nullopt;
  }
  auto const stat_path = std::format("/proc/{}/stat", pid);
  if (::access(stat_path.c_str(), R_OK) == 0) {
    auto const s = first_line(stat_path);
    if (!s.has_value()) {
      return std::nullopt;
    }
    auto const close = s->rfind(") ");
    if (close == std::string::npos) {
      return std::nullopt;
    }
    std::string boot;
    if (::access("/proc/sys/kernel/random/boot_id", R_OK) == 0) {
      boot = first_line("/proc/sys/kernel/random/boot_id").value_or("");
    }
    auto const fields = words(std::string_view{*s}.substr(close + 2));
    if (fields.size() < 20 || !is_digits(fields[19])) {
      return std::nullopt;
    }
    return std::format("proc:{}:{}", boot.empty() ? std::string{"none"} : boot, fields[19]);
  }
  auto const                            pid_text = std::to_string(pid);
  std::array<std::string_view, 7> const args{"TZ=UTC", "LC_ALL=C", "ps", "-o", "lstart=", "-p", pid_text};
  auto const                            got = process::capture("env", args);
  if (!got.spawned || got.exit_code != 0) {
    return std::nullopt;
  }
  auto const fields = words(got.output);
  if (fields.empty()) {
    return std::nullopt;
  }
  std::string tok = "psu:";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    tok += i == 0 ? "" : " ";
    tok += fields[i];
  }
  return tok;
}

auto node_name() -> std::string {
  struct utsname u{};
  if (::uname(&u) != 0 || u.nodename[0] == '\0') {
    return "unknown";
  }
  return u.nodename;
}

auto parse_record(const std::filesystem::path& path) -> std::optional<record> {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return std::nullopt;
  }
  auto const body = slurp(path.string());
  if (!body.has_value()) {
    return std::nullopt;
  }
  std::vector<std::string_view> lines;
  std::string_view              text{*body};
  while (!text.empty()) {
    auto const nl = text.find('\n');
    if (nl == std::string_view::npos) {
      lines.push_back(text);
      break;
    }
    lines.push_back(text.substr(0, nl));
    text.remove_prefix(nl + 1);
  }
  if (lines.empty() || lines.size() > 32 || lines.front() != "planar-mutation-lock 1") {
    return std::nullopt;
  }
  record                             r;
  std::set<std::string, std::less<>> seen;
  for (auto const line : lines | std::views::drop(1)) {
    auto const eq = line.find('=');
    if (line.empty() || line.front() < 'a' || line.front() > 'z' || eq == std::string_view::npos) {
      return std::nullopt;
    }
    auto const key   = line.substr(0, eq);
    auto const value = std::string{line.substr(eq + 1)};
    if (!seen.emplace(key).second) {
      return std::nullopt;
    }
    if (key == "gen") {
      r.gen = value;
    } else if (key == "operation") {
      r.operation = value;
    } else if (key == "pid") {
      r.pid = value;
    } else if (key == "start") {
      r.start = value;
    } else if (key == "node") {
      r.node = value;
    } else if (key == "nonce") {
      r.nonce = value;
    } else if (key == "root") {
      r.root = value;
    } else if (key == "tmp") {
      r.tmp = value;
    } else {
      return std::nullopt;
    }
  }
  if (!is_digits(r.gen) || !is_digits(r.pid)) {
    return std::nullopt;
  }
  if (r.operation != "install" && r.operation != "update" && r.operation != "uninstall") {
    return std::nullopt;
  }
  if (r.nonce.size() != 32 ||
      !std::ranges::all_of(r.nonce, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
    return std::nullopt;
  }
  if (r.start.empty() || r.node.empty() || r.root.empty()) {
    return std::nullopt;
  }
  return r;
}

auto acquire(std::string_view canonical_root, std::string_view operation, std::string_view tmp)
    -> std::expected<ownership, std::string> {
  if (operation != "install" && operation != "update" && operation != "uninstall") {
    return std::unexpected(std::format("unknown operation {}", operation));
  }
  auto const l = lock_dir(canonical_root);
  if (auto ok = ensure_dir(l); !ok) {
    return std::unexpected(ok.error());
  }
  auto const nonce = make_nonce();
  if (!nonce.has_value()) {
    return std::unexpected(std::string{"cannot read random bytes for an ownership nonce (/dev/urandom is required)"});
  }
  std::string cand;
  auto const  drop_cand = [&] {
    if (!cand.empty()) {
      ::unlink(cand.c_str());
    }
  };
  for (int tries = 1;; ++tries) {
    if (tries > 64) {
      drop_cand();
      return std::unexpected(std::format("could not acquire the mutation lock {}: too much contention", l));
    }
    auto const  g = max_gen(l);
    auto const  v = owner_state(l, g);
    std::string reclaimed;
    std::string reclaimed_tmp;
    switch (v.kind) {
    case state::vanished:
      continue;
    case state::held:
      drop_cand();
      return std::unexpected(std::format("another Planar {} (pid {}) is changing this installation; it holds the mutation lock "
                                         "{} (generation {}). Wait for it to finish, then re-run.",
                                         v.rec.operation, v.rec.pid, l, g));
    case state::ambiguous:
      drop_cand();
      return std::unexpected(
          std::format("the mutation lock {} cannot be judged free: {}. Nothing was changed. Remove {}/owner.{} "
                      "only after making sure no Planar install, update or uninstall is running.",
                      l, v.why, l, g));
    case state::dead:
      reclaimed     = std::format("{} pid {}", v.rec.operation, v.rec.pid);
      reclaimed_tmp = v.rec.tmp;
      break;
    case state::free:
    case state::released:
      break;
    }
    auto const next    = std::to_string(as_number(g).value_or(0) + 1);
    auto const written = write_candidate(l, next, operation, canonical_root, tmp, *nonce);
    if (!written) {
      return std::unexpected(written.error());
    }
    cand            = *written;
    auto const dest = std::format("{}/owner.{}", l, next);
    if (::link(cand.c_str(), dest.c_str()) != 0) {
      auto const err = errno;
      // Lost the race for this generation: start over. Any other failure
      // would repeat on every attempt, so name it.
      if (err == EEXIST || present(dest)) {
        continue;
      }
      drop_cand();
      return std::unexpected(std::format("cannot create the ownership record {}: {}. The mutation lock needs a directory you can "
                                         "write on a filesystem with hard links.",
                                         dest, std::strerror(err)));
    }
    auto const top = max_gen(l);
    if (as_number(top).value_or(0) > as_number(next).value_or(0)) {
      if (record_is_mine(dest, *nonce)) {
        ::unlink(dest.c_str());
      }
      continue;
    }
    drop_cand();
    ownership held{.dir       = l,
                   .root      = std::string{canonical_root},
                   .gen       = next,
                   .nonce     = *nonce,
                   .operation = std::string{operation},
                   .tmp       = std::string{tmp},
                   .reclaimed = reclaimed};
    if (!reclaimed_tmp.empty() && update_tmp_valid(canonical_root, reclaimed_tmp)) {
      held.reclaimed_tmp = reclaimed_tmp;
    }
    housekeep(l, *as_number(next));
    return held;
  }
}

auto assert_owner(const ownership& held) -> std::expected<void, std::string> {
  auto const rec = std::format("{}/owner.{}", held.dir, held.gen);
  if (max_gen(held.dir) != held.gen || present(std::format("{}/released.{}", held.dir, held.gen)) ||
      !record_is_mine(rec, held.nonce)) {
    return std::unexpected(std::format("this process no longer owns the mutation lock {} (generation {})", held.dir, held.gen));
  }
  return {};
}

auto release(const ownership& held) -> void {
  if (held.dir.empty() || held.gen.empty()) {
    return;
  }
  auto const rec = std::format("{}/owner.{}", held.dir, held.gen);
  auto const rel = std::format("{}/released.{}", held.dir, held.gen);
  if (record_is_mine(rec, held.nonce) && !present(rel)) {
    ::link(rec.c_str(), rel.c_str());
  }
}

auto update_tmp_valid(std::string_view canonical_root, std::string_view dir) -> bool {
  if (canonical_root.ends_with('/')) {
    canonical_root.remove_suffix(1);
  }
  auto const ns     = std::format("{}/{}", canonical_root, k_update_namespace);
  auto const prefix = ns + "/";
  if (!dir.starts_with(prefix)) {
    return false;
  }
  auto const name  = dir.substr(prefix.size());
  auto const alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  if (name.empty() || name == "." || name == ".." || !alnum(name.front()) ||
      !std::ranges::all_of(name, [&](char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; })) {
    return false;
  }
  auto const owned_dir = [](const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::geteuid();
  };
  return owned_dir(ns) && owned_dir(std::string{dir});
}

} // namespace planar::cmd::update::lock
