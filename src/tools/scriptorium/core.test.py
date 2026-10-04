"""End-to-end parity and drift tests for the in-tree Scriptorium."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

binary, repo, manifest = map(Path, sys.argv[1:])
expected = {}
for line in manifest.read_text().splitlines():
    digest, rel = line.split("  ", 1)
    expected[rel] = digest
assert len(expected) == 232

def run(*args, code=0):
    p = subprocess.run([str(binary), *map(str, args)], text=True, capture_output=True)
    assert p.returncode == code, (p.args, p.returncode, p.stdout, p.stderr)
    return p

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    config = repo / "scriptorium.yaml"
    run("render", "--config", config, "--output-root", root)
    import json
    preview = json.loads(run("render", "--config", config, "--output-root", root,
                             "--dry-run", "--json").stdout)
    assert len(preview["written"]) == 232 and len(preview["skipped"]) == 4
    actual = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in root.rglob("*") if p.is_file()}
    assert actual == expected, sorted(k for k in expected if actual.get(k) != expected[k])[:10]
    run("check", "--config", config, "--output-root", root)
    run("status", "--config", config, "--output-root", root, "--json")

    victim = root / "skills/codex/pl-health/SKILL.md"
    victim.write_text(victim.read_text() + "changed\n")
    assert "changed: skills/codex/pl-health/SKILL.md" in run(
        "check", "--config", config, "--output-root", root, code=1).stdout
    changed_report = json.loads(run("check", "--config", config, "--output-root", root,
                                    "--json", code=1).stdout)
    assert "skills/codex/pl-health/SKILL.md" in changed_report["changed"]
    run("render", "--config", config, "--output-root", root)
    assert hashlib.sha256(victim.read_bytes()).hexdigest() == expected[str(victim.relative_to(root))]
    victim.unlink()
    assert "missing: skills/codex/pl-health/SKILL.md" in run(
        "check", "--config", config, "--output-root", root, code=1).stdout

    foreign = root / "skills/codex/foreign/SKILL.md"
    foreign.parent.mkdir()
    foreign.write_text("foreign\n")
    assert "unexpected: skills/codex/foreign/SKILL.md" in run(
        "check", "--config", config, "--output-root", root, code=1).stdout
    report = json.loads(run("status", "--config", config, "--output-root", root,
                            "--json", code=1).stdout)
    assert any(row["slug"] == "foreign" and row["orphaned"] for row in report["artifacts"])

    bad = root / "bad"
    bad.mkdir()
    (bad / "bad.md").write_text("---\nslug: bad\ndescription: bad\n---\n\n{{.VendorTitle}}\n")
    untouched = root / "untouched"
    run("render", "--config", config, "--source", bad, "--output-root", untouched, code=2)
    assert not untouched.exists()
    (bad / "bad.md").write_text("---\nslug: bad\ndescription: bad\n---\n\n{{ Missing }}\n")
    run("render", "--config", config, "--source", bad, "--output-root", untouched, code=2)
    assert not untouched.exists()
    (bad / "bad.md").write_text("---\nslug: bad\nslug: other\ndescription: bad\n---\n\nbody\n")
    assert "duplicate frontmatter key" in run(
        "render", "--config", config, "--source", bad, "--output-root", untouched, code=2).stderr
    assert not untouched.exists()

    (bad / "bad.md").write_text("---\nslug: bad\ndescription: bad\n---\n\n# {{ VendorTitle }}\n")
    run("render", "--config", config, "--source", bad, "--vendor", "claude",
        "--output-root", untouched)
    staged = untouched / "commands/claude/bad.md"
    installed = root / "installed-bad.md"
    installed.write_bytes(staged.read_bytes())
    manifest_file = root / "install-manifest.json"
    manifest_file.write_text(json.dumps({"projections": [{"vendor": "claude", "kind": "skill",
        "name": "bad", "staged_path": str(staged), "installed_path": str(installed)}]}))
    report = json.loads(run("status", "--config", config, "--source", bad,
        "--vendor", "claude", "--output-root", untouched,
        "--install-manifest", manifest_file, "--json").stdout)
    assert report["artifacts"][0]["installed"]
    installed.write_text("drifted")
    report = json.loads(run("status", "--config", config, "--source", bad,
        "--vendor", "claude", "--output-root", untouched,
        "--install-manifest", manifest_file, "--json", code=1).stdout)
    assert report["artifacts"][0]["drifted"]

    (bad / "bad.md").write_text("---\nslug: bad\nkind: doc\ndescription: bad\n---\n\n# Companion\n")
    doc_stage = root / "doc-stage"
    run("render", "--config", config, "--source", bad, "--vendor", "claude",
        "--output-root", doc_stage)
    staged_doc = doc_stage / "agents/claude/bad.md"
    installed.write_bytes(staged_doc.read_bytes())
    manifest_file.write_text(json.dumps({"projections": [{"vendor": "claude", "kind": "agent",
        "name": "bad", "staged_path": str(staged_doc), "installed_path": str(installed)}]}))
    report = json.loads(run("status", "--config", config, "--source", bad,
        "--vendor", "claude", "--output-root", doc_stage,
        "--install-manifest", manifest_file, "--json").stdout)
    assert report["artifacts"][0]["kind"] == "doc" and report["artifacts"][0]["installed"]

    # Namespaced agent form: name + description on top, kind + slug under `planar`.
    nested = root / "nested-out"
    (bad / "bad.md").write_text(
        "---\nname: planar-bad\ndescription: bad\nplanar:\n  kind: agent\n  slug: planar-bad\n---\n\n# Body\n")
    run("render", "--config", config, "--source", bad, "--vendor", "claude", "--output-root", nested)
    assert (nested / "agents/claude/planar-bad.md").read_text().startswith('---\nname: "planar-bad"')
    (bad / "bad.md").write_text(
        "---\nname: planar-bad\ndescription: bad\nplanar:\n  kind: agent\n  slug: bad\n---\n\nbody\n")
    err = run("render", "--config", config, "--source", bad, "--output-root", root / "n2", code=2).stderr
    assert "name `planar-bad` differs from planar.slug `bad`" in err, err
    (bad / "bad.md").write_text(
        "---\nname: planar-bad\ndescription: bad\nkind: agent\nplanar:\n  kind: agent\n  slug: planar-bad\n---\n\nbody\n")
    err = run("render", "--config", config, "--source", bad, "--output-root", root / "n3", code=2).stderr
    assert "top-level `kind` beside a planar map" in err, err
print("scriptorium: 232 byte-pinned projections and drift/error cases passed")
