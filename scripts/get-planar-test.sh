#!/usr/bin/env bash
# get-planar-test.sh -- the release bootstrap, end to end (plan 1122, task
# rel-bootstrap-test; tech spec 677, "The bootstrap"; test spec 679).
#
# Runs scripts/get-planar.sh against a loopback release server in a scratch HOME.
# The server is `python3 -m http.server` serving a directory laid out like the
# GitHub release base (latest/download/VERSION and download/<tag>/{SHA256SUMS,
# planar-<platform>.tar.gz}); python3 is a TEST-ONLY dependency (decision 1333:
# no Python on any operator path), and the bootstrap runs with a PATH that holds
# no python3 at all. GNU wget is a declared test dependency too: the test fails
# when it is missing, it never skips. The bundles are the shared fake bundles
# (scripts/fixtures/prebuilt-bundle.sh, published by
# src/cmd/planar/handlers/update/release_fixture.sh): tiny stub binaries with the
# real install.sh and install-lib.
#
# Every case runs from a fresh scratch HOME with its own TMPDIR and asserts the
# exit status, the refusal's fixed opening text, that a refusal left HOME exactly
# as it was and that no bootstrap temporary directory survived. The host
# commands uname, sw_vers and ldd are shimmed so one host exercises every
# platform branch; curl and wget are wrapped by shims that can fire a hook
# (publish a newer release, deliver a signal, change the recovery journal) at a
# chosen request, which is how "a release published mid-run", "interrupted
# during the download" and "the journal changed before replay" are staged.
# Runs under stock bash 3.2.
# shellcheck disable=SC2016  # literal $ in fixtures and messages
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_UNDER_TEST="$ROOT/scripts/get-planar.sh"
FIXTURE="$ROOT/src/cmd/planar/handlers/update/release_fixture.sh"
WORK="$(cd "$(mktemp -d)" && pwd -P)"
SERVER_PID=""
# shellcheck disable=SC2329  # runs from the EXIT trap
cleanup() {
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

PASSED=0
FAILED=0
CASE=""
CASE_FAILS=0
die() { printf 'get-planar-test: %s\n' "$*" >&2; exit 2; }
fail() { # fail MESSAGE -- record a failure of the current case
  CASE_FAILS=$((CASE_FAILS + 1))
  printf 'FAIL %s: %s\n' "$CASE" "$*" >&2
}
begin_case() { CASE="$1"; CASE_FAILS=0; }
end_case() {
  if [[ "$CASE_FAILS" -eq 0 ]]; then
    PASSED=$((PASSED + 1))
    printf 'PASS %s\n' "$CASE"
  else
    FAILED=$((FAILED + 1))
  fi
}

# --- the host tools -----------------------------------------------------------------

[[ -f "$SCRIPT_UNDER_TEST" && -f "$FIXTURE" ]] || die "the repository layout is not as expected under $ROOT"
PYTHON3="$(command -v python3 || true)"
[[ -n "$PYTHON3" ]] || die "python3 is a test-only dependency of this test (the loopback release server) and is not installed"
REAL_CURL="$(command -v curl || true)"
[[ -n "$REAL_CURL" ]] || die "curl is a dependency of this test and is not installed"
REAL_WGET="$(command -v wget || true)"
[[ -n "$REAL_WGET" ]] || die "GNU wget is a declared dependency of this test (the wget fallback case) and is not installed"
"$REAL_WGET" --version 2>/dev/null | head -n 1 | grep -q 'GNU Wget' || die "$REAL_WGET is not GNU Wget; the wget fallback case needs GNU Wget"

HOST_OS="$(uname -s)"
case "$HOST_OS" in
  Darwin) HOST_PLATFORM=macos-arm64; HOST_UNAME_M=arm64 ;;
  Linux)  HOST_PLATFORM=linux-x86_64; HOST_UNAME_M=x86_64 ;;
  *) die "this test runs on macOS or Linux, not $HOST_OS" ;;
esac
ASSET="planar-$HOST_PLATFORM.tar.gz"

# --- the PATH the bootstrap sees: no python3, shimmed uname/sw_vers/ldd ------------------

