"""Separate verifier entry point for the two bounded structural checks."""
import json
import os
from pathlib import Path
from verify_a import assess as assess_worker
from verify_b import assess as assess_revision

out = Path('/logs/verifier')
out.mkdir(parents=True, exist_ok=True)
phase = os.environ.get('PLANAR_CLEAN_PHASE')
result = assess_worker(Path('/workspace/home/_planar_host')) if phase == 'worker' else assess_revision(Path('/workspace/home/_planar_host')) if phase == 'revision' else {'status':'incomplete', 'reason':'unknown phase'}
(out/'status.json').write_text(json.dumps(result, sort_keys=True)+'\n')
(out/'reward.txt').write_text('1\n' if result.get('status') == 'pass' else '0\n')
