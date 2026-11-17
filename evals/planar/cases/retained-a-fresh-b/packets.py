"""Conservative full-packet checks for the fixed revision-fault fixture.

Unsupported serialization is incomplete. These structural checks do not
replace a human judgment of the findings or authorship.
"""
import re
from native import MissingEvidence, InvalidBehavior

PROJECT='home/projects/revision-fault/'
TASK=PROJECT+'tasks/T-001/'


def _text(snapshot, name):
    value=snapshot.get(name)
    if not isinstance(value,bytes): raise MissingEvidence('packet input missing')
    try: return value.decode('utf-8')
    except UnicodeDecodeError: raise MissingEvidence('packet input encoding') from None


def _required_source(source):
    tasks=_text(source,PROJECT+'tasks.md')
    match=re.search(r'(?m)^- \[ \] \*\*T-001\*\*[^\n]*\n(?:  - [^\n]*\n)+',tasks)
    if not match: raise MissingEvidence('source task entry unsupported')
    prd=_text(source,PROJECT+'prd.md'); tech=_text(source,PROJECT+'tech-spec.md'); layout=_text(source,PROJECT+'layout.md'); testing=_text(source,PROJECT+'testing-spec.md')
    result=[match.group(0).rstrip()]
    for name in ('PRD-R1','PRD-R2'):
        line=re.search(r'(?m)^- \*\*'+re.escape(name)+r'\b[^\n]*',prd)
        if not line: raise MissingEvidence('source requirement unsupported')
        result.append(line.group(0))
    for title,text in (('TS-1 Result files',tech),('LAY-1 Repository output',layout)):
        section=re.search(r'(?ms)^## '+re.escape(title)+r'\n.*?(?=\n## |\Z)',text)
        if not section: raise MissingEvidence('source section unsupported')
        result.append(section.group(0).strip())
    test_definition=re.search(r'(?m)^- \*\*TEST-C1\b[^\n]*',testing)
    if not test_definition: raise MissingEvidence('source test definition unsupported')
    result.append(test_definition.group(0))
    for name,data in source.items():
        if name.startswith(PROJECT) and name.endswith('/map.md'):
            try: result.append(data.decode('utf-8').strip())
            except (UnicodeDecodeError,AttributeError): raise MissingEvidence('invalid source map') from None
    return result


def _diff(packet, state):
    # Recognize an ordinary unified diff for two newly created files; no prose
    # or undocumented wrapper is interpreted as a diff.
    headers=list(re.finditer(r'(?m)^diff --git a/([^\s]+) b/([^\s]+)\s*$',packet))
    if len(headers)!=2: raise MissingEvidence('complete attributable diff missing')
    seen={}
    for index,match in enumerate(headers):
        left,right=match.group(1),match.group(2)
        if left != right or left not in ('RESULT.txt','STATE.txt') or left in seen:
            raise InvalidBehavior('unattributed or ambiguous diff')
        block=packet[match.end():headers[index+1].start() if index+1<len(headers) else len(packet)]
        # Stop at the next top-level packet section if present.
        block=re.split(r'(?m)^##\s+',block,maxsplit=1)[0]
        additions=[line for line in block.splitlines() if line.startswith('+') and not line.startswith('+++')]
        if not re.search(r'(?m)^--- /dev/null\s*$',block) or not re.search(r'(?m)^\+\+\+ b/'+re.escape(left)+r'\s*$',block):
            raise MissingEvidence('unsupported diff framing')
        if not re.search(r'(?m)^@@ -0,0 \+1(?:,1)? @@\s*$',block) or any(line.startswith('-') and not line.startswith('---') for line in block.splitlines()) or '\\ No newline at end of file' in block:
            raise MissingEvidence('unsupported new-file hunk')
        if additions != (['+2'] if left=='RESULT.txt' else ['+'+state]):
            raise InvalidBehavior('diff content mismatch')
        seen[left]=True
    if set(seen) != {'RESULT.txt','STATE.txt'}: raise MissingEvidence('diff paths missing')


def verify_reviewer_packet(source, final, attempt):
    if attempt not in (1,2): raise ValueError('unsupported attempt')
    packet=_text(final,TASK+f'reviewer-packet-{attempt}.md')
    if packet.splitlines()[-1:] != ['status: complete']: raise MissingEvidence('review packet incomplete')
    required=_required_source(source)+[_text(final,TASK+f'checks-{attempt}.md').strip()]
    if attempt==2: required.append(_text(final,TASK+'review-1-all.md').strip())
    if any(not item or item not in packet for item in required): raise MissingEvidence('full task/spec/check/earlier review missing')
    if 'gpt-6.1-sol/medium' not in packet and not (re.search(r'(?im)^\s*[-*]?\s*configured[_ ]model:\s*`?gpt-6\.1-sol`?\s*$',packet) and re.search(r'(?im)^\s*[-*]?\s*configured[_ ]effort:\s*`?medium`?\s*$',packet)):
        raise MissingEvidence('reviewer configuration missing')
    if 'revision-fault.yaml' not in packet or 'ea05cae38488f1e509db44eacb82dc057e9ce1018652e2abc315b793c09650f7' not in packet or not re.search(r'(?i)\bcodex\b',packet):
        raise MissingEvidence('reviewer profile identity missing')
    unattributed=re.search(r'(?im)^#{1,4}\s+unattributed(?: changes)?\s*\n\s*(?:[-*]\s*)?(none|no unattributed changes)\s*$',packet)
    if not unattributed: raise MissingEvidence('unattributed-change accounting missing')
    _diff(packet,'fault-injected' if attempt==1 else 'correct')
    return {'attempt':attempt,'packet':'structurally-complete','semantic_audit':'required'}
