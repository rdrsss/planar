#!/usr/bin/env python3
"""Build (or check) a deterministic pinned-archive vendoring of a lifecycle
eval fixture repository tree (task 6858, Planar plan 1069 M3).

Planar's own vendoring rule (root CLAUDE.md "Vendoring rule") requires a
pinned release archive plus a SHA256 digest, never a bare git checkout, for
anything the runtime or its evals depend on. `evals/orchestrator/harness.py`
applies the same rule to a lifecycle fixture whose repository conventions
are deliberately unlike Planar's own (the `foreign-flat` fixture): the
AUTHORED source tree lives at `evals/fixtures/<name>.src/repo/`, and this
script turns it into the pinned pair the harness actually reads at prepare
time --

    evals/fixtures/<name>.tar.gz    (deterministic tar+gzip of repo/)
    evals/fixtures/<name>.sha256    (sha256sum-format digest of the .tar.gz)

Swapping the local archive this script writes for a pinned remote release
URL later is a one-line change to how `harness.verify_and_extract_vendored_fixture`
resolves `archive_path` -- fetch-then-verify instead of read-then-verify --
this script and the digest format it writes stay the same either way.

Usage:
    vendor_archive.py <name>            rebuild <name>.tar.gz/.sha256 from
                                         <name>.src/repo/
    vendor_archive.py <name> --check    rebuild into a scratch location and
                                         require it to be byte-identical to
                                         the committed archive, WITHOUT
                                         touching the committed files -- this
                                         is the fixture-freshness check
                                         (an edit to <name>.src/ that forgets
                                         to re-run this script fails here).
"""
from __future__ import annotations

import gzip
import hashlib
import io
import sys
import tarfile
from pathlib import Path

FIXTURES_DIR = Path(__file__).resolve().parent


def _deterministic_filter(tarinfo: tarfile.TarInfo) -> tarfile.TarInfo:
    """Strip every host/run-specific field a tar entry can carry so the
    same source tree always produces byte-identical archive bytes,
    regardless of which machine or timestamp built it.
    """
    tarinfo.uid = 0
    tarinfo.gid = 0
    tarinfo.uname = ""
    tarinfo.gname = ""
    tarinfo.mtime = 0
    return tarinfo


def build_tar_bytes(src_repo: Path) -> bytes:
    """Deterministically tar `src_repo` under a single top-level `repo/`
    member (so extraction always yields `<dest>/repo/...`), in sorted path
    order so directory-iteration order never affects the output.
    """
    if not src_repo.is_dir():
        raise SystemExit(f"vendor_archive: missing source tree: {src_repo}")
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        tar.add(
            src_repo,
            arcname="repo",
            recursive=False,
            filter=_deterministic_filter,
        )
        for path in sorted(src_repo.rglob("*")):
            parts = path.relative_to(src_repo).parts
            # Never vendor a bytecode cache: it is a build artifact of
            # running the fixture's own tests while authoring it, not part
            # of the authored source tree, and its contents are neither
            # deterministic nor meaningful to commit.
            if "__pycache__" in parts:
                continue
            arcname = "repo/" + "/".join(parts)
            tar.add(
                path,
                arcname=arcname,
                recursive=False,
                filter=_deterministic_filter,
            )
    return buffer.getvalue()


def gzip_bytes(data: bytes) -> bytes:
    """gzip with a fixed mtime and no embedded filename -- gzip's own
    header otherwise carries a timestamp, which would make the compressed
    output (and therefore its SHA256) different on every rebuild even
    though the decompressed tar bytes are identical.
    """
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0, filename="") as handle:
        handle.write(data)
    return out.getvalue()


def build_archive(name: str) -> tuple[bytes, str]:
    src_repo = FIXTURES_DIR / f"{name}.src" / "repo"
    archive_bytes = gzip_bytes(build_tar_bytes(src_repo))
    digest = hashlib.sha256(archive_bytes).hexdigest()
    return archive_bytes, digest


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3) or (len(argv) == 3 and argv[2] != "--check"):
        sys.stderr.write(__doc__ or "")
        return 2
    name = argv[1]
    check = len(argv) == 3
    archive_bytes, digest = build_archive(name)
    archive_path = FIXTURES_DIR / f"{name}.tar.gz"
    digest_path = FIXTURES_DIR / f"{name}.sha256"
    digest_line = f"{digest}  {name}.tar.gz\n"
    if check:
        if not archive_path.is_file() or not digest_path.is_file():
            sys.stderr.write(
                f"vendor_archive --check: missing committed archive or digest "
                f"for {name!r}\n"
            )
            return 1
        committed_bytes = archive_path.read_bytes()
        committed_digest_line = digest_path.read_text(encoding="utf-8")
        if committed_bytes != archive_bytes:
            sys.stderr.write(
                f"vendor_archive --check: {name}.tar.gz is stale -- rebuild it "
                f"from {name}.src/repo/ with `python3 evals/fixtures/"
                f"vendor_archive.py {name}` and commit the result\n"
            )
            return 1
        if committed_digest_line != digest_line:
            sys.stderr.write(
                f"vendor_archive --check: {name}.sha256 does not match a fresh "
                f"digest of {name}.tar.gz\n"
            )
            return 1
        print(f"vendor_archive --check: {name} is fresh ({digest})")
        return 0
    archive_path.write_bytes(archive_bytes)
    digest_path.write_text(digest_line, encoding="utf-8")
    print(f"vendor_archive: wrote {archive_path.name} and {digest_path.name} ({digest})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
