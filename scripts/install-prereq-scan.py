#!/usr/bin/env python3
"""install-prereq-scan.py -- find the programs the operator path runs.

Used by scripts/install-prereq-test.sh.

  install-prereq-scan.py update DIR      the programs `planar update` spawns, from the
                                         non-test C++ sources in DIR; and every spawn
                                         call site, as "site NAME COUNT" lines
  install-prereq-scan.py bootstrap FILE  the command-position words of a POSIX sh
                                         script, as "cmd NAME" lines
  install-prereq-scan.py --self-test     check the sh scanner on fixed inputs

update. Output lines: "program NAME" for a program a spawn site names (a literal
first argument, an env-wrapped argv head, or a resolve_program literal), and
"site API COUNT" for every spawn API found, counted. The caller pins the sites;
an API that is not pinned, or a count that moved, fails until reviewed. The
spawn APIs are every way the process library and libc start a program:
process::capture, run_inherited, runner::start, execv*, posix_spawn*, fork,
popen, system. Calls are found however the namespace is spelled (qualified, aliased,
or bare after a using-declaration).

bootstrap. A small lexer, not a shell: it follows quotes, `$(...)`, `${...}`,
`$((...))`, `( )` subshells, `case` patterns, here-documents (an unquoted one is expanded, so its `$(...)` and backticks
are scanned; a quoted one is skipped) and comments, and
reports the word in each command position (line start, after `|` `&&` `||` `;`
`&` `(` `$(` `then` `do` `else` `elif` `if` `while` `until` `!` `{`, after
leading assignments, and after the command wrappers `command`, `exec`, `env`,
`nohup`, `xargs`, `time`, `nice`, `sudo`). Keywords are dropped. Words built
from a variable or a quote are not names and are dropped. Builtins and the
script's own functions are the caller's to filter. An unsupported construct
(a backtick outside a here-document) stops the scan loudly.
"""
import glob
import re
import sys

KEYWORDS_KEEP = {"if", "then", "else", "elif", "while", "until", "do", "!", "{", "time"}
KEYWORDS_END = {"fi", "done", "}", "for", "in"}
WRAPPERS = {"command", "exec", "env", "nohup", "xargs", "nice", "sudo", "time"}
ASSIGN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


class Frame:
    """One command context: the top level, `$( )`, or a `( )` subshell."""

    def __init__(self, kind):
        self.kind = kind          # "top" | "sub"
        self.at_cmd = True
        self.cases = []           # one bool per open case: True while reading patterns
        self.pending_case = False
        self.wrapped = False
        self.skip_redir = False
        self.saved = None


def heredoc_substitutions(body):
    """The command names run by $(...) and `...` in the body of an unquoted here-document."""
    names = []
    i, n = 0, len(body)
    while i < n:
        c = body[i]
        if c == "\\":
            i += 2
        elif body.startswith("$((", i):
            depth, i = 0, i + 3
            while i < n and not (depth == 0 and body[i] == ")"):
                depth += {"(": 1, ")": -1}.get(body[i], 0)
                i += 1
            i += 2
        elif body.startswith("$(", i):
            depth, j, quote = 0, i + 2, ""
            while j < n:
                ch = body[j]
                if quote:
                    quote = "" if ch == quote else quote
                elif ch in "'\"":
                    quote = ch
                elif ch == "(":
                    depth += 1
                elif ch == ")":
                    if depth == 0:
                        break
                    depth -= 1
                j += 1
            names.extend(scan_sh(body[i + 2:j]))
            i = j + 1
        elif c == "`":
            j = i + 1
            while j < n and body[j] != "`":
                j += 2 if body[j] == "\\" else 1
            inner = re.sub(r"\\([$`\\])", r"\1", body[i + 1:j])
            names.extend(scan_sh(inner))
            i = j + 1
        else:
            i += 1
    return names


