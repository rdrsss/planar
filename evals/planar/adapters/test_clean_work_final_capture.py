import json
import shutil
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from clean_work_evidence import validate_final_capture
from retained_b_early_evidence import read_safe


class FinalCapture(TestCase):
    def test_real_producer_capture_and_mutations(self):
        proof=Path('/Users/mn/.planar/projects/planar/reviews/t045-clean-work-native-nop-v4-20261117')
        source=next((proof/'jobs').iterdir())/'clean-revision'
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); clean=root/'clean-revision'; clean.mkdir(); shutil.copytree(source/'final',clean/'final')
            for name in ('final-receipt.json','container.json','active.json'): shutil.copy2(source/name,clean/name)
            read=lambda path:read_safe(path,root)
            audited=validate_final_capture(clean,proof/'case',read)
            self.assertIn(str(clean/'final-receipt.json'),audited); self.assertGreater(len(audited),10)
            receipt=clean/'final-receipt.json'; saved=receipt.read_bytes(); data=json.loads(saved); data['manifest_sha256']='0'*64; receipt.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError,'receipt'): validate_final_capture(clean,proof/'case',read)
            receipt.write_bytes(saved)
            extra=clean/'final'/'unexpected'; extra.write_text('extra')
            with self.assertRaisesRegex(ValueError,'inventory'): validate_final_capture(clean,proof/'case',read)
            extra.unlink()
            target=clean/'final'/'host/git-head.txt'; before=target.read_bytes(); target.write_bytes(before+b'x')
            with self.assertRaisesRegex(ValueError,'content'): validate_final_capture(clean,proof/'case',read)
            target.write_bytes(before)
            container=clean/'container.json'; saved=container.read_bytes(); data=json.loads(saved); data['baseline']=[]; container.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError,'boundary'): validate_final_capture(clean,proof/'case',read)
