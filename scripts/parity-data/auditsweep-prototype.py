#!/usr/bin/env python3
"""Audit-row emission sweep.

The whole-table differential found that the C++ tree's divergences cluster in
audit_log. That table is the WORST case for a whole-table diff, because one
missing row shifts every later autoincrement id and buries the cause under its
own echoes.

So compare per-step DELTAS instead: what rows did THIS step add on each side,
compared by content with `id` and `recorded_at` dropped. Immune to the shift,
and it names the offending verb directly instead of the step after it.
"""
import json, os, sqlite3, subprocess, sys, tempfile

R = "/Users/mn/projects/github/rdrsss/planar"
CPP = "/tmp/planar-cpp-snapshot"
ZIG = f"{R}/zig/zig-out/bin/planar"


def mk(base, name):
    r = os.path.join(base, name)
    for s in ("home", "proj", "fakehome", "localhome"):
        os.makedirs(os.path.join(r, s), exist_ok=True)
    return r


def env(r):
    return dict(os.environ,
                PLANAR_DB=os.path.join(r, "planar.db"),
                PLANAR_HOME=os.path.join(r, "home"),
                PLANAR_CONFIG_PATH=os.path.join(r, "config.toml"),
                PLANAR_LOCAL_HOME=os.path.join(r, "localhome"),
                HOME=os.path.join(r, "fakehome"),
                PWD=os.path.join(r, "proj"))


def run(b, r, a):
    return subprocess.run([b, *a], env=env(r), cwd=os.path.join(r, "proj"),
                          capture_output=True, text=True)


def audit(r):
    """Every audit row as content-only text, in insertion order."""
    p = os.path.join(r, "planar.db")
    if not os.path.exists(p):
        return []
    con = sqlite3.connect(f"file:{p}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    try:
        rows = [dict(x) for x in con.execute("select * from audit_log order by id")]
    except sqlite3.Error:
        return []
    finally:
        con.close()
    out = []
    for x in rows:
        x.pop("id", None)
        x.pop("recorded_at", None)
        out.append(json.dumps(x, sort_keys=True))
    return out


def main():
    steps = json.load(open(sys.argv[1]))
    base = tempfile.mkdtemp(prefix="planar_auditsweep_")
    c, z = mk(base, "cpp"), mk(base, "zig")
    prev_c, prev_z = [], []
    findings = 0
    agreed = 0
    for args in steps:
        rc, rz = run(CPP, c, args), run(ZIG, z, args)
        now_c, now_z = audit(c), audit(z)
        # Rows this step appended, on each side.
        dc, dz = now_c[len(prev_c):], now_z[len(prev_z):]
        prev_c, prev_z = now_c, now_z
        if rc.returncode == 64 and "not implemented in this build" in rc.stderr:
            continue
        if sorted(dc) == sorted(dz):
            if dc:
                agreed += 1
            continue
        findings += 1
        print(f"planar {' '.join(args)}")
        print(f"    cpp emitted {len(dc)} row(s), zig emitted {len(dz)}")
        for r in dz:
            if r not in dc:
                print(f"      MISSING in cpp: {r}")
        for r in dc:
            if r not in dz:
                print(f"      EXTRA   in cpp: {r}")
    print(f"\n{findings} verb(s) disagree on audit emission; "
          f"{agreed} verb(s) emit identical rows.   arena={base}")


if __name__ == "__main__":
    main()
