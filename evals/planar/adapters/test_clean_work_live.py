import json
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase
from unittest.mock import patch
import clean_work_live as live


class CleanWorkLive(TestCase):
    def test_cleanup_and_fresh_census_fail_closed(self):
        with TemporaryDirectory() as tmp:
            root=Path(tmp).resolve(); jobs=root/'jobs'; eid='planar-t044-'+'a'*32; evidence=jobs/eid; evidence.mkdir(parents=True)
            def write(name,obj): (evidence/name).write_text(json.dumps(obj))
            for name in ('launcher-terminal.json','watchdog-terminal.json'): write(name,{'execution_id':eid,'status':'completed','return_code':0})
            write('harbor-process.json',{'execution_id':eid,'pid':12345})
            write('planar-cleanup.json',{'execution_id':eid,'paths':['/tmp/codex-secrets','/installed-agent/codex-home'],'status':'passed'})
            with patch.object(live.launch,'reviews_root',return_value=root),patch.object(live.launch,'jobs_root',return_value=jobs),patch.object(live.os,'killpg',side_effect=ProcessLookupError),patch.object(live,'check_projects',return_value={'registered_projects':['one'],'queried_projects':['one','two','three']}),patch.object(live.retained_b_census,'check',return_value={'matching_commands':0,'unresolved':0}):
                self.assertEqual(live.check(evidence,'case__x',{})['native_group'],'absent')
                write('watchdog-terminal.json',{'execution_id':eid,'status':'completed','return_code':False})
                with self.assertRaisesRegex(ValueError,'supervisor'): live.check(evidence,'case__x',{})
                write('watchdog-terminal.json',{'execution_id':eid,'status':'completed','return_code':0})
                with patch.object(live.retained_b_census,'check',return_value={'matching_commands':0,'unresolved':1}):
                    with self.assertRaisesRegex(ValueError,'census'): live.check(evidence,'case__x',{})
                (evidence/'recovery-required').touch()
                with self.assertRaisesRegex(ValueError,'recovery'): live.check(evidence,'case__x',{})
