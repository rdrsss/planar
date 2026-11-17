import unittest
from no_fault import receipt, verify


class NoFaultTests(unittest.TestCase):
    def test_equal_correct_snapshots_and_receipt(self):
        pre = {'repository/RESULT.txt': b'2\n', 'repository/STATE.txt': b'correct\n', 'other': b'unchanged'}
        value = receipt(pre, dict(pre))
        verify(value, pre, dict(pre))
        self.assertEqual(value['mode'], 'none')
        self.assertEqual(value['pre_products'], value['post_products'])
        with self.assertRaises(ValueError):
            verify({**value, 'mode': 'injected'}, pre, dict(pre))

    def test_changed_or_wrong_bytes_fail(self):
        pre = {'repository/RESULT.txt': b'2\n', 'repository/STATE.txt': b'correct\n', 'other': b'unchanged'}
        for post in ({**pre, 'repository/STATE.txt': b'fault-injected\n'}, {**pre, 'other': b'changed'}):
            with self.subTest(post=post), self.assertRaises(ValueError):
                receipt(pre, post)
        with self.assertRaises(ValueError):
            receipt({}, {})
