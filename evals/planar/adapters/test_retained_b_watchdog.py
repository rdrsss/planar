import os
from pathlib import Path
from unittest import TestCase
from unittest.mock import patch
from watchdog import process_record

class RetainedWatchdog(TestCase):
    def test_retained_receives_finite_monotonic_budget(self):
        with patch.dict(os.environ, {'PLANAR_T044_CASE': 'retained-a-fresh-b', 'PLANAR_T044_ABSOLUTE_DEADLINE': '2100-01-01T00:00:00+00:00'}, clear=True):
            record = process_record(Path('/tmp/planar-id'), 123, 1050)
        self.assertEqual(record['deadline_seconds'], 1050)
        self.assertGreater(record['started_monotonic_ns'], 0)
        self.assertEqual(record['absolute_deadline'], '2100-01-01T00:00:00+00:00')
