import unittest
from no_fault import verify, BASE


def fixture():
    return {'home/settings.yaml': b'settings', 'repository/README.md': b'base',
            'repository/RESULT.txt': b'2\n', 'repository/STATE.txt': b'correct\n',
            'host/git-head.txt': BASE, 'host/git-stage.txt': b'stage',
            'host/git-status.txt': b'?? RESULT.txt\n?? STATE.txt\n',
            'host/repository-inventory.json': b'inventory'}


def args():
    return {'source_receipt': {'wall_ns': 1}, 'restored_receipt': {'wall_ns': 2, 'monotonic_ns': 2},
            'post_receipt': {'wall_ns': 3, 'monotonic_ns': 3}, 'source_job': 'source', 'target_job': 'target',
            'injection_exists': False}


class NoFault(unittest.TestCase):
    def test_unchanged_restoration(self):
        self.assertEqual(verify(fixture(), fixture(), fixture(), **args())['boundary'], 'restored-no-fault')

    def test_rejects_mutation_injection_and_ambiguous_provenance(self):
        for change in ('durable', 'post', 'base', 'injection', 'same_job', 'order', 'unknown_time'):
            source, restored, post, options = fixture(), fixture(), fixture(), args()
            if change == 'durable': restored['home/settings.yaml'] = post['home/settings.yaml'] = b'changed'
            elif change == 'post': post['repository/STATE.txt'] = b'fault-injected\n'
            elif change == 'base': source['host/git-head.txt'] = b'wrong\n'
            elif change == 'injection': options['injection_exists'] = True
            elif change == 'same_job': options['target_job'] = options['source_job']
            elif change == 'order': options['post_receipt']['wall_ns'] = 1
            else: options['source_receipt']['wall_ns'] = True
            with self.subTest(change=change), self.assertRaises(ValueError):
                verify(source, restored, post, **options)


class HostHandoff(unittest.IsolatedAsyncioTestCase):
    async def test_collect_save_and_no_guest_mutation(self):
        import json
        from pathlib import Path
        from tempfile import TemporaryDirectory
        from no_fault import handoff
        class Capture:
            saved = False
            def load(self, path): return fixture()
            async def collect(self, environment, cid, baseline, **kwargs):
                self.asserted = (environment,cid,baseline)
                return fixture()
            def save(self, path, files):
                self.saved = True
                (path.parent/'post-receipt.json').write_text(json.dumps({'wall_ns':3,'monotonic_ns':3}))
        with TemporaryDirectory() as tmp:
            target=Path(tmp); (target/'restored-pre-receipt.json').write_text(json.dumps({'wall_ns':2,'monotonic_ns':2}))
            capture=Capture()
            result=await handoff('env',capture,container_id='id',baseline=['baseline'],source=fixture(),
                target=target,source_receipt={'wall_ns':1},source_job='source',target_job='target')
            self.assertTrue(capture.saved); self.assertEqual(result['boundary'],'restored-no-fault')
            (target/'injection.json').write_text('{}')
            with self.assertRaises(ValueError):
                await handoff('env',capture,container_id='id',baseline=['baseline'],source=fixture(),
                    target=target,source_receipt={'wall_ns':1},source_job='source',target_job='target')
