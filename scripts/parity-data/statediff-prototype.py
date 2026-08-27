#!/usr/bin/env python3
"""Prototype state differential: run identical argv against both binaries in
pinned scratch arenas, then diff EVERY table of the two SQLite databases.

Reconnaissance for plan 996: the existing parity lane compares only exit
code / stdout / stderr, and 9 of the 10 divergences found this milestone
lived in database state instead.
"""
import os, re, shutil, sqlite3, subprocess, sys, tempfile, json

R = "/Users/mn/projects/github/rdrsss/planar"
CPP = f"{R}/build/debug/bin/planar"
ZIG = f"{R}/zig/zig-out/bin/planar"


def arena(base, name):
    root = os.path.join(base, name)
    for sub in ("home", "proj", "fakehome", "localhome"):
        os.makedirs(os.path.join(root, sub), exist_ok=True)
    return root


def env_for(root):
    return dict(
        os.environ,
        PLANAR_DB=os.path.join(root, "planar.db"),
        PLANAR_HOME=os.path.join(root, "home"),
        PLANAR_CONFIG_PATH=os.path.join(root, "config.toml"),
        PLANAR_LOCAL_HOME=os.path.join(root, "localhome"),
        HOME=os.path.join(root, "fakehome"),
        PWD=os.path.join(root, "proj"),
        PLANAR_DISABLE_WORKTREE_GATE="1",
    )


def run(binary, root, args):
    return subprocess.run(
        [binary, *args], env=env_for(root), cwd=os.path.join(root, "proj"),
        capture_output=True, text=True)


def dump(dbpath):
    """Every user table -> list of row dicts, deterministically ordered."""
    if not os.path.exists(dbpath):
        return {}
    con = sqlite3.connect(f"file:{dbpath}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    tables = [r[0] for r in con.execute(
        "select name from sqlite_master where type='table' "
        "and name not like 'sqlite_%' order by name")]
    out = {}
    for t in tables:
        try:
            cols = [r[1] for r in con.execute(f"pragma table_info({t})")]
            order = ", ".join(f'"{c}"' for c in cols)
            rows = [dict(r) for r in con.execute(f'select * from "{t}" order by {order}')]
            out[t] = rows
        except sqlite3.Error as e:
            out[t] = f"<error {e}>"
    con.close()
    return out


# Columns whose values legitimately cannot agree across two processes.
VOLATILE_EXACT = {"execution_time_ms"}


def is_volatile(col):
    """A column whose value cannot agree across two separate processes.

    Every wall-clock stamp in this schema is spelled `<something>_at`, so the
    suffix rule covers them without a hand-maintained list that silently goes
    stale as migrations add columns. `checksum` is deliberately NOT volatile:
    migrations are shared files, so a checksum divergence is a real finding.
    """
    return col in VOLATILE_EXACT or col.endswith("_at")


def normalize(rows, roots):
    """Blank volatile columns; rewrite arena-absolute paths to a placeholder."""
    if not isinstance(rows, list):
        return rows
    out = []
    for r in rows:
        n = {}
        for k, v in r.items():
            if is_volatile(k):
                n[k] = "<volatile>" if v is not None else None
                continue
            if isinstance(v, str):
                for root in roots:
                    v = v.replace(root, "<ARENA>")
            n[k] = v
        out.append(n)
    return out


TS = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z?")


def scrub(text, root):
    """Remove the two things that legitimately differ between two runs."""
    return TS.sub("<TS>", text.replace(root, "<ARENA>"))


def diff_state(cpp_root, zig_root, label):
    a = dump(os.path.join(cpp_root, "planar.db"))
    b = dump(os.path.join(zig_root, "planar.db"))
    findings = []
    for t in sorted(set(a) | set(b)):
        ra = normalize(a.get(t, []), [cpp_root, zig_root])
        rb = normalize(b.get(t, []), [cpp_root, zig_root])
        if ra != rb:
            findings.append((t, ra, rb))
    return findings


def main():
    steps = json.load(open(sys.argv[1])) if len(sys.argv) > 1 else DEFAULT_STEPS
    base = tempfile.mkdtemp(prefix="planar_statediff_")
    cpp_root, zig_root = arena(base, "cpp"), arena(base, "zig")
    total = 0
    unported = []
    seen = set()
    id_shifted = {}
    for i, args in enumerate(steps):
        rc = run(CPP, cpp_root, args)
        rz = run(ZIG, zig_root, args)
        tag = f"[{i}] planar {' '.join(args)}"
        obs = []
        if rc.returncode == 64 and "not implemented in this build" in rc.stderr:
            unported.append(tag)
            continue
        if rc.returncode != rz.returncode:
            obs.append(f"    EXIT   cpp={rc.returncode} zig={rz.returncode}")
        oc = scrub(rc.stdout, cpp_root)
        oz = scrub(rz.stdout, zig_root)
        if oc != oz:
            obs.append(f"    STDOUT cpp={oc[:200]!r}\n           zig={oz[:200]!r}")
        ec = scrub(rc.stderr, cpp_root)
        ez = scrub(rz.stderr, zig_root)
        if ec != ez:
            obs.append(f"    STDERR cpp={ec[:200]!r}\n           zig={ez[:200]!r}")
        state = diff_state(cpp_root, zig_root, tag)
        for t, ra, rb in state:
            key = lambda rows: [json.dumps(r, sort_keys=True) for r in rows]
            ka, kb = key(ra), key(rb)
            only_cpp = [r for r in ka if r not in kb]
            only_zig = [r for r in kb if r not in ka]
            fresh_cpp = [r for r in only_cpp if (t, "cpp", r) not in seen]
            fresh_zig = [r for r in only_zig if (t, "zig", r) not in seen]
            for r in fresh_cpp:
                seen.add((t, "cpp", r))
            for r in fresh_zig:
                seen.add((t, "zig", r))
            if not fresh_cpp and not fresh_zig:
                continue

            def drop_id(row):
                d = json.loads(row)
                d.pop("id", None)
                return json.dumps(d, sort_keys=True)

            zig_by_content = {}
            for r in fresh_zig:
                zig_by_content.setdefault(drop_id(r), []).append(r)
            shifted, real_cpp = 0, []
            for r in fresh_cpp:
                bucket = zig_by_content.get(drop_id(r))
                if bucket:
                    bucket.pop()
                    shifted += 1
                else:
                    real_cpp.append(r)
            real_zig = [r for rows in zig_by_content.values() for r in rows]
            if not real_cpp and not real_zig:
                if shifted:
                    id_shifted[t] = id_shifted.get(t, 0) + shifted
                continue
            only_cpp, only_zig = real_cpp, real_zig
            if not only_cpp and not only_zig:
                continue
            obs.append(f"    STATE  table={t}  (+{len(only_cpp)} cpp-only / +{len(only_zig)} zig-only)")
            for r in only_cpp[:4]:
                obs.append(f"           cpp-only: {r[:260]}")
            for r in only_zig[:4]:
                obs.append(f"           zig-only: {r[:260]}")
        if obs:
            total += 1
            print(tag)
            print("\n".join(obs))
    print(f"\n{total} of {len(steps)} steps diverged "
          f"({len(unported)} skipped as unported).   arena={base}")
    for t, n in sorted(id_shifted.items()):
        print(f"  id-shift echo: {t}: {n} rows differ only by id "
              f"(downstream of an earlier finding, not separate findings)")
    for tag in unported:
        print(f"  unported: {tag}")


DEFAULT_STEPS = [
    ["init", "--skip-project", "--allow-no-repo", "--json"],
]

if __name__ == "__main__":
    main()
