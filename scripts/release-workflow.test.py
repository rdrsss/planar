#!/usr/bin/env python3
"""Lint and behaviour tests for .github/workflows/release.yml.

Plan 1122, task rel-release-workflow; test spec scenarios 3750 and 3760 (the CI
half). The workflow is the CI path of the same evidence contract `make
release-cut` uses (docs/operations.md, "Release Gate Evidence"), and it will not
be run on GitHub from this repository, so it is verified statically here:

  * LintTests parse release.yml and assert the trigger, the three jobs, runs-on
    per platform, the needs wiring, gate-before-upload order inside each platform
    job, that exactly one step calls scripts/release-publish.sh and that no other
    step calls gh, the permissions, and that the file has not drifted from the
    scripts and the Makefile target it mirrors.
  * MutationTests copy the workflow, break it one way at a time (a gate step
    removed, a needs dropped, an upload before the gates, a trigger widened, ...)
    and require the lint to refuse each copy, naming the defect.
  * PublishStepTests run the workflow's own publisher shell steps against the
    fake gh, fake bundles and a scratch repository with a scratch annotated tag
    that release-publish.test.py builds: a missing or failed gate artifact
    prevents `gh release create`.

YAML: no YAML library is a dependency of this repository's Python tooling and
the Linux gate image has none, so this file carries a small reader for the
block-style subset release.yml uses (mappings, sequences, flow lists, quoted and
plain scalars, `|` block scalars). Anything else (anchors, tags, flow mappings,
folded scalars, multi-line plain scalars) is refused, never guessed at.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / '.github/workflows/release.yml'
PLATFORMS = ('macos-arm64', 'linux-x86_64')


# --- the YAML subset reader -------------------------------------------------

class YamlError(ValueError):
    pass


def _cut_comment(value):
    if value[:1] in ('"', "'"):
        close = value.find(value[0], 1)
        if close < 0:
            raise YamlError(f'unterminated quoted scalar: {value}')
        rest = value[close + 1:]
        if rest.strip() and not rest.lstrip().startswith('#'):
            raise YamlError(f'text after a quoted scalar: {value}')
        return value[:close + 1]
    match = re.search(r'(^|\s)#', value)
    return value[:match.start()].rstrip() if match else value.rstrip()


def _scalar(value):
    value = _cut_comment(value.strip())
    if value == '':
        return None
    head = value[0]
    if head == '"':
        return json.loads(value)
    if head == "'":
        return value[1:-1].replace("''", "'")
    if head == '[':
        if not value.endswith(']'):
            raise YamlError(f'multi-line flow sequence: {value}')
        inner, items, depth, quote, start = value[1:-1], [], 0, None, 0
        for i, ch in enumerate(inner):
            if quote:
                quote = None if ch == quote else quote
            elif ch in '"\'':
                quote = ch
            elif ch in '[{':
                depth += 1
            elif ch in ']}':
                depth -= 1
            elif ch == ',' and depth == 0:
                items.append(inner[start:i])
                start = i + 1
        items.append(inner[start:])
        return [_scalar(item) for item in items if item.strip()]
    if head in '{&*!>|%@`' or value.startswith(('- ', '? ')) or ': ' in value or value.endswith(':'):
        raise YamlError(f'unsupported YAML scalar: {value}')
    if value in ('true', 'false'):
        return value == 'true'
    if value in ('null', '~'):
        return None
    if re.fullmatch(r'-?\d+', value):
        return int(value)
    return value


class _Reader:
    KEY = re.compile(r'^("[^"]*"|\'[^\']*\'|[^\s"\'\[{&*!|>%@`#-][^:]*?|-[^\s:][^:]*?)\s*:(?:\s+(.*))?$')

    def __init__(self, text):
        if '\t' in text:
            raise YamlError('tab characters are not valid YAML indentation')
        self.lines = text.split('\n')

    def indent(self, i):
        line = self.lines[i]
        return len(line) - len(line.lstrip(' '))

    def skip(self, i):
        while i < len(self.lines) and (not self.lines[i].strip() or self.lines[i].lstrip().startswith('#')):
            i += 1
        return i

    def node(self, i, minimum):
        i = self.skip(i)
        if i >= len(self.lines) or self.indent(i) < minimum:
            return None, i
        text = self.lines[i].strip()
        if text == '-' or text.startswith('- '):
            return self.sequence(i, self.indent(i))
        return self.mapping(i, self.indent(i))

    def sequence(self, i, indent):
        items = []
        while True:
            i = self.skip(i)
            if i >= len(self.lines) or self.indent(i) != indent:
                return items, i
            text = self.lines[i].strip()
            if not (text == '-' or text.startswith('- ')):
                return items, i
            rest = text[1:].lstrip()
            if not rest or rest.startswith('#'):
                item, i = self.node(i + 1, indent + 1)
            elif rest[0] not in '"\'[' and self.KEY.match(_cut_comment_line(rest)):
                # `- key: value` opens a mapping whose first line is this one.
                offset = indent + 1 + (len(text) - 1 - len(rest))
                self.lines[i] = ' ' * offset + rest
                item, i = self.mapping(i, offset)
            else:
                item, i = _scalar(rest), i + 1
            items.append(item)

    def mapping(self, i, indent):
        result = {}
        while True:
            i = self.skip(i)
            if i >= len(self.lines) or self.indent(i) != indent:
                return result, i
            text = self.lines[i].strip()
            if text == '-' or text.startswith('- '):
                return result, i
            match = self.KEY.match(_cut_comment_line(text))
            if not match:
                raise YamlError(f'line {i + 1}: not a mapping entry: {text}')
            key = _scalar(match.group(1)) if match.group(1)[0] in '"\'' else match.group(1).strip()
            if key in result:
                raise YamlError(f'line {i + 1}: duplicate key {key}')
            value = (match.group(2) or '').strip()
            if value[:1] == '|':
                if value not in ('|', '|-', '|+'):
                    raise YamlError(f'line {i + 1}: unsupported block scalar header {value}')
                result[key], i = self.block(i + 1, indent, value)
            elif value == '' or value.startswith('#'):
                j = self.skip(i + 1)
                if j < len(self.lines) and self.indent(j) == indent and self.lines[j].strip().startswith('- '):
                    result[key], i = self.sequence(j, indent)
                else:
                    result[key], i = self.node(i + 1, indent + 1)
            else:
                result[key], i = _scalar(value), i + 1

    def block(self, i, parent, header):
        body, width = [], None
        while i < len(self.lines):
            line = self.lines[i]
            if line.strip() == '':
                body.append('')
                i += 1
                continue
            if self.indent(i) <= parent:
                break
            width = self.indent(i) if width is None else min(width, self.indent(i))
            body.append(line)
            i += 1
        while body and body[-1] == '':
            body.pop()
        text = '\n'.join(line[width:] if line else '' for line in body)
        return (text if header == '|-' else text + '\n'), i


def _cut_comment_line(text):
    return text  # comments inside values are cut by _scalar; keys carry none


def parse_yaml(text):
    reader = _Reader(text)
    value, i = reader.node(0, 0)
    if reader.skip(i) < len(reader.lines):
        raise YamlError(f'line {reader.skip(i) + 1}: unparsed content')
    return value


# --- GitHub tag filter ------------------------------------------------------

def tag_filter_regex(pattern):
    """The GitHub filter-pattern subset release.yml may use, as a full-match regex."""
    out, i = '', 0
    while i < len(pattern):
        ch = pattern[i]
        if ch == '*':
            raise ValueError(f'wildcard * in tag filter {pattern!r} is wider than a stable tag')
        if ch == '[':
            close = pattern.index(']', i)
            out += pattern[i:close + 1]
            i = close
        elif ch == '+':
            out += '+'
        elif ch == '.':
            out += r'\.'
        elif ch in '?!\\':
            raise ValueError(f'unsupported tag filter character {ch!r}')
        else:
            out += re.escape(ch)
        i += 1
    return re.compile(out)


STABLE = ('v0.0.1', 'v1.2.3', 'v10.20.30')
NOT_STABLE = ('v1.2.3-rc1', 'v1.2.3+build', 'v1.2', 'v1.2.3.4', 'v1', 'vv1.2.3', 'v1.2.x', '1.2.3', 'master', 'v*', '')


# --- the lint ---------------------------------------------------------------

USES = re.compile(r'^(actions/(?:checkout|upload-artifact|download-artifact))@([0-9a-f]{40})$')
PUBLISH_RUN = 'scripts/release-publish.sh "$TAG" release-in/macos-arm64 release-in/linux-x86_64'
NAMES = {p: f'release-{p}' for p in PLATFORMS}
BUILD = {'macos-arm64': 'make dist', 'linux-x86_64': 'make linux-dist LINUX_DIST_OUT=release-out/linux-x86_64'}


def step_text(step):
    return f'{step.get("run", "")}\n{step.get("uses", "")}'


def lint(text, root=ROOT):
    """Return the list of defects in a release.yml text (empty when it is sound)."""
    problems = []

    def need(condition, message):
        if not condition:
            problems.append(message)

    try:
        wf = parse_yaml(text)
    except (YamlError, ValueError) as error:
        return [f'release.yml does not parse: {error}']
    if not isinstance(wf, dict):
        return ['release.yml is not a mapping']
    need(set(wf) == {'name', 'on', 'permissions', 'concurrency', 'jobs'},
         f'top-level keys are {sorted(wf)}, not name, on, permissions, concurrency, jobs')

    # Trigger: stable tags only.
    on = wf.get('on')
    need(isinstance(on, dict) and set(on) == {'push'},
         f'trigger must be push only (no branches, pull_request, schedule or dispatch), found {on!r}')
    push = on.get('push') if isinstance(on, dict) else None
    need(isinstance(push, dict) and set(push) == {'tags'},
         f'push trigger must filter tags only (no branches, paths or branches-ignore), found {push!r}')
    tags = push.get('tags') if isinstance(push, dict) else None
    if isinstance(tags, list) and len(tags) == 1 and isinstance(tags[0], str):
        try:
            regex = tag_filter_regex(tags[0])
            need(all(regex.fullmatch(t) for t in STABLE), f'tag filter {tags[0]!r} must match stable tags {STABLE}')
            need(not any(regex.fullmatch(t) for t in NOT_STABLE),
                 f'tag filter {tags[0]!r} matches a tag that is not a stable vMAJOR.MINOR.PATCH: '
                 f'{[t for t in NOT_STABLE if regex.fullmatch(t)]}')
        except ValueError as error:
            problems.append(str(error))
    else:
        problems.append(f'tags must be exactly one filter pattern, found {tags!r}')

    need(wf.get('permissions') == {'contents': 'read'}, 'workflow permissions must be exactly contents: read')
    concurrency = wf.get('concurrency')
    need(isinstance(concurrency, dict) and concurrency.get('cancel-in-progress') is False
         and 'github.ref' in str(concurrency.get('group')),
         'concurrency must group by ref and never cancel a release in progress')

    jobs = wf.get('jobs')
    if not isinstance(jobs, dict) or set(jobs) != {*PLATFORMS, 'publish'}:
        problems.append(f'jobs must be exactly {sorted([*PLATFORMS, "publish"])}, found '
                        f'{sorted(jobs) if isinstance(jobs, dict) else jobs!r}')
        return problems

    # Whole-file rules: pinned actions from an allowlist, nothing may soften a failure.
    for name, job in jobs.items():
        steps = job.get('steps') if isinstance(job, dict) else None
        if not isinstance(steps, list) or not steps:
            problems.append(f'job {name} has no steps')
            return problems
        need(isinstance(job.get('timeout-minutes'), int) and not isinstance(job['timeout-minutes'], bool),
             f'job {name} needs an integer timeout-minutes')
        need('continue-on-error' not in job and 'strategy' not in job and 'container' not in job,
             f'job {name} must not use continue-on-error, strategy or container')
        for step in steps:
            label = f'job {name} step {step.get("name")!r}'
            need('continue-on-error' not in step, f'{label} uses continue-on-error')
            if 'uses' in step:
                need(bool(USES.match(step['uses'])), f'{label} uses {step["uses"]!r}: only pinned (40-hex) '
                     'actions/checkout, upload-artifact and download-artifact are allowed')
            else:
                need('run' in step, f'{label} neither uses nor runs anything')
    need(re.search(r'(?m)^\s*(shell|working-directory):', text) is None and 'defaults' not in str(wf),
         'no custom shell or working-directory: a step must run in the checkout under the runner default shell -e')

    # Permissions: only the publisher writes.
    for name, job in jobs.items():
        want = {'contents': 'write'} if name == 'publish' else {'contents': 'read'}
        need(job.get('permissions') == want, f'job {name} permissions must be exactly {want}, found '
             f'{job.get("permissions")!r}')

    # Runners.
    for name, job in jobs.items():
        runner = job.get('runs-on')
        if name == 'macos-arm64':
            match = re.fullmatch(r'macos-(\d+)', runner) if isinstance(runner, str) else None
            need(bool(match) and int(match.group(1)) >= 26,
                 f'macos-arm64 must run on an Apple-silicon macOS runner at or above the 26.0 floor (macos-26), found {runner!r}')
        else:
            need(isinstance(runner, str) and re.fullmatch(r'ubuntu-\d\d\.04', runner) is not None,
                 f'job {name} must run on an x86_64 ubuntu runner (ubuntu-NN.04), found {runner!r}')

    # needs: the publisher waits for both platforms; nothing else waits or is conditional.
    publish = jobs['publish']
    needs = publish.get('needs')
    need(isinstance(needs, list) and sorted(needs) == sorted(PLATFORMS),
         f'publish must need exactly {sorted(PLATFORMS)}, found {needs!r}')
    for name, job in jobs.items():
        need('if' not in job, f'job {name} must not carry a job-level if (a skipped gate must not publish)')
        if name != 'publish':
            need('needs' not in job, f'platform job {name} must not wait on another job')
        for step in job['steps']:
            need('if' not in step, f'job {name} step {step.get("name")!r} must not be conditional')

    # Every job builds from the resolved tag commit.
    for name, job in jobs.items():
        checkout = job['steps'][0]
        need(str(checkout.get('uses', '')).startswith('actions/checkout@'), f'job {name} must start with a checkout')
        if checkout.get('with') is not None or True:
            w = checkout.get('with') or {}
            need(w.get('ref') == 'refs/tags/${{ github.ref_name }}',
                 f'job {name} must check out refs/tags/${{{{ github.ref_name }}}} explicitly, found {w.get("ref")!r}')
            need(w.get('fetch-depth') == 0 and w.get('fetch-tags') is True,
                 f'job {name} checkout needs fetch-depth 0 and fetch-tags so the annotated tag object exists')
            need(w.get('persist-credentials') is False, f'job {name} checkout must not persist credentials')
        need((job.get('env') or {}).get('TAG') == '${{ github.ref_name }}', f'job {name} env TAG must be github.ref_name')

    # Platform jobs: preflight, build, gates, then (and only then) upload.
    for platform in PLATFORMS:
        job = jobs[platform]
        steps = job['steps']
        label = f'job {platform}'
        need((job.get('env') or {}).get('PLANAR_RELEASE_VERSION') == '${{ github.ref_name }}',
             f'{label} must build with PLANAR_RELEASE_VERSION set to the tag')
        runs = [(i, s.get('run', '').strip()) for i, s in enumerate(steps)]
        gate_cmd = f'scripts/release-gates.sh --platform {platform} release-out/{platform}'
        pre = [i for i, r in runs if r == 'scripts/release-publish.sh --preflight "$TAG"']
        build = [i for i, r in runs if r == BUILD[platform]]
        stage = [i for i, r in runs if platform == 'macos-arm64' and 'release-out/macos-arm64' in r
                 and r.startswith('set -eu') and 'cp dist/planar-macos-arm64.tar.gz' in r]
        gates = [i for i, r in runs if 'release-gates.sh' in r]
        gate_ok = [i for i, r in runs if r == gate_cmd]
        uploads = [i for i, s in enumerate(steps) if str(s.get('uses', '')).startswith('actions/upload-artifact@')]
        need(len(pre) == 1, f'{label} needs exactly one preflight step (tag, annotated, HEAD at tag, clean tree)')
        need(len(build) == 1, f'{label} needs exactly one build step {BUILD[platform]!r}')
        need(len(gates) == 1 and gate_ok == gates,
             f'{label} needs exactly one gate step running exactly {gate_cmd!r}, with no || or ; softening it')
        need(len(uploads) == 1, f'{label} needs exactly one upload step')
        if len(pre) == len(build) == len(gate_ok) == len(uploads) == 1:
            need(pre[0] < build[0] < gate_ok[0] < uploads[0],
                 f'{label} must preflight, build, run gates, then upload (order is {pre[0]}, {build[0]}, '
                 f'{gate_ok[0]}, {uploads[0]})')
            if platform == 'macos-arm64':
                need(len(stage) == 1 and build[0] < stage[0] < gate_ok[0],
                     f'{label} must stage dist/ into release-out/macos-arm64 between build and gates')
        for step in steps:
            need('gh ' not in step_text(step) and 'release-publish.sh' not in step_text(step).replace(
                'release-publish.sh --preflight "$TAG"', ''), f'{label} step {step.get("name")!r} publishes')
        if len(uploads) == 1:
            up = steps[uploads[0]]
            w = up.get('with') or {}
            lines = [x.strip() for x in str(w.get('path', '')).splitlines() if x.strip()]
            want = [f'release-out/{platform}/planar-{platform}.tar.gz',
                    f'release-out/{platform}/planar-{platform}.tar.gz.gates.json',
                    f'release-out/{platform}/get-planar.sh',
                    f'release-out/{platform}/planar-{platform}.gate-logs']
            need(lines == want, f'{label} must upload exactly the archive, its gates.json, get-planar.sh and '
                 f'the gate-logs directory, found {lines}')
            need(w.get('name') == NAMES[platform], f'{label} must upload artifact {NAMES[platform]}')
            need(w.get('if-no-files-found') == 'error', f'{label} upload must fail on a missing file')

    # Publisher job: both artifacts into separate directories, then the one publisher step.
    steps = publish['steps']
    publisher = [i for i, s in enumerate(steps) if 'release-publish.sh' in step_text(s)]
    need(len(publisher) == 1 and steps[publisher[0]].get('run', '').strip() == PUBLISH_RUN,
         f'publish must have exactly one step whose run is exactly {PUBLISH_RUN!r}')
    need(publisher == [len(steps) - 1], 'the publisher step must be the last step of the publish job')
    downloads = {}
    for i, s in enumerate(steps):
        if str(s.get('uses', '')).startswith('actions/download-artifact@'):
            w = s.get('with') or {}
            downloads[w.get('name')] = (w.get('path'), i)
        need('upload' not in str(s.get('uses', '')), 'the publish job must not upload artifacts')
    need({k: v[0] for k, v in downloads.items()} == {NAMES[p]: f'release-in/{p}' for p in PLATFORMS},
         f'publish must download each platform artifact into its own release-in/<platform> directory, found {downloads}')
    env = publish.get('env') or {}
    need(env.get('GH_TOKEN') == '${{ github.token }}', 'publish env GH_TOKEN must be github.token')
    for name, job in jobs.items():
        for s in job['steps']:
            if s.get('run') and 'release-publish.sh "$TAG" release-in' not in s['run']:
                need(not re.search(r'\bgh\b|github\.com/.*/releases|curl\b', s['run']),
                     f'job {name} step {s.get("name")!r} talks to GitHub outside the publisher')

    # No drift from the scripts and the Makefile target it mirrors.
    makefile = (root / 'Makefile').read_text()
    gates_sh = (root / 'scripts/release-gates.sh').read_text()
    publish_sh = (root / 'scripts/release-publish.sh').read_text()
    for platform in PLATFORMS:
        need(f'scripts/release-gates.sh --platform {platform} ' in makefile,
             f'release-cut no longer gates {platform} through scripts/release-gates.sh')
        need(f"'{platform}': dict(" in publish_sh, f'release-publish.sh no longer lists platform {platform}')
        need(f'planar-{platform}' in makefile or platform in makefile, f'Makefile does not name {platform}')
    need('$(MAKE) dist' in makefile and '$(MAKE) linux-dist LINUX_DIST_OUT=' in makefile,
         'release-cut no longer builds with make dist and make linux-dist LINUX_DIST_OUT=')
    need('scripts/release-publish.sh $$publish_mode "$$tag"' in makefile, 'release-cut no longer ends in the publisher')
    need('archive="$dir/planar-$platform.tar.gz"' in gates_sh and 'planar-$platform.gate-logs' in gates_sh,
         'release-gates.sh no longer names planar-$platform.tar.gz and planar-$platform.gate-logs')
    need('--preflight' in publish_sh, 'release-publish.sh no longer has --preflight')
    return problems


# --- tests ------------------------------------------------------------------

def load(path):
    spec = importlib.util.spec_from_file_location(Path(path).stem.replace('.', '_').replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class YamlReaderTests(unittest.TestCase):
    def test_reads_the_supported_subset(self):
        doc = parse_yaml('a: 1\nb:\n  - x\n  - y: "q # r"  # c\n    z: [1, "two", three]\nc: |\n  l1\n\n  l2\nd:\n- p\n')
        self.assertEqual(doc, {'a': 1, 'b': ['x', {'y': 'q # r', 'z': [1, 'two', 'three']}],
                               'c': 'l1\n\nl2\n', 'd': ['p']})

    def test_refuses_what_it_cannot_read(self):
        for bad in ('a: &x 1\n', 'a: *x\n', 'a: {b: 1}\n', 'a: >\n  folded\n', 'a: b: c\n', 'a: 1\na: 2\n',
                    'a:\n\tb: 1\n', 'a: !!str 1\n'):
            with self.subTest(bad=bad), self.assertRaises(YamlError):
                parse_yaml(bad)

    def test_agrees_with_pyyaml_on_the_repository_workflows(self):
        try:
            import yaml
        except ImportError:
            return  # PyYAML is optional; the reader's own cases above are authoritative.
        for path in sorted((ROOT / '.github/workflows').glob('*.yml')):
            with self.subTest(workflow=path.name):
                expected = yaml.load(path.read_text(), Loader=yaml.BaseLoader)
                got = json.loads(json.dumps(parse_yaml(path.read_text())))

                def norm(v):
                    if isinstance(v, dict):
                        return {k: norm(x) for k, x in v.items()}
                    if isinstance(v, list):
                        return [norm(x) for x in v]
                    return '' if v is None else str(v).lower() if isinstance(v, bool) else str(v)
                self.assertEqual(norm(got), norm(expected))


class LintTests(unittest.TestCase):
    """Scenario 3750: the workflow's shape, its gate dependencies, and no drift."""

    def setUp(self):
        self.text = WORKFLOW.read_text()
        self.wf = parse_yaml(self.text)

    def test_release_workflow_is_sound(self):
        self.assertEqual(lint(self.text), [])

    def test_triggers_on_stable_version_tags_only(self):
        self.assertEqual(self.wf['on'], {'push': {'tags': ['v[0-9]+.[0-9]+.[0-9]+']}})
        regex = tag_filter_regex(self.wf['on']['push']['tags'][0])
        for tag in STABLE:
            self.assertTrue(regex.fullmatch(tag), tag)
        for tag in NOT_STABLE:
            self.assertFalse(regex.fullmatch(tag), tag)

    def test_three_jobs_with_runners_and_needs(self):
        jobs = self.wf['jobs']
        self.assertEqual(list(jobs), ['macos-arm64', 'linux-x86_64', 'publish'])
        self.assertEqual(jobs['macos-arm64']['runs-on'], 'macos-26')
        self.assertEqual(jobs['linux-x86_64']['runs-on'], 'ubuntu-24.04')
        self.assertEqual(jobs['publish']['needs'], ['macos-arm64', 'linux-x86_64'])
        self.assertIn('make linux-dist', ' '.join(s.get('run', '') for s in jobs['linux-x86_64']['steps']))

    def test_gates_run_before_every_upload(self):
        for platform in PLATFORMS:
            steps = self.wf['jobs'][platform]['steps']
            gate = next(i for i, s in enumerate(steps) if 'release-gates.sh' in s.get('run', ''))
            upload = next(i for i, s in enumerate(steps) if 'upload-artifact' in s.get('uses', ''))
            self.assertLess(gate, upload, platform)

    def test_only_the_publisher_step_publishes(self):
        callers = [(name, s['name']) for name, job in self.wf['jobs'].items() for s in job['steps']
                   if 'release-publish.sh' in s.get('run', '') and '--preflight' not in s['run']]
        self.assertEqual(callers, [('publish', 'Re-validate the evidence and publish')])
        every_step = ' '.join(step_text(s) for job in self.wf['jobs'].values() for s in job['steps'])
        self.assertNotIn('gh release', every_step, 'gh release is only ever run inside release-publish.sh')
        last = self.wf['jobs']['publish']['steps'][-1]
        self.assertEqual(last['run'].strip(), PUBLISH_RUN)

    def test_permissions_are_least_privilege(self):
        self.assertEqual(self.wf['permissions'], {'contents': 'read'})
        self.assertEqual({n: j['permissions'] for n, j in self.wf['jobs'].items()},
                         {'macos-arm64': {'contents': 'read'}, 'linux-x86_64': {'contents': 'read'},
                          'publish': {'contents': 'write'}})

    def test_every_action_is_pinned_and_jobs_are_bounded(self):
        for name, job in self.wf['jobs'].items():
            self.assertIsInstance(job['timeout-minutes'], int, name)
            for step in job['steps']:
                if 'uses' in step:
                    self.assertRegex(step['uses'], r'^actions/[a-z-]+@[0-9a-f]{40}$')

    def test_workflow_is_actionlint_clean_when_actionlint_is_installed(self):
        tool = shutil.which('actionlint')
        if tool is None:
            return  # reported by the validation profile; the structural lint above does not need it
        result = subprocess.run([tool, str(WORKFLOW)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


MUTATIONS = {
    # name: (regex, replacement, count, substring the lint must report)
    'gate step removed (macos)': (
        r'      - name: Run the macos-arm64 release gates\n        run: [^\n]*\n', '', 1, 'exactly one gate step'),
    'gate step removed (linux)': (
        r'      - name: Run the linux-x86_64 release gates\n        run: [^\n]*\n', '', 1, 'exactly one gate step'),
    'gate softened with || true': (
        r'(release-gates\.sh --platform linux-x86_64 release-out/linux-x86_64)', r'\1 || true', 1, 'exactly one gate step'),
    'gate runs another platform': (
        r'release-gates\.sh --platform macos-arm64 release-out', 'release-gates.sh --platform linux-x86_64 release-out', 1,
        'exactly one gate step'),
    'publish needs dropped': (r'    needs: \[macos-arm64, linux-x86_64\]\n', '', 1, 'publish must need exactly'),
    'publish needs only macos': (r'needs: \[macos-arm64, linux-x86_64\]', 'needs: [macos-arm64]', 1, 'publish must need exactly'),
    'publish needs only linux': (r'needs: \[macos-arm64, linux-x86_64\]', 'needs: [linux-x86_64]', 1, 'publish must need exactly'),
    'publish forced with if always': (
        r'    needs: \[macos-arm64, linux-x86_64\]\n', '    needs: [macos-arm64, linux-x86_64]\n    if: always()\n', 1,
        'must not carry a job-level if'),
    'gate step made conditional': (
        r'(      - name: Run the linux-x86_64 release gates\n)', r'\1        if: always()\n', 1, 'must not be conditional'),
    'gate allowed to fail': (
        r'(      - name: Run the macos-arm64 release gates\n)', r'\1        continue-on-error: true\n', 1,
        'continue-on-error'),
    'upload before gates (linux)': ('MOVE_UPLOAD', None, None, 'must preflight, build, run gates, then upload'),
    'upload before gates (macos)': ('MOVE_UPLOAD_MACOS', None, None, 'must preflight, build, run gates, then upload'),
    'trigger widened to branches': (
        r'    tags:\n', '    branches: [master]\n    tags:\n', 1, 'push trigger must filter tags only'),
    'trigger widened to all v tags': (r'"v\[0-9\]\+\.\[0-9\]\+\.\[0-9\]\+"', '"v*"', 1, 'wider than a stable tag'),
    'trigger admits pre-release tags': (
        r'"v\[0-9\]\+\.\[0-9\]\+\.\[0-9\]\+"', '"v[0-9]+.[0-9]+.[0-9]+*"', 1, 'wider than a stable tag'),
    'trigger adds pull_request': (r'^on:\n', 'on:\n  pull_request:\n    branches: [master]\n', 1, 'trigger must be push only'),
    'trigger adds workflow_dispatch': (r'^on:\n', 'on:\n  workflow_dispatch:\n', 1, 'trigger must be push only'),
    'workflow-level write': (r'^permissions:\n  contents: read\n', 'permissions:\n  contents: write\n', 1,
                             'workflow permissions must be exactly contents: read'),
    'platform job gets write': (
        r'(  linux-x86_64:\n(?:.*\n)*?    permissions:\n      contents: )read', r'\1write', 1, 'permissions must be exactly'),
    'publisher loses write': (
        r'(  publish:\n(?:.*\n)*?    permissions:\n      contents: )write', r'\1read', 1, 'permissions must be exactly'),
    'second gh release step': (
        r'(      - name: Upload the gated bundle and its evidence\n        uses: actions/upload-artifact@[0-9a-f]{40} # v4.6.2\n'
        r'        with:\n          name: release-linux-x86_64\n)',
        '      - name: Publish early\n        run: gh release create "$TAG" --notes x\n' + r'\1', 1, 'publishes'),
    'second publisher call in a platform job': (
        r'(      - name: Free disk space\n)', '      - name: Publish early\n        run: scripts/release-publish.sh "$TAG" release-out/linux-x86_64\n' + r'\1', 1,
        'publishes'),
    'publisher drops a platform directory': (
        r'release-in/macos-arm64 release-in/linux-x86_64', 'release-in/linux-x86_64', 1, 'exactly one step whose run is exactly'),
    'both artifacts into one directory': (
        r'path: release-in/linux-x86_64', 'path: release-in/macos-arm64', 1, 'its own release-in'),
    'download of one platform dropped': (
        r'      - name: Download the linux-x86_64 bundle\n(?:        .*\n){4}', '', 1, 'its own release-in'),
    'upload no longer fails when files are missing': (
        r'          if-no-files-found: error\n', '', 2, 'must fail on a missing file'),
    'upload omits the gate evidence': (
        r'            release-out/linux-x86_64/planar-linux-x86_64.tar.gz.gates.json\n', '', 1, 'must upload exactly'),
    'checkout without an explicit tag ref': (
        r'          ref: refs/tags/\$\{\{ github.ref_name \}\}\n', '', 3, 'must check out refs/tags'),
    'checkout without tag objects': (r'          fetch-tags: true\n', '', 3, 'fetch-tags'),
    'preflight removed': (
        r'      - name: Verify the tag, its commit and a clean checkout\n        run: [^\n]*\n', '', 2, 'preflight'),
    'build not from the tag version': (
        r'      PLANAR_RELEASE_VERSION: \$\{\{ github.ref_name \}\}\n', '', 2, 'PLANAR_RELEASE_VERSION'),
    'macos on an old runner': (r'runs-on: macos-26', 'runs-on: macos-13', 1, 'macos-arm64 must run on'),
    'linux on an arm runner': (r'runs-on: ubuntu-24.04\n    timeout-minutes: 240', 'runs-on: ubuntu-24.04-arm\n    timeout-minutes: 240', 1,
                               'x86_64 ubuntu runner'),
    'linux job built natively instead of the Docker dist stage': (
        r'run: make linux-dist LINUX_DIST_OUT=release-out/linux-x86_64', 'run: make dist', 1, 'exactly one build step'),
    'unpinned action': (r'actions/download-artifact@[0-9a-f]{40} # v4.3.0', 'actions/download-artifact@v4', 2, 'pinned'),
    'foreign release action': (
        r'(      - name: Re-validate the evidence and publish\n)', '      - name: Third party\n        uses: softprops/action-gh-release@v2\n' + r'\1', 1,
        'only pinned'),
    'timeout removed': (r'    timeout-minutes: 30\n', '', 1, 'timeout-minutes'),
    'trigger filter admits a two-part version': (
        r'"v\[0-9\]\+\.\[0-9\]\+\.\[0-9\]\+"', '"v[0-9]+.[0-9]+"', 1, 'must match stable tags'),
    'trigger filter admits extra version parts': (
        r'"v\[0-9\]\+\.\[0-9\]\+\.\[0-9\]\+"', '"v[0-9]+.[0-9]+[.0-9]+"', 1, 'matches a tag that is not a stable'),
    'second upload step after the gates': (
        r'(      - name: Run the linux-x86_64 release gates\n        run: [^\n]*\n)',
        r'\1      - name: Upload again\n        uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02 # v4.6.2\n'
        '        with:\n          name: again\n          path: x\n', 1, 'exactly one upload step'),
    'a step after the publisher': ('\\Z', '      - name: Summary\n        run: echo done\n', 1, 'must be the last step'),
    'gh release step before the publisher': (
        r'(      - name: Re-validate the evidence and publish\n)',
        '      - name: Early\n        run: gh release create "$TAG" release-in/macos-arm64/planar-macos-arm64.tar.gz\n' + r'\1', 1,
        'talks to GitHub outside the publisher'),
    'a job added': (r'^jobs:\n', 'jobs:\n  extra:\n    runs-on: ubuntu-24.04\n    steps:\n      - run: true\n', 1, 'jobs must be exactly'),
    'concurrency may cancel a release': (r'cancel-in-progress: false', 'cancel-in-progress: true', 1, 'never cancel'),
}


def mutate(text, name):
    pattern, replacement, count, _ = MUTATIONS[name]
    if pattern in ('MOVE_UPLOAD', 'MOVE_UPLOAD_MACOS'):
        platform = 'linux-x86_64' if pattern == 'MOVE_UPLOAD' else 'macos-arm64'
        upload = re.search(r'      - name: Upload the gated bundle and its evidence\n(?:        .*\n|          .*\n)+?'
                           r'(?=\n|\Z)', text[text.index(f'  {platform}:'):])
        start = text.index(f'  {platform}:') + upload.start()
        block = upload.group(0)
        without = text[:start] + text[start + len(block):]
        anchor = re.search(r'      - name: Check out the tag commit\n(?:        .*\n|          .*\n)+', without[without.index(f'  {platform}:'):])
        at = without.index(f'  {platform}:') + anchor.end()
        return without[:at] + block + without[at:]
    flags = re.MULTILINE
    new, n = re.subn(pattern, replacement, text, count=count, flags=flags)
    if n != count:
        raise AssertionError(f'mutation {name!r} applied {n} times, expected {count}')
    return new


class MutationTests(unittest.TestCase):
    """Each broken copy of the workflow must be refused, for the right reason."""

    def test_the_unbroken_workflow_passes(self):
        self.assertEqual(lint(WORKFLOW.read_text()), [])

    def test_every_mutation_is_refused_naming_its_defect(self):
        original = WORKFLOW.read_text()
        for name, (_, _, _, expect) in MUTATIONS.items():
            with self.subTest(mutation=name):
                broken = mutate(original, name)
                self.assertNotEqual(broken, original, 'the mutation must change the workflow')
                problems = lint(broken)
                self.assertTrue(problems, f'{name}: the lint accepted a broken workflow')
                self.assertTrue(any(expect in p for p in problems), f'{name}: expected {expect!r} in {problems}')

    def test_the_lint_reads_a_scratch_copy_not_the_repository_file(self):
        original = WORKFLOW.read_text()
        before = WORKFLOW.read_bytes()
        with tempfile.TemporaryDirectory() as scratch:
            copy = Path(scratch) / 'release.yml'
            copy.write_text(mutate(original, 'publish needs dropped'))
            self.assertTrue(lint(copy.read_text()))
        self.assertEqual(WORKFLOW.read_bytes(), before)

    def test_script_drift_is_refused(self):
        # The workflow names the scripts' platforms and the release-cut target; a rename on
        # their side must fail the lint even when release.yml is untouched.
        with tempfile.TemporaryDirectory() as scratch:
            fake = Path(scratch)
            (fake / 'scripts').mkdir()
            for name in ('release-gates.sh', 'release-publish.sh'):
                shutil.copy2(ROOT / 'scripts' / name, fake / 'scripts' / name)
            shutil.copy2(ROOT / 'Makefile', fake / 'Makefile')
            self.assertEqual(lint(WORKFLOW.read_text(), root=fake), [])
            for rel, old, new, expect in (
                    ('Makefile', '$(MAKE) linux-dist LINUX_DIST_OUT=', '$(MAKE) linux-bundle OUT=', 'release-cut no longer builds'),
                    ('scripts/release-gates.sh', 'planar-$platform.gate-logs', 'planar-$platform.logs', 'gate-logs'),
                    ('scripts/release-publish.sh', "'linux-x86_64': dict(", "'linux-amd64': dict(", 'no longer lists platform')):
                with self.subTest(drift=rel):
                    path = fake / rel
                    original = path.read_text()
                    self.assertIn(old, original)
                    path.write_text(original.replace(old, new))
                    self.assertTrue(any(expect in p for p in lint(WORKFLOW.read_text(), root=fake)))
                    path.write_text(original)


rp = load(ROOT / 'scripts/release-publish.test.py')


class PublishStepTests(rp.ReleaseFixture):
    """Scenario 3760, CI half: the workflow's own shell steps against fakes.

    The publish job's steps are taken from release.yml itself and run under `bash -e`
    (the runner's default shell) in the scratch repository, with the downloaded
    artifacts laid out as actions/download-artifact leaves them.
    """

    def steps(self, job):
        return parse_yaml(WORKFLOW.read_text())['jobs'][job]['steps']

    def run_step(self, job, name_prefix, tag=rp.TAG):
        step = next(s for s in self.steps(job) if s.get('name', '').startswith(name_prefix))
        return self.run_cmd(['bash', '-e', '-c', step['run']], TAG=tag)

    def download(self, **knobs):
        """Gate both platforms, then lay them out as the publish job's downloads."""
        for platform in rp.PLATFORMS:
            self.gated(platform, **{k: v for k, v in knobs.items() if k.startswith(platform)})
        for platform in rp.PLATFORMS:
            shutil.copytree(self.inputs / platform, self.repo / 'release-in' / platform)

    def publish_step(self, **updates):
        return self.run_cmd(['bash', '-e', '-c', self.steps('publish')[-1]['run']], TAG=rp.TAG, **updates)

    def assert_nothing_published(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.gh_log.exists(), 'gh must not run')
        self.assertNotIn('gh release create', result.stdout)

    def test_complete_gated_downloads_publish_once(self):
        self.download()
        result = self.publish_step()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = [json.loads(line) for line in self.gh_log.read_text().splitlines()]
        self.assertEqual([c['args'][:3] for c in calls], [['release', 'create', rp.TAG]])
        self.assertIn('--verify-tag', calls[0]['args'])
        self.assertIn('--notes-from-tag', calls[0]['args'])

    def test_preflight_step_accepts_the_tag_checkout(self):
        for job in ('macos-arm64', 'linux-x86_64', 'publish'):
            if job == 'publish':
                continue
            with self.subTest(job=job):
                result = self.run_step(job, 'Verify the tag')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), self.sha)

    def test_preflight_step_refuses_a_pre_release_a_moved_head_and_a_dirty_tree(self):
        self.git('tag', '-a', 'v1.2.0-rc1', '-m', 'candidate')
        pre = self.run_step('macos-arm64', 'Verify the tag', tag='v1.2.0-rc1')
        self.assertNotEqual(pre.returncode, 0)
        self.assertIn('pre-release tag v1.2.0-rc1 refused', pre.stderr)
        (self.repo / 'stray').write_text('x')
        dirty = self.run_step('linux-x86_64', 'Verify the tag')
        self.assertNotEqual(dirty.returncode, 0)
        self.assertIn('dirty source', dirty.stderr)
        (self.repo / 'stray').unlink()
        self.git('commit', '--allow-empty', '-qm', 'after the tag')
        moved = self.run_step('linux-x86_64', 'Verify the tag')
        self.assertIn('HEAD is at', moved.stderr)
        self.assertNotEqual(moved.returncode, 0)

    def test_missing_platform_download_publishes_nothing(self):
        self.download()
        shutil.rmtree(self.repo / 'release-in/linux-x86_64')
        result = self.publish_step()
        self.assert_nothing_published(result)
        self.assertIn('not a directory', result.stderr)

    def test_missing_gate_evidence_publishes_nothing(self):
        for platform in rp.PLATFORMS:
            with self.subTest(platform=platform):
                self.download()
                (self.repo / 'release-in' / platform / f'planar-{platform}.tar.gz.gates.json').unlink()
                result = self.publish_step()
                self.assert_nothing_published(result)
                self.assertIn('missing gate evidence', result.stderr)
                shutil.rmtree(self.repo / 'release-in')

    def test_assembly_only_evidence_publishes_nothing(self):
        # A platform job that uploaded straight after `make dist`, skipping its gates, leaves format 1.
        self.download()
        path = self.repo / 'release-in/macos-arm64/planar-macos-arm64.tar.gz.gates.json'
        record = json.loads(path.read_text())
        record['format_version'] = 1
        path.write_text(json.dumps(record))
        result = self.publish_step()
        self.assert_nothing_published(result)
        self.assertIn('format_version 1 is not release evidence', result.stderr)

    def test_failed_and_missing_gates_publish_nothing(self):
        cases = {'macos-arm64': ('smoke', None), 'linux-x86_64': ('ca_debian', None)}
        for platform, (gate, _) in cases.items():
            for how in ('fail', 'missing'):
                with self.subTest(platform=platform, gate=gate, how=how):
                    self.download()
                    path = self.repo / 'release-in' / platform / f'planar-{platform}.tar.gz.gates.json'
                    record = json.loads(path.read_text())
                    if how == 'fail':
                        record['gates'][gate]['result'] = 'fail'
                    else:
                        del record['gates'][gate]
                    path.write_text(json.dumps(record))
                    result = self.publish_step()
                    self.assert_nothing_published(result)
                    self.assertIn(gate, result.stderr)
                    shutil.rmtree(self.repo / 'release-in')

    def test_archive_changed_after_its_gates_publishes_nothing(self):
        self.download()
        with (self.repo / 'release-in/linux-x86_64/planar-linux-x86_64.tar.gz').open('ab') as archive:
            archive.write(b'\0')
        result = self.publish_step()
        self.assert_nothing_published(result)
        self.assertIn('differs from its gate evidence', result.stderr)

    def test_both_directories_holding_one_platform_is_refused(self):
        self.download()
        shutil.rmtree(self.repo / 'release-in/linux-x86_64')
        shutil.copytree(self.inputs / 'macos-arm64', self.repo / 'release-in/linux-x86_64')
        result = self.publish_step()
        self.assert_nothing_published(result)
        self.assertIn('duplicate macos-arm64 bundles', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
