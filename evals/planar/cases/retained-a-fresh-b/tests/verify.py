"""Unregistered separate-verifier candidate: incomplete until host gate binding."""
import json
from pathlib import Path
from target_verifier import assess

root=Path('/workspace/home/_planar_host')
out=Path('/logs/verifier'); out.mkdir(parents=True,exist_ok=True)
result=assess(root/'source-packet',root/'candidate',root/'target')
(out/'status.json').write_text(json.dumps(result,sort_keys=True)+'\n')
(out/'reward.txt').write_text('1\n' if result.get('status')=='pass' else '0\n')
