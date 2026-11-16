"""Deterministic synthetic transformation of the retained v1 reviewer packet."""


def derive_packet(original, worker):
    text = original.decode('utf-8')
    source = worker.decode('utf-8')
    if text.count('\n## Change') != 1 or '## Checks this task must pass' in text:
        raise ValueError('unexpected historical packet')
    if source.count('## Checks this task must pass\n') != 1 or '\nstatus: complete' not in source:
        raise ValueError('missing complete retained check definition')
    definition = source.split('## Checks this task must pass\n',1)[1].rsplit('\nstatus: complete',1)[0]
    return text.replace('\n## Change','\n## Checks this task must pass\n'+definition+'\n## Change',1).encode('utf-8')
