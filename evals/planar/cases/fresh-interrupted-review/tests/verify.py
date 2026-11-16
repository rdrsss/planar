"""Separate verifier for bounded recorded recovery evidence."""
import json
from pathlib import Path
from assess import assess_export, unassessed_extras

status, reason = assess_export(Path('/workspace'), Path('/logs/agent'))
print(json.dumps({'status': status, 'reason': reason, 'contract': 'fresh-interrupted-review-v2-oracle-v8-linked-cleanup',
                  'unassessed_extra_evidence': unassessed_extras(Path('/workspace')),
                  'limitations': ['agent-writable native records', 'backend identity and causal writes unavailable',
                                  'semantic finding correctness and source-read compliance unassessed', 'full S63/S70 pending']}))
raise SystemExit(0 if status == 'pass' else 1 if status == 'behavioral_failure' else 2)
