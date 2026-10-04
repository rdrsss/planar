#!/usr/bin/env python3
"""Render Planar agents as Codex custom-agent TOML files.

Usage: render-codex-agents.py <agents-dir> <out-dir>

Reads every ``*.md`` directly under <agents-dir> whose YAML frontmatter has a
``planar:`` map with ``kind: agent`` and writes ``<out-dir>/<name>.toml`` with
exactly three keys, in order: ``name``, ``description`` and
``developer_instructions`` (the Markdown after the closing ``---`` with at
most one leading blank line stripped). Files without a ``planar`` map or with
another ``planar.kind`` are skipped silently.

Every other frontmatter key (``model``, ``codex``, ``tools``, ...) is IGNORED
on purpose: Codex documents only name, description and developer_instructions.

All values are TOML basic strings (double-quoted, one line, fully escaped), so
triple quotes, trailing backslashes and non-ASCII text round-trip exactly.

Exit status: 0 success (one line printed per file written); 1 malformed input
(offending file named on stderr, nothing written to <out-dir>); 2 usage error.
Python 3.10+, standard library only.
"""
import os
import shutil
import sys
import tempfile

ESC = {'\\': '\\\\', '"': '\\"', '\n': '\\n', '\r': '\\r', '\t': '\\t',
       '\b': '\\b', '\f': '\\f'}


class Malformed(Exception):
    pass


def toml_str(text):
    out = []
    for ch in text:
        if ch in ESC:
            out.append(ESC[ch])
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append('\\u%04X' % ord(ch))
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def unquote(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
        return v[1:-1]
    return v


def parse(path):
    """Return (fm, body); fm has top-level scalars and a 'planar' dict."""
    with open(path, encoding='utf-8', newline='') as f:
        text = f.read()
    lines = text.split('\n')
    if lines[0].rstrip('\r') != '---':
        raise Malformed('missing frontmatter')
    end = next((i for i in range(1, len(lines))
                if lines[i].rstrip('\r') == '---'), None)
    if end is None:
        raise Malformed('unterminated frontmatter')
    fm, sub = {}, None
    for line in lines[1:end]:
        line = line.rstrip('\r')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        if line[0] in ' \t':
            if sub is not None and ':' in line:
                k, v = line.split(':', 1)
                sub[k.strip()] = unquote(v)
            continue
        if ':' not in line:
            continue
        k, v = line.split(':', 1)
        k, v = k.strip(), v.strip()
        if k == 'planar' and not v:
            sub = fm['planar'] = {}
        else:
            sub = None
            fm[k] = unquote(v)
    body = '\n'.join(lines[end + 1:])
    if body.startswith('\r\n'):
        body = body[2:]
    elif body.startswith('\n'):
        body = body[1:]
    return fm, body


def render(path):
    fm, body = parse(path)
    meta = fm.get('planar')
    if not isinstance(meta, dict) or meta.get('kind') != 'agent':
        return None
    name, desc = fm.get('name'), fm.get('description')
    if not name or not desc:
        raise Malformed('frontmatter lacks name or description')
    stem = os.path.splitext(os.path.basename(path))[0]
    if name != meta.get('slug') or name != stem:
        raise Malformed('name %r differs from planar.slug %r or file stem %r'
                        % (name, meta.get('slug'), stem))
    return name, ('name = %s\ndescription = %s\ndeveloper_instructions = %s\n'
                  % (toml_str(name), toml_str(desc), toml_str(body)))


def main(argv):
    if len(argv) != 3:
        print(__doc__.split('\n')[2], file=sys.stderr)
        return 2
    src, dst = argv[1], argv[2]
    if not os.path.isdir(src):
        print('not a directory: %s' % src, file=sys.stderr)
        return 2
    os.makedirs(dst, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=dst) as tmp:
        names = []
        for fn in sorted(os.listdir(src)):
            path = os.path.join(src, fn)
            if not fn.endswith('.md') or not os.path.isfile(path):
                continue
            try:
                res = render(path)
            except (Malformed, OSError, UnicodeDecodeError) as e:
                print('%s: %s' % (path, e), file=sys.stderr)
                return 1
            if res:
                with open(os.path.join(tmp, res[0] + '.toml'), 'w',
                          encoding='utf-8', newline='') as f:
                    f.write(res[1])
                names.append(res[0])
        for n in names:
            final = os.path.join(dst, n + '.toml')
            shutil.move(os.path.join(tmp, n + '.toml'), final)
            print(final)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
