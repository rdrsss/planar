"""Offline compatibility findings, not an approval or candidate freeze."""
import hashlib
from pathlib import Path


def inspect(candidate, fixture):
    candidate, fixture = Path(candidate), Path(fixture)
    packets = (candidate/'references/packets.md').read_text()
    records = (candidate/'references/task-record.md').read_text()
    packet = (fixture/'home/projects/revision-fault/tasks/T-001/reviewer-packet-1.md').read_text()
    worker = (fixture/'home/projects/revision-fault/tasks/T-001/worker-packet-1.md').read_text()
    profile = (fixture/'home/profiles/revision-fault.yaml').read_bytes()
    required = ('## Resolved configuration', '## Task', '## Paths you may create or change',
                '## Cited spec sections, in full', '## Checks this task must pass',
                '## Change', '## Unattributed changes', '## Earlier uncommitted tasks', '## Checks')
    missing = [section for section in required if section+'\n' not in packet]
    if '## Checks this task must pass' not in packets or 'Packet preflight evidence' not in records:
        raise ValueError('unsupported current candidate contracts')
    if '## Checks this task must pass' not in worker:
        raise ValueError('worker packet lacks reconstruction input')
    if not missing:
        positions = [packet.index(section+'\n') for section in required]
        if positions != sorted(positions): raise ValueError('reviewer packet outline out of order')
        definition = worker.split('## Checks this task must pass\n',1)[1].rsplit('\nstatus: complete',1)[0]
        if packet.count('## Checks this task must pass\n'+definition) != 1:
            raise ValueError('reviewer check definition mismatch')
        spec=(fixture/'home/projects/revision-fault/testing-spec.md').read_text()
        source_line=next((line for line in spec.splitlines() if line.startswith('- **TEST-C1 ')), None)
        if not source_line or source_line not in definition:
            raise ValueError('check definition differs from source spec')
        if hashlib.sha256(profile).hexdigest() not in packet or 'gpt-6.1-sol' not in packet or 'gpt-6.1-terra' not in packet:
            raise ValueError('packet configuration evidence mismatch')
        if '08bacc3' not in packet or '+fault-injected' not in packet or '+2' not in packet:
            raise ValueError('packet base or diff mismatch')
    return {'profile_sha256':hashlib.sha256(profile).hexdigest(), 'missing_packet_sections':missing,
            'historical_preflight':'absent', 'immutable_replacement_required':bool(missing),
            'compatibility':'v2 packet structure and retained check definition validated; preflight remains required' if not missing else 'unverified until replacement and preflight are validated against frozen candidate'}