mk_tools() { # mk_tools DIR [EXCLUDED_NAME...] -- symlinks to the host tools the bootstrap and installer need
  local dir="$1" n found x skip
  shift
  mkdir -p "$dir"
  local base_names
  base_names="$(sed -n '/^BASE_DEPS=(/,/^)/p' "$ROOT/install.sh" | sed -n 's/^  "\([^|"]*\)|.*/\1/p')"
  for n in $base_names sh bash env tar gzip gunzip tail id sha256sum shasum xargs expr test true false touch stat ln wget; do
    skip=0
    for x in "$@"; do [[ "$x" == "$n" ]] && skip=1; done
    [[ "$skip" -eq 0 ]] || continue
    case "$n" in uname|wget) continue ;; esac
    found="$(command -v "$n" || true)"
    case "$found" in /*) ln -sf "$found" "$dir/$n" ;; esac
  done
  for n in python3 python curl cmake ninja clang; do
    [[ ! -e "$dir/$n" ]] || die "$n must not be in the tool directory"
  done
}

SHIM="$WORK/shim"       # uname, sw_vers, ldd, wget (always); curl lives in SHIMC
SHIMC="$WORK/shim-curl"
mkdir -p "$SHIM" "$SHIMC" "$WORK/hooks" "$WORK/cases" "$WORK/srv"
cat > "$SHIM/uname" <<'STUB'
#!/bin/sh
case "${1:-}" in
  -s) echo "$FAKE_UNAME_S" ;;
  -m) echo "$FAKE_UNAME_M" ;;
  *) exec /usr/bin/uname "$@" ;;
esac
STUB
cat > "$SHIM/sw_vers" <<'STUB'
#!/bin/sh
if [ "${1:-}" = -productVersion ]; then echo "$FAKE_MACOS"; exit 0; fi
exec /usr/bin/sw_vers "$@"
STUB
cat > "$SHIM/ldd" <<'STUB'
#!/bin/sh
echo "$FAKE_LDD_LINE"
STUB
# wget: logs each call, then runs the real one.
cat > "$SHIM/wget" <<'STUB'
#!/bin/sh
if [ -n "${FAKE_WGET_LOG:-}" ]; then
  for a in "$@"; do last=$a; done
  case "$last" in -*) ;; *) echo "$last" >> "$FAKE_WGET_LOG" ;; esac
fi
# FAKE_FILE_LIMIT=N makes every file the real client writes past N blocks fail with a
# real write error (SIGXFSZ ignored, so the client sees EFBIG): a full disk.
if [ -n "${FAKE_FILE_LIMIT:-}" ]; then ulimit -f "$FAKE_FILE_LIMIT"; trap '' XFSZ; fi
exec "$FAKE_REAL_WGET" "$@"
STUB
# curl: runs the real one; when the request URL matches FAKE_HOOK_MATCH it also
# runs FAKE_HOOK_CMD (a file run by /bin/sh) before or after (FAKE_HOOK_WHEN).
cat > "$SHIMC/curl" <<'STUB'
#!/bin/sh
for a in "$@"; do url=$a; done
# FAKE_CURL_EXIT=N: curl fails with exit status N before any request, as for a bad certificate setup.
if [ -n "${FAKE_CURL_EXIT:-}" ]; then echo "curl: ($FAKE_CURL_EXIT) fake failure" >&2; exit "$FAKE_CURL_EXIT"; fi
fire=no
if [ -n "${FAKE_HOOK_MATCH:-}" ]; then
  case "$url" in $FAKE_HOOK_MATCH) fire=yes ;; esac
fi
[ "$fire" = yes ] && [ "${FAKE_HOOK_WHEN:-after}" = before ] && /bin/sh "$FAKE_HOOK_CMD"
# FAKE_FILE_LIMIT=N: a full disk, as in the wget shim.
if [ -n "${FAKE_FILE_LIMIT:-}" ]; then ulimit -f "$FAKE_FILE_LIMIT"; trap '' XFSZ; fi
"$FAKE_REAL_CURL" "$@"
rc=$?
[ "$fire" = yes ] && [ "${FAKE_HOOK_WHEN:-after}" = after ] && /bin/sh "$FAKE_HOOK_CMD"
exit "$rc"
STUB
chmod +x "$SHIM"/* "$SHIMC"/*

TOOLS="$WORK/tools"
mk_tools "$TOOLS"
HASH_TOOLS=""
for n in sha256sum shasum; do
  [[ -e "$TOOLS/$n" ]] && HASH_TOOLS="$HASH_TOOLS $n"
done
[[ -n "$HASH_TOOLS" ]] || die "neither sha256sum nor shasum is on this host"

# --- the release server ---------------------------------------------------------------

SRV="$WORK/srv"
SRV_LOG="$WORK/server.log"
start_server() { # start_server DIR LOG [SCRIPT] -- sets SRV_PORT_OUT and SRV_PID_OUT (port 0, read back)
  local dir="$1" log="$2" script="${3:-}" port
  if [[ -n "$script" ]]; then
    "$PYTHON3" -u "$script" >"$log" 2>&1 &
  else
    "$PYTHON3" -u -m http.server 0 --bind 127.0.0.1 --directory "$dir" >"$log" 2>&1 &
  fi
  SRV_PID_OUT=$!
  for _ in $(seq 1 100); do
    port="$(sed -n 's/^Serving HTTP on [^ ]* port \([0-9][0-9]*\).*/\1/p' "$log" | head -n 1)"
    if [[ -n "$port" ]]; then SRV_PORT_OUT="$port"; return 0; fi
    kill -0 "$SRV_PID_OUT" 2>/dev/null || die "the http server exited: $(cat "$log")"
    sleep 0.1
  done
  die "the http server did not report a port: $(cat "$log")"
}
start_server "$SRV" "$SRV_LOG"
SERVER_PID="$SRV_PID_OUT"
PORT="$SRV_PORT_OUT"
SERVER="http://127.0.0.1:$PORT"
# A port with nothing behind it: a server that was started and stopped.
start_server "$WORK/srv" "$WORK/dead.log"
DEAD_PORT="$SRV_PORT_OUT"
kill "$SRV_PID_OUT"; wait "$SRV_PID_OUT" 2>/dev/null
[[ "$DEAD_PORT" != "$PORT" ]] || die "the dead port equals the live port"

# --- release templates, built once ----------------------------------------------------

publish() { # publish REL TAG PLATFORM [LATEST] -- one fake release under the directory REL
  bash "$FIXTURE" "$ROOT" "$1" "$2" "$3" 41 "${4:-yes}" || die "could not publish $2 $3 into $1"
}
TMPL="$WORK/tmpl-host"; publish "$TMPL" v1.0.0 "$HOST_PLATFORM"
TMPL_LINUX="$WORK/tmpl-linux"; publish "$TMPL_LINUX" v1.0.0 linux-x86_64
NEXT="$WORK/next-host"; publish "$NEXT" v1.1.0 "$HOST_PLATFORM"
mkdir -p "$WORK/extract"
tar -xzf "$TMPL/download/v1.0.0/$ASSET" -C "$WORK/extract" || die "cannot unpack the template bundle"
BUNDLE_V1="$WORK/extract/planar-$HOST_PLATFORM"
cp "$SCRIPT_UNDER_TEST" "$TMPL/download/v1.0.0/get-planar.sh"

# --- case plumbing ----------------------------------------------------------------------

new_case() { # new_case NAME [TEMPLATE] -- fresh HOME, TMPDIR and release directory (served as $BASE)
  begin_case "$1"
  local d="$WORK/cases/$1"
  H="$d/home"; TMPD="$d/tmp"; REL="$SRV/$1"
  mkdir -p "$H/.claude" "$H/.codex" "$TMPD"
  cp -R "${2:-$TMPL}" "$REL"
  BASE="$SERVER/$1"
  HOME_BEFORE="$(home_listing)"
  OUT="$d/out"; ERR="$d/err"
  LOGMARK="$(wc -l < "$SRV_LOG" | tr -d ' ')"
}
home_listing() { ( cd "$H" && find . | LC_ALL=C sort | while IFS= read -r f; do if [[ -f "$f" ]]; then printf '%s %s\n' "$f" "$(cksum < "$f")"; else printf '%s\n' "$f"; fi; done ); }
server_log() { tail -n +"$((LOGMARK + 1))" "$SRV_LOG"; }
requests() { server_log | sed -n 's/.*"GET \([^ ]*\) HTTP.*/\1/p'; }

RUN_PATH=""
# boot [VAR=value ...] -- run the bootstrap in the case's scratch HOME. Sets RC,
# OUT and ERR. PATH defaults to the shims, the curl shim and the tool directory.
boot() {
  RC=0
  local path="${RUN_PATH:-$SHIM:$SHIMC:$TOOLS}"
  ( cd "$H" && /usr/bin/env -i HOME="$H" PATH="$path" TMPDIR="$TMPD" LC_ALL=C NO_COLOR=1 \
      PLANAR_DB="$H/.planar/planar.db" PLANAR_CONFIG_PATH="$H/.planar/config.toml" \
      FAKE_UNAME_S="$HOST_OS" FAKE_UNAME_M="$HOST_UNAME_M" FAKE_MACOS=26.0 \
      FAKE_LDD_LINE="ldd (GNU libc) 2.39" FAKE_REAL_CURL="$REAL_CURL" FAKE_REAL_WGET="$REAL_WGET" \
      "$@" \
      sh "$SCRIPT_UNDER_TEST" >"$OUT" 2>"$ERR" ) || RC=$?
}

expect_rc() { [[ "$RC" == "$1" ]] || fail "exit status $RC, expected $1; stderr: $(cat "$ERR"); stdout: $(cat "$OUT")"; }
expect_err() { grep -Fq -- "$1" "$ERR" || fail "stderr lacks '$1'; stderr: $(cat "$ERR")"; }
expect_out() { grep -Fq -- "$1" "$OUT" || fail "stdout lacks '$1'; stdout: $(cat "$OUT")"; }
expect_home_unchanged() { [[ "$(home_listing)" == "$HOME_BEFORE" ]] || fail "HOME changed on a refusal: $(home_listing)"; }
expect_tmp_clean() { [[ -z "$(ls -A "$TMPD")" ]] || fail "the temporary directory was left behind: $(ls -A "$TMPD")"; }
expect_no_requests() { [[ -z "$(requests)" ]] || fail "the server saw requests: $(requests)"; }
refusal() { # refusal EXIT TEXT -- the standard assertions of a refused run
  expect_rc "$1"; expect_err "$2"; expect_home_unchanged; expect_tmp_clean
}
release_version() { sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' "$H/.planar/release.json"; }
tree_sum() { ( cd "$1" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do cksum "$f"; done ); }
expect_matches_bundle() { # expect_matches_bundle BUNDLE_DIR -- every managed subtree is the bundle's
  local b="$1" p="$H/.planar" n
  for n in planar planar-agent planar-watch planar-execute planar-ext; do
    cmp -s "$b/bin/$n" "$p/bin/$n" || fail "bin/$n is not the bundle's"
  done
  cmp -s "$b/release.json" "$p/release.json" || fail "release.json is not the bundle's"
  [[ "$(tree_sum "$b/codex-agents")" == "$(tree_sum "$p/codex-agents")" ]] || fail "codex-agents/ differs from the bundle's"
  [[ "$(tree_sum "$b/skills/planar")" == "$(tree_sum "$p/skills/planar")" ]] || fail "skills/planar differs from the bundle's"
  [[ "$(tree_sum "$b/templates")" == "$(tree_sum "$p/templates")" ]] || fail "templates/ differs from the bundle's"
  [[ "$(tree_sum "$b/workflows")" == "$(tree_sum "$p/workflows")" ]] || fail "workflows/ differs from the bundle's"
  [[ "$(tree_sum "$b/migrations")" == "$(tree_sum "$p/migrations")" ]] || fail "migrations/ differs from the bundle's"
  [[ ! -e "$p/.planar-journal" ]] || fail "a recovery journal is left behind"
}
corrupt_tarball() { printf 'tail' >> "$1"; }

hook_script() { # hook_script NAME BODY -- writes $WORK/hooks/NAME and prints its path
  printf '%s\n' "$2" > "$WORK/hooks/$1"
  printf '%s' "$WORK/hooks/$1"
}
publish_next_hook() { # publish_next_hook REL -- the hook that publishes v1.1.0 as latest into REL
  hook_script "publish-next-$CASE" "cp -R '$NEXT/download/v1.1.0' '$1/download/v1.1.0' && cp '$NEXT/latest/download/VERSION' '$1/latest/download/VERSION'"
}
publish_next() { cp -R "$NEXT/download/v1.1.0" "$1/download/v1.1.0" && cp "$NEXT/latest/download/VERSION" "$1/latest/download/VERSION"; }

# kill_mutating -- run the bootstrap with the installer killed after its mutating
# record; the journal pins v1.0.0 and the case's base.
kill_mutating() {
  boot PLANAR_RELEASE_URL="$BASE" PLANAR_INSTALL_TEST_FAULT=kill@after-mutating PLANAR_INSTALL_TEST_FAULT_ARMED=test-only
  [[ "$RC" -ne 0 ]] || fail "the killed install exited 0"
  expect_err "the installer exited with status"
  grep -Fxq 'phase=mutating' "$H/.planar/.planar-journal" 2>/dev/null || fail "no mutating journal after the kill"
  expect_tmp_clean
}

# =====================================================================================
# 3725 install: latest, from the release server
# =====================================================================================
new_case install-latest
boot PLANAR_RELEASE_URL="$BASE"
expect_rc 0
expect_out "installing Planar v1.0.0 for $HOST_PLATFORM"
expect_matches_bundle "$BUNDLE_V1"
[[ "$(release_version)" == v1.0.0 ]] || fail "release.json names '$(release_version)', not v1.0.0"
expect_tmp_clean
"$H/.planar/bin/planar" version >/dev/null 2>&1 || fail "the installed planar does not run"
reqs="$(requests)"
[[ "$reqs" == *"/$CASE/latest/download/VERSION"* ]] || fail "VERSION was not read: $reqs"
for want in SHA256SUMS "$ASSET"; do
  printf '%s\n' "$reqs" | grep -Fxq "/$CASE/download/v1.0.0/$want" || fail "$want was not fetched from download/v1.0.0/: $reqs"
done
[[ "$(printf '%s\n' "$reqs" | grep -vc "^/$CASE/\(latest/download/VERSION\|download/v1.0.0/\(SHA256SUMS\|$ASSET\)\)\$")" == 0 ]] \
  || fail "unexpected requests (only the selected platform's tarball may be fetched): $reqs"
[[ -z "$(find "$H" -name '*python*' 2>/dev/null)" ]] || fail "python was involved"
end_case

# a second run is a no-op install of the same release
begin_case install-rerun-is-idempotent
boot PLANAR_RELEASE_URL="$BASE"
expect_rc 0
expect_matches_bundle "$BUNDLE_V1"
expect_tmp_clean
end_case

# =====================================================================================
# pinned install; the checksum tools; the published entry point (3762)
# =====================================================================================
new_case install-pinned
publish_next "$REL"
boot PLANAR_RELEASE_URL="$BASE" PLANAR_VERSION=v1.0.0
expect_rc 0
[[ "$(release_version)" == v1.0.0 ]] || fail "release.json names '$(release_version)', not the pinned v1.0.0"
expect_matches_bundle "$BUNDLE_V1"
expect_tmp_clean
printf '%s\n' "$(requests)" | grep -Fq 'latest/download/VERSION' && fail "a pinned install read latest/download/VERSION"
end_case

for tool in $HASH_TOOLS; do
  new_case "install-checksum-tool-$tool"
  other=sha256sum; [[ "$tool" == sha256sum ]] && other=shasum
  mk_tools "$WORK/tools-only-$tool" "$other"
  [[ -e "$WORK/tools-only-$tool/$tool" && ! -e "$WORK/tools-only-$tool/$other" ]] || fail "could not isolate $tool"
  RUN_PATH="$SHIM:$SHIMC:$WORK/tools-only-$tool"
  boot PLANAR_RELEASE_URL="$BASE"
  RUN_PATH=""
  expect_rc 0
  expect_matches_bundle "$BUNDLE_V1"
  expect_tmp_clean
  end_case
done

new_case entry-point-curl-pipe
EP="$BASE/download/v1.0.0/get-planar.sh"
cmp -s "$TMPL/download/v1.0.0/get-planar.sh" "$SCRIPT_UNDER_TEST" || fail "the published get-planar.sh is not the repository's"
RC=0
( cd "$H" && "$REAL_CURL" -fsSL "$EP" | /usr/bin/env -i HOME="$H" PATH="$SHIM:$SHIMC:$TOOLS" TMPDIR="$TMPD" LC_ALL=C \
    PLANAR_DB="$H/.planar/planar.db" PLANAR_CONFIG_PATH="$H/.planar/config.toml" \
    FAKE_UNAME_S="$HOST_OS" FAKE_UNAME_M="$HOST_UNAME_M" FAKE_MACOS=26.0 FAKE_LDD_LINE="ldd (GNU libc) 2.39" \
    FAKE_REAL_CURL="$REAL_CURL" FAKE_REAL_WGET="$REAL_WGET" \
    PLANAR_VERSION=v1.0.0 PLANAR_RELEASE_URL="$BASE" sh >"$OUT" 2>"$ERR"
  p=("${PIPESTATUS[@]}"); [[ "${p[0]}" == 0 ]] || exit 90; exit "${p[1]}" ) || RC=$?
expect_rc 0
[[ "$(release_version)" == v1.0.0 ]] || fail "PLANAR_VERSION did not reach the consuming shell: release.json names '$(release_version)'"
expect_matches_bundle "$BUNDLE_V1"
expect_tmp_clean
end_case

# =====================================================================================
# SHA256SUMS refusals (3763): checksum mismatch, missing, duplicate, malformed, path-bearing
# =====================================================================================
new_case checksum-mismatch
corrupt_tarball "$REL/download/v1.0.0/$ASSET"
boot PLANAR_RELEASE_URL="$BASE"
refusal 1 "get-planar: checksum mismatch for $ASSET: the download does not match SHA256SUMS; nothing was extracted or installed"
end_case

sums_case() { # sums_case NAME EXPECTED_TEXT SED_SCRIPT
  new_case "$1"
  sed "$3" "$REL/download/v1.0.0/SHA256SUMS" > "$WORK/sums.new" && cp "$WORK/sums.new" "$REL/download/v1.0.0/SHA256SUMS"
  boot PLANAR_RELEASE_URL="$BASE"
  refusal 1 "$2"
  end_case
}
sums_case checksum-record-missing "get-planar: SHA256SUMS has no checksum record for $ASSET" "/$ASSET\$/d"
sums_case checksum-record-duplicate "get-planar: SHA256SUMS has 2 checksum records for $ASSET" "/$ASSET\$/{p;}"
sums_case checksum-record-short-hash "get-planar: the SHA256SUMS record for $ASSET is malformed or names a path" "/$ASSET\$/s/^[0-9a-f]//"
sums_case checksum-record-nonhex-hash "get-planar: the SHA256SUMS record for $ASSET holds a malformed hash" "/$ASSET\$/s/^[0-9a-f]/Z/"
sums_case checksum-record-path "get-planar: the SHA256SUMS record for $ASSET is malformed or names a path" "/$ASSET\$/s/  /  .\\//"

# =====================================================================================
# platform refusals: nothing is fetched, nothing is installed
# =====================================================================================
new_case unsupported-platform-freebsd
boot PLANAR_RELEASE_URL="$BASE" FAKE_UNAME_S=FreeBSD FAKE_UNAME_M=amd64
refusal 1 "get-planar: unsupported platform FreeBSD amd64; Planar release bundles exist for macos-arm64"
expect_no_requests
end_case

new_case unsupported-platform-intel-mac
boot PLANAR_RELEASE_URL="$BASE" FAKE_UNAME_S=Darwin FAKE_UNAME_M=x86_64
refusal 1 "get-planar: unsupported platform Darwin x86_64"
expect_no_requests
end_case

new_case unsupported-platform-arm-linux
boot PLANAR_RELEASE_URL="$BASE" FAKE_UNAME_S=Linux FAKE_UNAME_M=aarch64
refusal 1 "get-planar: unsupported platform Linux aarch64"
expect_no_requests
end_case

new_case old-macos
boot PLANAR_RELEASE_URL="$BASE" FAKE_UNAME_S=Darwin FAKE_UNAME_M=arm64 FAKE_MACOS=15.4
refusal 1 "get-planar: this is macOS 15.4; Planar release bundles need macOS 26.0 or later"
expect_no_requests
end_case

new_case old-macos-unreadable-version
boot PLANAR_RELEASE_URL="$BASE" FAKE_UNAME_S=Darwin FAKE_UNAME_M=arm64 FAKE_MACOS=
refusal 1 "get-planar: cannot read the macOS version"
expect_no_requests
end_case

new_case glibc-too-old
rm -rf "$REL"; cp -R "$TMPL_LINUX" "$REL"
boot PLANAR_RELEASE_URL="$BASE" FAKE_UNAME_S=Linux FAKE_UNAME_M=x86_64 FAKE_LDD_LINE="ldd (Ubuntu GLIBC 2.31-0ubuntu9) 2.31"
refusal 1 "get-planar: this host has glibc 2.31 but this release needs glibc 2.36 or later; nothing was installed"
end_case

new_case glibc-numeric-comparison
# 2.9 is older than 2.36 numerically, though it sorts after it as a string.
rm -rf "$REL"; cp -R "$TMPL_LINUX" "$REL"
boot PLANAR_RELEASE_URL="$BASE" FAKE_UNAME_S=Linux FAKE_UNAME_M=x86_64 FAKE_LDD_LINE="ldd (GNU libc) 2.9"
refusal 1 "get-planar: this host has glibc 2.9 but this release needs glibc 2.36 or later; nothing was installed"
end_case

new_case glibc-musl
rm -rf "$REL"; cp -R "$TMPL_LINUX" "$REL"
boot PLANAR_RELEASE_URL="$BASE" FAKE_UNAME_S=Linux FAKE_UNAME_M=x86_64 FAKE_LDD_LINE="musl libc (x86_64)"
refusal 1 "get-planar: cannot read the glibc version from 'ldd --version'"
end_case

# =====================================================================================
# server and VERSION refusals
# =====================================================================================
new_case missing-tag
boot PLANAR_RELEASE_URL="$BASE" PLANAR_VERSION=v9.9.9
refusal 1 "get-planar: release v9.9.9 does not exist on the release server $BASE: HTTP 404"
end_case

new_case malformed-version-word
printf 'latest\n' > "$REL/latest/download/VERSION"
boot PLANAR_RELEASE_URL="$BASE"
refusal 1 "get-planar: $BASE/latest/download/VERSION holds 'latest', which is not a release tag"
end_case

new_case malformed-version-two-parts
printf 'v1.2\n' > "$REL/latest/download/VERSION"
boot PLANAR_RELEASE_URL="$BASE"
refusal 1 "get-planar: $BASE/latest/download/VERSION holds 'v1.2', which is not a release tag"
end_case

new_case malformed-version-injection
printf 'v1.0.0; touch pwned\n' > "$REL/latest/download/VERSION"
boot PLANAR_RELEASE_URL="$BASE"
refusal 1 "which is not a release tag"
[[ ! -e "$H/pwned" && ! -e "$H/.planar" ]] || fail "VERSION content was acted on"
end_case

new_case malformed-version-oversize
head -c 200 /dev/zero | tr '\0' 'v' > "$REL/latest/download/VERSION"
boot PLANAR_RELEASE_URL="$BASE"
refusal 1 "is too large to be a release tag"
end_case

new_case version-file-missing
rm -f "$REL/latest/download/VERSION"
boot PLANAR_RELEASE_URL="$BASE"
refusal 1 "get-planar: cannot read the latest release from $BASE/latest/download/VERSION: HTTP 404"
end_case

new_case unreachable-server
BASE="http://127.0.0.1:$DEAD_PORT/none"
boot PLANAR_RELEASE_URL="$BASE"
refusal 1 "get-planar: cannot reach the release server at $BASE:"
end_case

new_case unreachable-server-pinned
BASE="http://127.0.0.1:$DEAD_PORT/none"
boot PLANAR_RELEASE_URL="$BASE" PLANAR_VERSION=v1.0.0
refusal 1 "get-planar: cannot reach the release server at $BASE:"
end_case

new_case bad-release-url
boot PLANAR_RELEASE_URL="http://example.com/releases"
refusal 1 "get-planar: PLANAR_RELEASE_URL='http://example.com/releases' is not an accepted release base"
end_case

new_case bad-version-env
boot PLANAR_RELEASE_URL="$BASE" PLANAR_VERSION='v1.0'
refusal 1 "get-planar: PLANAR_VERSION='v1.0' is not a release tag"
expect_no_requests
end_case

# =====================================================================================
# 3727: a release published between the VERSION read and the asset fetch
# =====================================================================================
new_case release-published-mid-run
HOOK="$(publish_next_hook "$REL")"
boot PLANAR_RELEASE_URL="$BASE" FAKE_HOOK_MATCH='*/latest/download/VERSION' FAKE_HOOK_WHEN=after FAKE_HOOK_CMD="$HOOK"
[[ "$(cat "$REL/latest/download/VERSION")" == v1.1.0 ]] || fail "the hook did not publish v1.1.0 as latest"
expect_rc 0
[[ "$(release_version)" == v1.0.0 ]] || fail "release.json names '$(release_version)', not the originally resolved v1.0.0"
expect_matches_bundle "$BUNDLE_V1"
expect_tmp_clean
reqs="$(requests)"
printf '%s\n' "$reqs" | grep -Fq '/download/v1.1.0/' && fail "an asset was fetched from the newly published release: $reqs"
printf '%s\n' "$reqs" | grep -Fxq "/$CASE/download/v1.0.0/$ASSET" || fail "the tarball did not come from download/v1.0.0/: $reqs"
printf '%s\n' "$reqs" | grep -Fxq "/$CASE/download/v1.0.0/SHA256SUMS" || fail "SHA256SUMS did not come from download/v1.0.0/: $reqs"
[[ "$(printf '%s\n' "$reqs" | grep -c 'latest/download/VERSION')" == 1 ]] || fail "VERSION was read more than once: $reqs"
end_case

# =====================================================================================
# 3732, 3767: interrupted bootstrap, recovery pin
# =====================================================================================
new_case interrupted-during-download
PIDFILE="$WORK/hooks/boot.pid"; rm -f "$PIDFILE"
HOOK="$(hook_script "term-$CASE" "i=0; while [ ! -s '$PIDFILE' ] && [ \$i -lt 100 ]; do sleep 0.1; i=\$((i+1)); done; kill -TERM \"\$(cat '$PIDFILE')\"; sleep 1")"
RC=0
( cd "$H" && exec /usr/bin/env -i HOME="$H" PATH="$SHIM:$SHIMC:$TOOLS" TMPDIR="$TMPD" LC_ALL=C \
    PLANAR_DB="$H/.planar/planar.db" PLANAR_CONFIG_PATH="$H/.planar/config.toml" \
    FAKE_UNAME_S="$HOST_OS" FAKE_UNAME_M="$HOST_UNAME_M" FAKE_MACOS=26.0 FAKE_LDD_LINE="ldd (GNU libc) 2.39" \
    FAKE_REAL_CURL="$REAL_CURL" FAKE_REAL_WGET="$REAL_WGET" PLANAR_RELEASE_URL="$BASE" \
    FAKE_HOOK_MATCH="*/$ASSET" FAKE_HOOK_WHEN=before FAKE_HOOK_CMD="$HOOK" \
    sh "$SCRIPT_UNDER_TEST" >"$OUT" 2>"$ERR" ) &
BOOT_PID=$!
printf '%s\n' "$BOOT_PID" > "$PIDFILE"
wait "$BOOT_PID" || RC=$?
[[ "$RC" -ne 0 ]] || fail "the signalled bootstrap exited 0"
expect_tmp_clean
expect_home_unchanged
[[ ! -e "$H/.planar" ]] || fail "the interrupted bootstrap created ~/.planar"
end_case

new_case interrupted-install-rerun-completes
kill_mutating
[[ ! -e "$H/.planar/release.json" ]] || fail "release.json exists before the recovery"
boot PLANAR_RELEASE_URL="$BASE"
expect_rc 0
expect_matches_bundle "$BUNDLE_V1"
[[ "$(release_version)" == v1.0.0 ]] || fail "the re-run installed '$(release_version)'"
expect_tmp_clean
end_case

new_case recovery-pins-interrupted-release
kill_mutating
JOURNAL_BASE="$(sed -n 's/^release_base=//p' "$H/.planar/.planar-journal")"
[[ "$JOURNAL_BASE" == "$BASE" ]] || fail "the journal records release base '$JOURNAL_BASE', not $BASE"
publish_next "$REL"
LOGMARK="$(wc -l < "$SRV_LOG" | tr -d ' ')"
boot
expect_rc 0
expect_out "finishing the interrupted install of v1.0.0"
expect_out "finished the interrupted install of v1.0.0"
[[ "$(release_version)" == v1.0.0 ]] || fail "recovery installed '$(release_version)', not the recorded v1.0.0"
expect_matches_bundle "$BUNDLE_V1"
expect_tmp_clean
reqs="$(requests)"
printf '%s\n' "$reqs" | grep -Fq 'latest/download/VERSION' && fail "a recovery run read latest/download/VERSION: $reqs"
printf '%s\n' "$reqs" | grep -Fq 'v1.1.0' && fail "a recovery run touched v1.1.0: $reqs"
# the recovery run stopped; the next run goes to latest
LOGMARK="$(wc -l < "$SRV_LOG" | tr -d ' ')"
boot PLANAR_RELEASE_URL="$BASE"
expect_rc 0
[[ "$(release_version)" == v1.1.0 ]] || fail "the run after recovery installed '$(release_version)', not latest v1.1.0"
printf '%s\n' "$(requests)" | grep -Fq 'latest/download/VERSION' || fail "the run after recovery did not read latest"
end_case

new_case recovery-conflicting-pin
kill_mutating
publish_next "$REL"
before_journal="$(cksum < "$H/.planar/.planar-journal")"
LOGMARK="$(wc -l < "$SRV_LOG" | tr -d ' ')"
boot PLANAR_RELEASE_URL="$BASE" PLANAR_VERSION=v1.1.0
expect_rc 1
expect_err "get-planar: PLANAR_VERSION=v1.1.0 conflicts with the interrupted install of v1.0.0"
expect_err "Run: curl -fsSL $BASE/download/v1.0.0/get-planar.sh | PLANAR_VERSION=v1.0.0"
[[ "$(cksum < "$H/.planar/.planar-journal")" == "$before_journal" ]] || fail "the journal changed"
[[ ! -e "$H/.planar/release.json" ]] || fail "a conflicting pin installed"
expect_tmp_clean
expect_no_requests
end_case

new_case recovery-conflicting-base
kill_mutating
LOGMARK="$(wc -l < "$SRV_LOG" | tr -d ' ')"
boot PLANAR_RELEASE_URL="$SERVER/elsewhere"
expect_rc 1
expect_err "get-planar: PLANAR_RELEASE_URL=$SERVER/elsewhere conflicts with the release base $BASE recorded by the interrupted install of v1.0.0"
expect_tmp_clean
expect_no_requests
end_case

new_case recovery-assets-missing-keeps-evidence
kill_mutating
publish_next "$REL"
rm -rf "$REL/download/v1.0.0"
boot
expect_rc 1
expect_err "get-planar: release v1.0.0 does not exist on the release server $BASE"
expect_err "the interrupted install of v1.0.0 is kept for another try"
grep -Fxq 'phase=mutating' "$H/.planar/.planar-journal" || fail "the recovery journal was not kept"
[[ ! -e "$H/.planar/release.json" ]] || fail "release.json appeared"
expect_tmp_clean
end_case

new_case recovery-journal-changes-before-replay
kill_mutating
JOURNAL="$H/.planar/.planar-journal"
# make the rewrite certain to differ: set the id to a fixed other value
HOOK="$(hook_script "flip-$CASE" "sed 's/^operation_id=.*/operation_id=abababababababababababababababab/' '$JOURNAL' > '$JOURNAL.new' && cat '$JOURNAL.new' > '$JOURNAL' && rm -f '$JOURNAL.new'")"
boot FAKE_HOOK_MATCH="*/$ASSET" FAKE_HOOK_WHEN=after FAKE_HOOK_CMD="$HOOK"
expect_rc 1
expect_err "the recovery state of $H/.planar changed while $ASSET was downloading; refusing to replay stale evidence. Run the command again"
[[ ! -e "$H/.planar/release.json" && ! -e "$H/.planar/bin" ]] || fail "stale evidence was replayed"
grep -Fxq 'phase=mutating' "$JOURNAL" || fail "the journal was not kept"
expect_tmp_clean
end_case

# =====================================================================================
# shadowing binary: the installer's warning, printed once
# =====================================================================================
new_case shadowing-binary
mkdir -p "$H/.local/bin"
printf '#!/bin/sh\nexit 0\n' > "$H/.local/bin/planar"; chmod 755 "$H/.local/bin/planar"
HOME_BEFORE="$(home_listing)"
RUN_PATH="$H/.local/bin:$SHIM:$SHIMC:$TOOLS"
boot PLANAR_RELEASE_URL="$BASE"
RUN_PATH=""
expect_rc 0
n="$(cat "$OUT" "$ERR" | grep -c 'shadows the installed')"
[[ "$n" == 1 ]] || fail "the shadow warning appeared $n times, not once: $(cat "$OUT" "$ERR")"
# shellcheck disable=SC2088  # the warning prints a literal ~
cat "$OUT" "$ERR" | grep -Fq '~/.local/bin/planar shadows the installed '"$H/.planar/bin/planar" || fail "the warning does not name ~/.local/bin/planar and the installed binary: $(cat "$OUT" "$ERR")"
expect_matches_bundle "$BUNDLE_V1"
expect_tmp_clean
end_case

new_case no-shadow-when-planar-bin-first
RUN_PATH="$H/.planar/bin:$SHIM:$SHIMC:$TOOLS"
boot PLANAR_RELEASE_URL="$BASE"
RUN_PATH=""
expect_rc 0
cat "$OUT" "$ERR" | grep -Fq 'shadows' && fail "a shadow warning appeared with ~/.planar/bin first"
end_case

# =====================================================================================
# wget fallback: curl is not on PATH
# =====================================================================================
new_case wget-fallback
mk_tools "$WORK/tools-nocurl"
[[ ! -e "$WORK/tools-nocurl/curl" ]] || fail "curl is in the no-curl tool directory"
RUN_PATH="$SHIM:$WORK/tools-nocurl"
[[ -z "$(PATH="$RUN_PATH" command -v curl || true)" ]] || fail "curl is still on the PATH"
: > "$WORK/wget.log"
boot PLANAR_RELEASE_URL="$BASE" FAKE_WGET_LOG="$WORK/wget.log"
RUN_PATH=""
expect_rc 0
expect_matches_bundle "$BUNDLE_V1"
expect_tmp_clean
for want in "$BASE/latest/download/VERSION" "$BASE/download/v1.0.0/SHA256SUMS" "$BASE/download/v1.0.0/$ASSET"; do
  grep -Fxq "$want" "$WORK/wget.log" || fail "wget did not fetch $want: $(cat "$WORK/wget.log")"
done
end_case

new_case wget-fallback-missing-tag
RUN_PATH="$SHIM:$WORK/tools-nocurl"
boot PLANAR_RELEASE_URL="$BASE" PLANAR_VERSION=v9.9.9
RUN_PATH=""
refusal 1 "get-planar: release v9.9.9 does not exist on the release server $BASE: HTTP 404"
end_case

new_case wget-fallback-unreachable
BASE="http://127.0.0.1:$DEAD_PORT/none"
RUN_PATH="$SHIM:$WORK/tools-nocurl"
boot PLANAR_RELEASE_URL="$BASE"
RUN_PATH=""
refusal 1 "get-planar: cannot reach the release server at $BASE:"
end_case

new_case no-download-client
mk_tools "$WORK/tools-noclient" wget
RUN_PATH="$SHIM:$WORK/tools-noclient"
rm -f "$SHIM/wget.off"; mv "$SHIM/wget" "$SHIM/wget.off"
boot PLANAR_RELEASE_URL="$BASE"
mv "$SHIM/wget.off" "$SHIM/wget"
RUN_PATH=""
refusal 1 "get-planar: neither curl nor wget is installed; install one of them"
expect_no_requests
end_case


# =====================================================================================
# task rel-m5-bootstrap-gaps: a failure names its cause, and only a refused
# connection names the server unreachable
# =====================================================================================
# A server that promises 100000 bytes, sends 5000 and closes: a truncated transfer.
cat > "$WORK/truncating-server.py" <<'PY'
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0))
s.listen(16)
print("Serving HTTP on 127.0.0.1 port %d" % s.getsockname()[1], flush=True)
while True:
    c, _ = s.accept()
    c.recv(65536)
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n" + b"x" * 5000)
    c.close()