def scan_sh(text):
    names = []
    stack = [Frame("top")]        # entries are Frame, or a str for dq / brace / arith / arith:N
    heredocs = []                 # delimiters waiting for the next newline
    i, n = 0, len(text)
    word, raw, dynamic = [], [], False
    in_word = False

    def finish_word():
        nonlocal word, raw, dynamic, in_word
        if not in_word:
            return
        w, r, dyn = "".join(word), "".join(raw), dynamic
        word, raw, dynamic, in_word = [], [], False, False
        f = stack[-1]
        if not isinstance(f, Frame):
            return
        m = re.match(r"^\d*<<-?(.*)$", r)
        if m and not r.startswith("<<<"):
            delim = m.group(1)
            if delim == "":
                f.skip_redir = True
                heredocs.append(["", True])      # delimiter arrives with the next word
            else:
                heredocs.append([delim.strip("'\"\\"), not re.search(r"['\"\\]", delim)])
            return
        if heredocs and heredocs[-1][0] == "":
            heredocs[-1] = [r.strip("'\"\\"), not re.search(r"['\"\\]", r)]
            f.skip_redir = False
            return
        if f.skip_redir:
            f.skip_redir = False
            return
        if f.pending_case:
            if w == "in" and not dyn:
                f.pending_case = False
                f.cases.append(True)
                f.at_cmd = False
            return
        if f.cases and f.cases[-1]:
            if w == "esac" and not dyn:
                f.cases.pop()
                f.at_cmd = False
            return
        if re.match(r"^\d*[<>]", r):
            if re.match(r"^\d*(<|>|>>|<&|>&|<>)$", r):
                f.skip_redir = True
            return
        if not f.at_cmd:
            return
        if ASSIGN.match(r):
            return
        if dyn or not w:
            f.at_cmd = False
            return
        if f.wrapped and w.startswith("-"):
            return
        f.wrapped = False
        if w in KEYWORDS_KEEP:
            f.at_cmd = True
            return
        if w == "case":
            f.pending_case = True
            f.at_cmd = False
            return
        if w == "esac":
            if f.cases:
                f.cases.pop()
            f.at_cmd = False
            return
        if w in KEYWORDS_END:
            f.at_cmd = False
            return
        names.append(w)
        if w in WRAPPERS:
            f.wrapped = True
            f.at_cmd = True
        else:
            f.at_cmd = False

    def push_sub():
        nonlocal word, raw, dynamic, in_word
        fr = Frame("sub")
        fr.saved = (word, raw, dynamic, in_word)
        word, raw, dynamic, in_word = [], [], False, False
        stack.append(fr)

    def pop_sub():
        nonlocal word, raw, dynamic, in_word
        fr = stack.pop()
        word, raw, dynamic, in_word = fr.saved

    def to_cmd():
        f = stack[-1]
        if isinstance(f, Frame):
            f.at_cmd = not (f.cases and f.cases[-1])
            f.wrapped = False

    while i < n:
        c = text[i]
        top = stack[-1]
        nxt = text[i + 1] if i + 1 < n else ""

        # ---- inside "..." ----
        if top == "dq":
            if c == "\\":
                i += 2
            elif c == '"':
                stack.pop()
                raw.append(c)
                i += 1
            elif c == "$" and nxt == "(" and text[i + 2:i + 3] == "(":
                stack.append("arith:0")
                i += 3
            elif c == "$" and nxt == "(":
                push_sub()
                i += 2
            elif c == "$" and nxt == "{":
                stack.append("brace")
                i += 2
            elif c == "`":
                raise SystemExit("unsupported construct: backtick command substitution")
            else:
                raw.append(c)
                i += 1
            continue
        # ---- inside ${...} ----
        if top == "brace":
            if c == "\\":
                i += 2
            elif c == "}":
                stack.pop()
                i += 1
            elif c == '"':
                stack.append("dq")
                i += 1
            elif c == "'":
                j = text.find("'", i + 1)
                i = n if j < 0 else j + 1
            elif c == "$" and nxt == "(" and text[i + 2:i + 3] == "(":
                stack.append("arith:0")
                i += 3
            elif c == "$" and nxt == "(":
                push_sub()
                i += 2
            elif c == "$" and nxt == "{":
                stack.append("brace")
                i += 2
            else:
                i += 1
            continue
        # ---- inside $(( ... )) ----
        if isinstance(top, str) and top.startswith("arith"):
            depth = int(top.split(":")[1])
            if c == "(":
                stack[-1] = "arith:%d" % (depth + 1)
            elif c == ")":
                if depth == 0:
                    stack.pop()
                    i += 1 if nxt != ")" else 2
                    continue
                stack[-1] = "arith:%d" % (depth - 1)
            i += 1
            continue

        # ---- command context ----
        f = top
        if c == "\\":
            if nxt == "\n":
                i += 2
                continue
            in_word, dynamic = True, True
            raw.append(c + nxt)
            i += 2
            continue
        if c == "'":
            j = text.find("'", i + 1)
            end = n if j < 0 else j + 1
            in_word, dynamic = True, True
            raw.append(text[i:end])
            i = end
            continue
        if c == '"':
            in_word, dynamic = True, True
            raw.append('"')
            stack.append("dq")
            i += 1
            continue
        if c == "`":
            raise SystemExit("unsupported construct: backtick command substitution")
        if c == "$":
            if nxt == "(" and text[i + 2:i + 3] == "(":
                in_word, dynamic = True, True
                raw.append("$")
                stack.append("arith:0")
                i += 3
                continue
            if nxt == "(":
                in_word, dynamic = True, True
                raw.append("$")
                push_sub()
                i += 2
                continue
            if nxt == "{":
                in_word, dynamic = True, True
                raw.append("$")
                stack.append("brace")
                i += 2
                continue
            in_word, dynamic = True, True
            raw.append(c)
            i += 1
            continue
        if c == "#" and not in_word:
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c in " \t":
            finish_word()
            i += 1
            continue
        if c == "\n":
            finish_word()
            i += 1
            if heredocs and heredocs[-1][0] != "":
                pending, heredocs[:] = list(heredocs), []
                for delim, expands in pending:
                    body = []
                    while i < n:
                        j = text.find("\n", i)
                        line = text[i:] if j < 0 else text[i:j]
                        i = n if j < 0 else j + 1
                        if line.strip("\t") == delim:
                            break
                        body.append(line)
                    # An unquoted delimiter expands the body: $(...) and `...` run.
                    if expands:
                        names.extend(heredoc_substitutions("\n".join(body)))
            to_cmd()
            continue
        if c == ";":
            finish_word()
            if nxt == ";":
                i += 2
                if f.cases:
                    f.cases[-1] = True
                    f.at_cmd = False
                continue
            i += 1
            to_cmd()
            continue
        if c == "&":
            if raw and raw[-1] and raw[-1][-1] in "<>" and in_word:
                raw.append(c)
                word.append(c)
                i += 1
                continue
            finish_word()
            i += 2 if nxt == "&" else 1
            to_cmd()
            continue
        if c == "|":
            if f.cases and f.cases[-1]:
                finish_word()
                i += 1
                continue
            finish_word()
            i += 2 if nxt == "|" else 1
            to_cmd()
            continue
        if c == "(":
            finish_word()
            if text[i + 1:].lstrip(" \t").startswith(")"):      # name() -- a function definition
                i = text.index(")", i) + 1
                to_cmd()
                continue
            if f.cases and f.cases[-1]:
                i += 1
                continue
            push_sub()
            i += 1
            continue
        if c == ")":
            finish_word()
            if f.cases and f.cases[-1]:
                f.cases[-1] = False
                f.at_cmd = True
                f.wrapped = False
            elif f.kind == "sub":
                pop_sub()
            i += 1
            continue
        in_word = True
        word.append(c)
        raw.append(c)
        i += 1
    finish_word()
    return names


