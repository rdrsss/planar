#!/usr/bin/env python3
"""Verify every candidate in agents/models.md §Candidate Presets can be spawned.

§Candidate Presets is hand-authored with no generator and no probe step. A
listed candidate is therefore a CLAIM, not a verified fact — the file says so
itself — and it goes stale silently when a vendor renames or retires a model.
The failure surfaces only when a real dispatch tries to spawn something that no
longer exists.

This reads the table (never a duplicated list, so the eval fails when the TABLE
is wrong) and spawns each candidate once with a trivial prompt.

Opt-in by design: one live invocation per candidate. Wiring it into `make eval`
would burn rate limit on every run.

Detection only — it never rewrites the table. Repair is an operator edit,
consistent with Planar owning the catalog.

Exit codes: 0 all spawnable (or skipped), 1 at least one not spawnable,
2 usage/parse error.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MODELS_DOC = REPO_ROOT / "agents" / "models.md"

# Vendors whose CLI this eval knows how to drive. A vendor in the table but not
# here is reported as unsupported rather than silently ignored.
SPAWNERS: dict[str, list[str]] = {
    "claude": ["claude", "-p", "--model", "{candidate}", "--permission-mode", "dontAsk", "{prompt}"],
    "codex": ["codex", "exec", "--model", "{candidate}", "{prompt}"],
    "copilot": ["copilot", "-p", "{prompt}", "--model", "{candidate}"],
    # No gemini CLI on this machine. Declared anyway so its rows report
    # "gemini CLI not installed" — a skip naming the missing dependency —
    # instead of "no spawner for vendor", which reads like the eval simply
    # does not know how to check them.
    "gemini": ["gemini", "-p", "{prompt}", "--model", "{candidate}"],
}

PROMPT = "Reply with exactly: OK"

# An invalid id fails loudly and distinctly on the Claude host; codex reports a
# comparable model error. Matching the message is what separates "this id does
# not exist" from an unrelated failure (auth, network, rate limit), which must
# NOT be reported as a stale table entry.
NOT_A_MODEL = re.compile(
    r"issue with the selected model"
    r"|it may not exist or you may not have access"
    r"|model[^\n]{0,40}(not found|does not exist|unknown|unsupported|invalid|not available)"
    r"|unknown model",
    re.IGNORECASE,
)

RATE_LIMITED = re.compile(
    r"rate limit|resource_exhausted|429|quota|overloaded|usage limit",
    re.IGNORECASE,
)


def parse_candidates(doc: str) -> list[tuple[str, str]]:
    """Return [(vendor, candidate)] from the §Candidate Presets tables.

    Vendor comes from the `### <vendor>` heading that precedes each table, so a
    new vendor section is picked up without editing this parser.
    """
    out: list[tuple[str, str]] = []
    section = doc.split("## Candidate Presets", maxsplit=1)
    if len(section) != 2:
        raise SystemExit("verify: could not find '## Candidate Presets' in agents/models.md")
    body = section[1]
    # Stop at the next H2 so later sections (Work-type routes, Notes) are not scanned.
    body = re.split(r"\n## ", body, maxsplit=1)[0]

    vendor = None
    vendor_col: int | None = None
    for line in body.splitlines():
        heading = re.match(r"###\s+(.+?)\s*$", line)
        if heading:
            vendor = heading.group(1).strip()
            vendor_col = None  # each section declares its own table shape
            continue
        if vendor is None or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 2:
            continue

        # Header row: note a Vendor column if this table has one. Sections
        # differ — the claude and codex tables carry vendor in the HEADING,
        # while the combined section carries it PER ROW. Reading only the
        # heading gave those rows the vendor "copilot and gemini", which
        # matches no spawner, so every one of them skipped no matter which
        # CLIs were installed — silently defeating the promotion path that
        # section's own note tells operators to use.
        if vendor_col is None and any(c.lower() == "vendor" for c in cells):
            vendor_col = next(i for i, c in enumerate(cells) if c.lower() == "vendor")
            continue

        candidate = re.fullmatch(r"`([^`]+)`", cells[0])
        if not candidate:
            continue  # separator row

        row_vendor = vendor
        if vendor_col is not None and len(cells) > vendor_col:
            # The cell may carry a trailing note ("gemini — same ID as ...");
            # the vendor is the first token.
            raw = cells[vendor_col].split("—")[0].split("--")[0].strip()
            if raw:
                row_vendor = raw
        out.append((row_vendor, candidate.group(1)))
    return out


def spawn(vendor: str, candidate: str, timeout: int) -> tuple[str, str]:
    """Return (status, detail). status ∈ spawnable|not-spawnable|error|skipped."""
    template = SPAWNERS.get(vendor)
    if template is None:
        return "skipped", f"no spawner for vendor '{vendor}'"
    binary = template[0]
    if shutil.which(binary) is None:
        # Absent CLI is a SKIP, never a pass: saying nothing here would imply
        # the id was checked.
        return "skipped", f"{binary} CLI not installed"

    argv = [part.format(candidate=candidate, prompt=PROMPT) for part in template]
    try:
        proc = subprocess.run(
            argv,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=REPO_ROOT,
        )
    except subprocess.TimeoutExpired:
        return "error", f"timed out after {timeout}s"

    blob = f"{proc.stdout}\n{proc.stderr}"
    if NOT_A_MODEL.search(blob):
        return "not-spawnable", "host rejected the model id"
    if RATE_LIMITED.search(blob):
        # Not a table problem. Reporting this as a stale entry would send an
        # operator to edit a row that is actually fine.
        return "error", "rate limited — inconclusive, retry later"
    if proc.returncode != 0:
        first = next((l for l in blob.splitlines() if l.strip()), "")
        return "error", f"exit {proc.returncode}: {first[:120]}"
    return "spawnable", "completed normally"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="parse and print candidates; spawn nothing")
    ap.add_argument("--vendor", help="restrict to one vendor")
    ap.add_argument("--candidate", help="restrict to one candidate id (implies a single spawn)")
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    if not MODELS_DOC.is_file():
        print(f"verify: {MODELS_DOC} not found", file=sys.stderr)
        return 2
    candidates = parse_candidates(MODELS_DOC.read_text(encoding="utf-8"))
    if not candidates:
        print("verify: parsed zero candidates — the table shape changed", file=sys.stderr)
        return 2

    if args.vendor:
        candidates = [c for c in candidates if c[0] == args.vendor]
    if args.candidate:
        candidates = [c for c in candidates if c[1] == args.candidate]
    if not candidates:
        print("verify: no candidates matched the filters", file=sys.stderr)
        return 2

    if args.list:
        for vendor, candidate in candidates:
            print(f"{vendor}\t{candidate}")
        return 0

    print(f"candidate spawn verification — {len(candidates)} candidate(s) from agents/models.md\n")
    failures: list[str] = []
    errors: list[str] = []
    skipped: list[str] = []
    for vendor, candidate in candidates:
        status, detail = spawn(vendor, candidate, args.timeout)
        mark = {
            "spawnable": "PASS",
            "not-spawnable": "FAIL",
            "error": "ERROR",
            "skipped": "SKIP",
        }[status]
        print(f"  {mark:<5} {vendor:<8} {candidate:<24} {detail}")
        if status == "not-spawnable":
            failures.append(f"{vendor}/{candidate}")
        elif status == "error":
            errors.append(f"{vendor}/{candidate} ({detail})")
        elif status == "skipped":
            skipped.append(f"{vendor}/{candidate}")

    print()
    if skipped:
        print(f"skipped (not verified, NOT passing): {len(skipped)} — {', '.join(skipped)}")
    if errors:
        print(f"inconclusive: {len(errors)} — {'; '.join(errors)}")
    if failures:
        print(f"\nFAIL: {len(failures)} candidate(s) the host will not spawn: {', '.join(failures)}")
        print("agents/models.md §Candidate Presets is stale. Repair is an operator edit.")
        return 1

    verified = len(candidates) - len(skipped) - len(errors)
    if verified == 0:
        # Never print OK when nothing was actually checked. A run where every
        # candidate skipped verifies exactly nothing, and reporting success
        # would be the same "silence reads as a pass" failure this eval exists
        # to catch in the table.
        print("NOTHING VERIFIED: every candidate was skipped or inconclusive.")
        return 2
    print(f"OK: {verified} candidate(s) spawned normally.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
