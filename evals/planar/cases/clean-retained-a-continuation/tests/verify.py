"""Network-isolated retained clean continuation structural assessment."""
import json
from pathlib import Path
from candidate import inventory
from verify_b import assess

root=Path('/workspace/home/_planar_host'); out=Path('/logs/verifier'); out.mkdir(parents=True,exist_ok=True)
try:
    candidate=inventory(root/'candidate')['sha256']
    result=assess(root/'source-packet',candidate,root/'target')
except (OSError,ValueError,KeyError) as error:
    result={'status':'incomplete','reason':str(error)}
(out/'status.json').write_text(json.dumps(result,sort_keys=True)+'\n')
(out/'reward.txt').write_text('0\n')