PY
start_server "$WORK/srv" "$WORK/trunc.log" "$WORK/truncating-server.py"
TRUNC_PID="$SRV_PID_OUT"; TRUNC_PORT="$SRV_PORT_OUT"
[[ -d "$WORK/tools-nocurl" ]] || mk_tools "$WORK/tools-nocurl"

expect_not_err() { grep -Fq -- "$1" "$ERR" && fail "stderr has '$1' but must not; stderr: $(cat "$ERR")"; return 0; }

for client in curl wget; do
  if [[ "$client" == curl ]]; then CLIENT_PATH=""; else CLIENT_PATH="$SHIM:$WORK/tools-nocurl"; fi

  new_case "cause-refused-$client"
  BASE="http://127.0.0.1:$DEAD_PORT/none"
  RUN_PATH="$CLIENT_PATH"; boot PLANAR_RELEASE_URL="$BASE"; RUN_PATH=""
  refusal 1 "get-planar: cannot reach the release server at $BASE:"
  expect_not_err "cut short"
  expect_not_err "cannot write"
  end_case

  new_case "cause-truncated-$client"
  BASE="http://127.0.0.1:$TRUNC_PORT/rel"
  RUN_PATH="$CLIENT_PATH"; boot PLANAR_RELEASE_URL="$BASE" PLANAR_VERSION=v1.0.0; RUN_PATH=""
  refusal 1 "get-planar: cannot download SHA256SUMS for v1.0.0 from $BASE/download/v1.0.0/SHA256SUMS: the transfer was cut short"
  expect_not_err "cannot reach"
  expect_not_err "cannot write"
  end_case

  new_case "cause-full-disk-$client"
  # ulimit -f makes the real $client fail the tarball's write with a real write error
  # (curl exit 23, wget exit 3); the small VERSION and SHA256SUMS files still fit.
  RUN_PATH="$CLIENT_PATH"; boot PLANAR_RELEASE_URL="$BASE" FAKE_FILE_LIMIT=1; RUN_PATH=""
  refusal 1 "get-planar: cannot download $ASSET for v1.0.0 from $BASE/download/v1.0.0/$ASSET: cannot write the download to the temporary directory"
  expect_not_err "cannot reach"
  expect_not_err "cut short"
  end_case
