# shellcheck shell=bash
#
# prebuilt-bundle.sh -- test-only fixture: a release bundle directory built from
# the repository with five tiny executables in bin/, laid out as scripts/dist.sh
# lays out a real one (tech spec 677, "The bundle"). Sourced by
# scripts/install-prebuilt-test.sh and scripts/install-data-paths-test.sh.
#
# The fake binaries answer what the installer asks of them:
#   <bin> version                  the six-token version line
#   planar-agent queue status ...  the contents of $PLANAR_DB's directory's
#                                  stub.out and exit status of stub.rc when they
#                                  exist, else {"seq":1} at exit 0
#   anything else                  exit 0
# python3 is used here to render the Codex agents; that is test tooling.

FAKE_BUNDLE_VERSION_TAG="v1.2.3"

# fake_bundle_make ROOT DEST [CLEANUP_FILE] -- build the bundle at DEST from the
# repository at ROOT. CLEANUP_FILE replaces install-cleanup.txt.
fake_bundle_make() {
  local root="$1" dest="$2" cleanup="${3:-$1/install-cleanup.txt}" b
  mkdir -p "$dest/bin" "$dest/skills" "$dest/scripts" "$dest/templates" "$dest/workflows"
  cp "$root/install.sh" "$dest/install.sh"
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
    cat > "$dest/bin/$b" <<STUB
#!/bin/bash
if [ "\${1:-}" = version ]; then echo "$b abc123def456 2026-10-06T00:00:00Z cxx Clang-23.1.2 $FAKE_BUNDLE_VERSION_TAG"; exit 0; fi
if [ "$b" = planar-agent ] && [ "\${1:-}" = queue ]; then
  d="\$(dirname "\${PLANAR_DB:-/nonexistent/x}")"
  if [ -f "\$d/stub.out" ]; then cat "\$d/stub.out"; exit "\$(cat "\$d/stub.rc" 2>/dev/null || echo 0)"; fi
  echo '{"seq":1}'
fi
exit 0
STUB
    chmod 755 "$dest/bin/$b"
  done
  printf '{\n  "version": "%s",\n  "sha": "%s",\n  "date": "2026-10-06T00:00:00Z",\n  "os": "macos",\n  "arch": "arm64",\n  "os_floor": "26.0",\n  "schema_version": 41\n}\n' \
    "$FAKE_BUNDLE_VERSION_TAG" "0123456789012345678901234567890123456789" > "$dest/release.json"
}
