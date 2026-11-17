import unittest
from unittest.mock import patch

import recovery_control_run


class RetiredControlTests(unittest.TestCase):
    def test_entrypoint_refuses_before_filesystem_or_child(self):
        for variant, error in [('invalid', ValueError), ('positive', RuntimeError),
                               ('changed-check', RuntimeError), ('recorded-live', RuntimeError)]:
            with self.subTest(variant=variant), patch('pathlib.Path.mkdir', side_effect=AssertionError('mkdir')), \
                    patch('subprocess.run', side_effect=AssertionError('child')):
                with self.assertRaises(error):
                    recovery_control_run.run(variant)