done
kill "$TRUNC_PID" 2>/dev/null; wait "$TRUNC_PID" 2>/dev/null

# =====================================================================================
# task rel-m6-bootstrap-cert-errors: curl exit 58 and 77 are local certificate problems
# =====================================================================================
for code in 58 77; do
  new_case "cause-certificate-curl-$code"
  boot PLANAR_RELEASE_URL="$BASE" FAKE_CURL_EXIT="$code"
  refusal 1 "local certificate problem (curl exit status $code)"
  expect_not_err "cannot reach"
  expect_no_requests
  end_case
done

# =====================================================================================
# task rel-m5-bootstrap-gaps: one trailing-slash rule in the bootstrap and install.sh
# =====================================================================================
new_case recovery-trailing-slash-base
boot PLANAR_RELEASE_URL="$BASE//" PLANAR_INSTALL_TEST_FAULT=kill@after-mutating PLANAR_INSTALL_TEST_FAULT_ARMED=test-only
[[ "$RC" -ne 0 ]] || fail "the killed install exited 0"
[[ "$(sed -n 's/^release_base=//p' "$H/.planar/.planar-journal")" == "$BASE" ]] \
  || fail "the journal records '$(sed -n 's/^release_base=//p' "$H/.planar/.planar-journal")', not $BASE"
