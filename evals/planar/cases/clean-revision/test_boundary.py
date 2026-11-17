import unittest
from boundary import verify_injection

class BoundaryTests(unittest.TestCase):
    def test_exact_delta(self):
        before = {'repository/STATE.txt': b'correct\n', 'repository/RESULT.txt': b'2\n', 'repository/.git/index': b'index', 'home/task.md': b'record'}
        after = dict(before, **{'repository/STATE.txt': b'fault-injected\n'})
        self.assertEqual(verify_injection(before, after)['changed'], 'repository/STATE.txt')
        changed = dict(after, **{'home/task.md': b'changed'})
        with self.assertRaisesRegex(ValueError, 'unexpected'): verify_injection(before, changed)
        del after['repository/.git/index']
        with self.assertRaisesRegex(ValueError, 'inventory'): verify_injection(before, after)

    def test_wrong_or_unsafe_entries(self):
        before = {'repository/STATE.txt': b'correct\n'}
        with self.assertRaisesRegex(ValueError, 'STATE'): verify_injection(before, {'repository/STATE.txt': b'fault-injected'})
        with self.assertRaisesRegex(ValueError, 'invalid'): verify_injection({'../STATE': b'x'}, {'../STATE': b'x'})

if __name__ == '__main__': unittest.main()