# A call is a call however its namespace is spelled: `process::capture(`,
# `planar::process::capture(`, an alias `pr::capture(`, or bare `capture(` after a
# using-declaration. A member call (`x.capture(`, `p->capture(`) is not one.
QUAL = r"(?<![\w.>])(?:\w+::)*"


def runner_start_regex(src):
    """`runner::start(` under any alias of the runner namespace, and a bare
    `start(` when a using-declaration or using-directive brought it in."""
    names = {"runner"}
    names.update(re.findall(r"namespace\s+(\w+)\s*=\s*[\w:]*\brunner\s*;", src))
    alt = "|".join(sorted(names))
    rx = r"(?<![\w.>])(?:\w+::)*(?:%s)::start\s*\(" % alt
    if re.search(r"using\s+namespace\s+[\w:]*\brunner\s*;|using\s+[\w:]*\brunner::start\s*;", src):
        rx = r"(?:%s)|(?<![\w.>:])start\s*\(" % rx
    return rx


def scan_update(directory):
    programs = set()
    sites = {}
    apis = [
        ("capture", QUAL + r"capture\s*\("),
        ("run_inherited", r"\brun_inherited\s*\("),
        ("runner_start", None),
        ("execv", r"\bexecv[a-z]*\s*\("),
        ("posix_spawn", r"\bposix_spawn[a-z]*\s*\("),
        ("fork", r"\b(?:vfork|fork)\s*\("),
        ("popen", r"\bpopen\s*\("),
        ("system", r"(?<![\w:.])system\s*\("),
        ("resolve_program", r"\bresolve_program\s*\("),
    ]
    for path in sorted(glob.glob(directory + "/*.cpp") + glob.glob(directory + "/*.cppm")):
        if path.endswith(".t.cpp"):
            continue
        src = open(path, encoding="utf-8").read()
        src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
        src = re.sub(r"//[^\n]*", "", src)
        for name, rx in apis:
            if rx is None:
                rx = runner_start_regex(src)
            for m in re.finditer(rx, src):
                sites[name] = sites.get(name, 0) + 1
                rest = src[m.end():]
                if name == "capture":
                    lit = re.match(r'\s*"([^"]+)"', rest)
                    if not lit:
                        programs.add("<non-literal capture program>")
                elif name == "resolve_program":
                    lit = re.match(r'[^,()]*(?:\([^()]*\))?[^,()]*,\s*"([^"]+)"', rest)
                    if not lit:
                        programs.add("<non-literal resolve_program program>")
        consts = dict(re.findall(r'constexpr\s+std::string_view\s+(\w+)\s*=\s*"([^"]+)"', src))
        for rx in (QUAL + r'capture\(\s*"([^"]+)"',
                   r'resolve_program\([^,()]*(?:\([^()]*\))?[^,()]*,\s*"([^"]+)"',
                   r'\.argv\s*=\s*\{\s*"([^"]+)"',
                   r'\{\s*(?:"[A-Za-z_]+=[^"]*",\s*)+"([^"]+)"'):
            for m in re.finditer(rx, src):
                programs.add(m.group(1).rsplit("/", 1)[-1])
        for m in re.finditer(r'std::array<std::string_view,\s*\d+>\s*const\s+args\{\s*((?:"[A-Za-z_]+=[^"]*",\s*)+)([^,}]+)', src):
            tok = m.group(2).strip()
            lit = re.match(r'^"([^"]+)"$', tok)
            if lit:
                programs.add(lit.group(1).rsplit("/", 1)[-1])
            else:
                programs.add(consts.get(tok, tok).rsplit("/", 1)[-1])
    for p in sorted(programs):
        print("program " + p)
    for name, count in sorted(sites.items()):
        print("site %s %d" % (name, count))