boot PLANAR_RELEASE_URL="$BASE/"
expect_rc 0
expect_out "finished the interrupted install of v1.0.0"
expect_matches_bundle "$BUNDLE_V1"
expect_tmp_clean
end_case

new_case base-normalize-parity
# The bootstrap is a standalone release asset and cannot source install-lib, so the two
# rules are separate code. They must agree on every input.
# shellcheck source=/dev/null
source "$ROOT/scripts/install-lib/release.sh"
eval "$(sed -n '/^base_normalize() {/,/^}/p' "$SCRIPT_UNDER_TEST")"
declare -F release_base_normalize >/dev/null || fail "install-lib/release.sh has no release_base_normalize"
if declare -F release_base_normalize >/dev/null; then
  for in in 'https://h/x' 'https://h/x/' 'https://h/x//' 'https://h/x///' 'https://h/' 'https://h//' 'file:///a/b//' 'http://127.0.0.1:1/r/s/' '/' '//' ''; do
    a="$(base_normalize "$in")"; b="$(release_base_normalize "$in")"
    [[ "$a" == "$b" ]] || fail "'$in': the bootstrap gives '$a', install.sh gives '$b'"
  done
  [[ "$(release_base_normalize 'https://h/x//')" == 'https://h/x' ]] || fail "trailing slashes were not all removed"
fi
end_case

# --- the tally ---------------------------------------------------------------------------

printf 'get-planar tests: %s passed, %s failed\n' "$PASSED" "$FAILED"
[[ "$FAILED" -eq 0 ]] || exit 1
[[ "$PASSED" -gt 0 ]] || exit 1
exit 0
