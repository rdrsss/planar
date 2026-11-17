"""Bounded packet-content comparison; dispatch timing needs independent evidence."""
import re


class PacketError(ValueError):
    pass


def sections(text):
    result = {}
    name = None
    lines = []
    fence = None
    for line in text.splitlines(keepends=True):
        stripped = line.rstrip('\n')
        marker = re.match(r'^\s*(`{3,}|~{3,})', stripped)
        if marker:
            token = marker.group(1)
            if fence is None: fence = token
            elif token[0] == fence[0] and len(token) >= len(fence): fence = None
        if fence is None and stripped.startswith('## ') and stripped[3:] in {'Resolved configuration', 'Task', 'Paths you may create or change', 'Cited spec sections, in full', 'Checks this task must pass', 'Change', 'Unattributed changes', 'Earlier uncommitted tasks', 'Map', 'Checks', 'Earlier reviews'}:
            if name is not None: result[name] = ''.join(lines).strip()
            name = stripped[3:]
            if name in result: raise PacketError('duplicate packet section')
            lines = []
        elif name is not None:
            lines.append(line)
    if fence is not None: raise PacketError('unclosed packet fence')
    if name is not None: result[name] = ''.join(lines).strip()
    return result


def fenced(value, language='', *, allow_markdown=False):
    tag = r'(?:markdown)?' if allow_markdown and not language else re.escape(language)
    match = re.fullmatch(r'(~{3,})' + tag + r'\n(.*?)\n\1', value, re.S)
    if not match:
        raise PacketError('missing complete fenced artifact')
    return match.group(2)


def verify(packet, worker_packet, checks, expected_diff, source=None):
    """Compare authoritative inputs; caller establishes their provenance/order."""
    if not packet.startswith('# Reviewer packet: T-001, attempt 1\n') or packet.splitlines()[-1:] != ['status: complete']:
        raise PacketError('incomplete packet')
    reviewer = sections(packet.rsplit("\nstatus: complete", 1)[0] + "\n")
    worker = sections(worker_packet.rsplit("\nstatus: complete", 1)[0] + "\n")
    shared = ('Task', 'Paths you may create or change', 'Cited spec sections, in full', 'Checks this task must pass')
    for name in shared:
        if not worker.get(name) or reviewer.get(name) != worker[name]:
            raise PacketError('missing or changed worker evidence')
    config = reviewer.get('Resolved configuration', '')
    if not all(value in config for value in ('reviewer', 'gpt-6.1-sol', 'medium', 'revision-fault.yaml', 'ea05cae38488f1e509db44eacb82dc057e9ce1018652e2abc315b793c09650f7')):
        raise PacketError('missing reviewer configuration')
    if source is not None:
        cited = reviewer['Cited spec sections, in full']
        for required in source:
            if required not in cited and required not in reviewer['Checks this task must pass'] and required not in reviewer['Task']:
                raise PacketError('approved source content missing')
    if fenced(reviewer.get('Change', ''), 'diff') != expected_diff.rstrip('\n') or not expected_diff:
        raise PacketError('current diff mismatch')
    if reviewer.get('Unattributed changes') != 'None.':
        raise PacketError('unattributed changes not empty')
    if reviewer.get('Earlier uncommitted tasks') != 'None.':
        raise PacketError('earlier tasks not empty')
    if fenced(reviewer.get('Checks', ''), allow_markdown=True) != checks.rstrip('\n'):
        raise PacketError('complete checks missing')
    # First attempt has no prior review by construction. If included, make the
    # empty history explicit rather than accepting a fabricated prior review.
    if 'Earlier reviews' in reviewer and reviewer['Earlier reviews'] != 'None.':
        raise PacketError('unexpected prior reviews')
