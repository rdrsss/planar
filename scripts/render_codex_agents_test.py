"""Unit tests for scripts/render-codex-agents.py (run: python3 file [repo-root])."""
import os
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, 'render-codex-agents.py')
ROOT = os.path.dirname(HERE)


def agent_md(name, body, extra='', stem=None):
    return ('---\nname: %s\ndescription: Does a thing\n%splanar:\n'
            '  kind: agent\n  slug: %s\n---\n\n%s' % (name, extra, name, body))


class RenderCodexAgents(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.src = os.path.join(self.tmp, 'agents')
        self.out = os.path.join(self.tmp, 'out')
        os.makedirs(self.src)

    def put(self, fn, text):
        with open(os.path.join(self.src, fn), 'w', encoding='utf-8',
                  newline='') as f:
            f.write(text)

    def run_script(self):
        return subprocess.run([sys.executable, SCRIPT, self.src, self.out],
                              capture_output=True, text=True)

    def load(self, name):
        with open(os.path.join(self.out, name + '.toml'), 'rb') as f:
            return tomllib.load(f)

    def test_happy_path_over_repo_agents(self):
        shutil.copytree(os.path.join(ROOT, 'agents'), self.src,
                        dirs_exist_ok=True)
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stderr)
        got = sorted(os.listdir(self.out))
        want = sorted(f[:-3] + '.toml' for f in os.listdir(self.src)
                      if f.startswith('planar-') and f.endswith('.md'))
        self.assertEqual(got, want)
        self.assertEqual(len(got), 15)
        for doc in ('doctrine', 'methodology', 'models', 'cross-scope-writes'):
            self.assertNotIn(doc + '.toml', got)
        for fn in got:
            data = self.load(fn[:-5])
            self.assertEqual(list(data), ['name', 'description',
                                          'developer_instructions'])
            self.assertEqual(data['name'], fn[:-5])
            with open(os.path.join(self.src, fn[:-5] + '.md'),
                      encoding='utf-8', newline='') as f:
                text = f.read()
            self.assertEqual(data['developer_instructions'],
                             text.split('\n---\n', 1)[1].removeprefix('\n'))

    def test_body_round_trips(self):
        body = ('a """ b\nends with one backslash \\\nC:\\Users\\x\n\\d+\n'
                'é — \U0001F600\ttab\n')
        self.put('planar-x.md', agent_md('planar-x', body))
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.load('planar-x')['developer_instructions'], body)

    def test_ignores_other_keys(self):
        extra = 'model: opus\ncodex:\n  sandbox: x\n'
        self.put('planar-x.md', agent_md('planar-x', 'hi\n', extra))
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(list(self.load('planar-x')),
                         ['name', 'description', 'developer_instructions'])

    def test_no_frontmatter_exits_1_and_writes_nothing(self):
        self.put('planar-good.md', agent_md('planar-good', 'ok\n'))
        self.put('planar-bad.md', '# no frontmatter\n')
        r = self.run_script()
        self.assertEqual(r.returncode, 1)
        self.assertIn('planar-bad.md', r.stderr)
        self.assertEqual(os.listdir(self.out), [])

    def test_name_differs_from_stem_exits_1(self):
        self.put('planar-stem.md', agent_md('planar-other', 'x\n'))
        r = self.run_script()
        self.assertEqual(r.returncode, 1)
        self.assertIn('planar-stem.md', r.stderr)
        self.assertIn('planar-other', r.stderr)
        self.assertEqual(os.listdir(self.out), [])

    def test_usage_error_exits_2(self):
        r = subprocess.run([sys.executable, SCRIPT], capture_output=True)
        self.assertEqual(r.returncode, 2)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
