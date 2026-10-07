# shellcheck shell=bash
#
# prebuilt-bundle.sh -- test-only fixture: a release bundle directory built from
# the repository with five tiny executables in bin/, laid out as scripts/dist.sh
# lays out a real one (tech spec 677, "The bundle"). Sourced by
# scripts/install-prebuilt-test.sh and scripts/install-data-paths-test.sh.
#
# The fake binaries are stub_binary_write's (below); they answer what the
# installer asks of them. In the table, D is the directory holding $PLANAR_DB:
#   <bin> version                  the six-token version line
#   <bin> version --json           the version object, with a full 40-digit sha
#   planar-watch queue --json      D/watch.out and exit status D/watch.rc when
#                                  they exist, else [] at exit 0 (the database
#                                  probe reads this as the current schema)
#   planar-agent queue ...         D/stub.out and exit status D/stub.rc when they
#                                  exist, else {"seq":1} at exit 0
#   planar init ...                appends its arguments to D/init.calls when
#                                  that file exists (a test creates it), exits
#                                  with D/init.rc when it exists, else creates a
#                                  placeholder database at $PLANAR_DB when none
#                                  is there and exits 0
#   anything else                  exit 0
# python3 is used here to render the Codex agents; that is test tooling.

FAKE_BUNDLE_VERSION_TAG="v1.2.3"

# stub_binary_write PATH TAG [BUILD] -- write one fake Planar binary at PATH whose
# version line ends in TAG and whose build id (second token) is BUILD.
stub_binary_write() {
  local path="$1" tag="$2" build="${3:-abc123def456}" name
  name="${path##*/}"
  cat > "$path" <<STUB
#!/bin/sh
d="\$(dirname "\${PLANAR_DB:-/nonexistent/x}")"
case "\${1:-}" in
  version)
    if [ "\${2:-}" = --json ]; then
      echo '{"release":"$tag","sha":"${build}0123456789abcdef0123456789ab","date":"2026-10-06T00:00:00Z","dirty":false,"compiler":"Clang-23.1.2"}'
    else
      echo "$name $build 2026-10-06T00:00:00Z cxx Clang-23.1.2 $tag"
    fi
    exit 0 ;;
  queue)
    if [ "$name" = planar-watch ]; then
      if [ -f "\$d/watch.out" ]; then cat "\$d/watch.out"; exit "\$(cat "\$d/watch.rc" 2>/dev/null || echo 0)"; fi
      echo '[]'; exit 0
    fi
    if [ "$name" = planar-agent ]; then
      if [ -f "\$d/stub.out" ]; then cat "\$d/stub.out"; exit "\$(cat "\$d/stub.rc" 2>/dev/null || echo 0)"; fi
      echo '{"seq":1}'; exit 0
    fi
    exit 0 ;;
  init)
    if [ "$name" = planar ]; then
      if [ -e "\$d/init.calls" ]; then echo "\$*" >> "\$d/init.calls"; fi
      if [ -f "\$d/init.rc" ]; then exit "\$(cat "\$d/init.rc")"; fi
      if [ -n "\${PLANAR_DB:-}" ] && [ ! -e "\$PLANAR_DB" ]; then printf 'SQLite format 3 (stub)\\n' > "\$PLANAR_DB"; fi
    fi
    exit 0 ;;
esac
exit 0
STUB
  chmod 755 "$path"
}

# fake_bundle_make ROOT DEST [CLEANUP_FILE] -- build the bundle at DEST from the
# repository at ROOT. CLEANUP_FILE replaces install-cleanup.txt.
fake_bundle_make() {
  local root="$1" dest="$2" cleanup="${3:-$1/install-cleanup.txt}" b
  mkdir -p "$dest/bin" "$dest/skills" "$dest/scripts" "$dest/templates" "$dest/workflows"
  cp "$root/install.sh" "$dest/install.sh"
  cp "$root/scripts/uninstall.sh" "$dest/uninstall.sh"
  cp "$cleanup" "$dest/install-cleanup.txt"
  cp -R "$root/scripts/install-lib" "$dest/scripts/install-lib"
  rm -rf "$dest/scripts/install-lib/__pycache__"
  cp -R "$root/skills/planar" "$dest/skills/planar"
  cp -R "$root/agents" "$dest/agents"
  cp -R "$root/migrations" "$dest/migrations"
  printf 'shipped a\n' > "$dest/templates/a.toml"
  printf 'shipped b\n' > "$dest/templates/b.toml"
  printf -- '-- workflow\n' > "$dest/workflows/w.lua"
  python3 "$root/scripts/render-codex-agents.py" "$dest/agents" "$dest/codex-agents" >/dev/null
  for b in planar planar-agent planar-watch planar-execute planar-ext; do
    stub_binary_write "$dest/bin/$b" "$FAKE_BUNDLE_VERSION_TAG"
  done
  printf '{\n  "version": "%s",\n  "sha": "%s",\n  "date": "2026-10-06T00:00:00Z",\n  "os": "macos",\n  "arch": "arm64",\n  "os_floor": "26.0",\n  "schema_version": 41\n}\n' \
    "$FAKE_BUNDLE_VERSION_TAG" "0123456789012345678901234567890123456789" > "$dest/release.json"
}