def self_test():
    got = scan_sh('''#!/bin/sh
# a comment: curl nothing
die() { printf '%s\\n' "$*" >&2; exit 1; }
x=$(uname -s 2>/dev/null) || x=""
case "$x:$y" in
  Darwin:arm64) v=$(sw_vers -productVersion); ;;
  a|b) tar -xzf f ;;
  *) ;;
esac
if command -v ldd >/dev/null 2>&1 && [ -n "$(find . -type f | wc -c)" ]; then
  (cd "$d" && sha256sum -c "$2" >/dev/null 2>&1)
fi
while IFS= read -r l; do echo "$l" | tr a b; done < f
cat <<'EOF'
not-a-command here
EOF
n=$((n + 1)); FOO=1 mkdir "$d/x" || die "x $(id -u)"
''')
    want = {"printf", "exit", "uname", "sw_vers", "tar", "command", "ldd", "find", "wc", "sha256sum", "cd", "read", "echo", "tr", "cat", "mkdir", "die", "id", "["}
    assert set(got) == want, (sorted(set(got) ^ want))
    assert "curl" not in got and "not-a-command" not in got
    # An unquoted here-document expands its body, so its $( ) and backticks run; a quoted one does not.
    got = scan_sh("cat <<EOF\nhello $(hd_sub --x) and `hd_tick` \\$(hd_escaped)\nEOF\n"
                  "cat <<'EOF'\n$(q_single)\nEOF\ncat <<\"EOF\"\n$(q_double)\nEOF\n"
                  "cat <<\\EOF\n$(q_back)\nEOF\ncat << 'EOF'\n$(q_spaced)\nEOF\n"
                  "cat <<-EOF\n\t$(hd_dash)\n\tEOF\n"
                  "cat << EOF\n$(hd_spaced)\nEOF\n")
    assert sorted(got) == ["cat"] * 7 + ["hd_dash", "hd_spaced", "hd_sub", "hd_tick"], got
    print("install-prereq-scan: self-test passed")


def main(argv):
    if argv == ["--self-test"]:
        self_test()
        return 0
    if len(argv) == 2 and argv[0] == "update":
        scan_update(argv[1])
        return 0
    if len(argv) == 2 and argv[0] == "bootstrap":
        for name in sorted(set(scan_sh(open(argv[1], encoding="utf-8").read()))):
            print("cmd " + name)
        return 0
    print(__doc__.split("\n\n")[0], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
