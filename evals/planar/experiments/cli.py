"""Frozen experiment controls and individually review-gated Codex executions."""
import argparse
import json
from pathlib import Path
import controls


def main():
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(dest='command', required=True)
    init = commands.add_parser('plan', help='resolve a version-1 JSON definition')
    init.add_argument('definition', type=Path)
    init.add_argument('output', type=Path)
    run = commands.add_parser('run', help='run or continue the no-model controls')
    run.add_argument('experiment', type=Path)
    run.add_argument('identity')
    run.add_argument('--max-new', type=int)
    run.add_argument('--retry-of')
    run.add_argument('--retry-reason')
    preflight = commands.add_parser('codex-plan', help='print review inputs for one Codex trial')
    preflight.add_argument('experiment', type=Path)
    preflight.add_argument('identity')
    preflight.add_argument('logical_id')
    live = commands.add_parser('codex-run', help='execute one separately reviewed Codex trial')
    live.add_argument('experiment', type=Path)
    live.add_argument('identity')
    live.add_argument('logical_id')
    live.add_argument('--auth-file', type=Path, required=True)
    live.add_argument('--review-file', type=Path, required=True)
    live.add_argument('--retry-of')
    live.add_argument('--retry-reason')
    batch = commands.add_parser('codex-batch', help='run unreserved trials with individually reviewed gates')
    batch.add_argument('experiment', type=Path)
    batch.add_argument('identity')
    batch.add_argument('--auth-file', type=Path, required=True)
    batch.add_argument('--gates-file', type=Path, required=True, help='JSON object mapping each pending logical ID to its gate file')
    resolve = commands.add_parser('resolve-account-stop', help='record explicit account-stop resolution')
    resolve.add_argument('experiment', type=Path)
    resolve.add_argument('identity')
    resolve.add_argument('execution_id')
    resolve.add_argument('--reason', required=True)
    report = commands.add_parser('report', help='print all-trial report')
    report.add_argument('experiment', type=Path)
    report.add_argument('identity')
    report.add_argument('--markdown', action='store_true')
    report.add_argument('--output', type=Path, help='create a new report file without overwriting')
    args = p.parse_args()
    if args.command == 'plan':
        definition = json.loads(args.definition.read_text())
        if definition.pop('schema', None) != 1:
            raise ValueError('unsupported definition schema')
        result = controls.initialize(args.output, **definition)
        print(json.dumps(result, indent=2))
    elif args.command == 'run':
        print(json.dumps({'launched': controls.run_nop(args.experiment, args.identity, max_new=args.max_new, retry_of=args.retry_of, retry_reason=args.retry_reason)}))
    elif args.command == 'codex-plan':
        import codex_run
        print(json.dumps(codex_run.preflight_one(args.experiment, args.identity, args.logical_id), indent=2))
    elif args.command == 'codex-run':
        import codex_run
        print(json.dumps(codex_run.execute_one(args.experiment, args.identity, args.logical_id, args.auth_file, args.review_file, retry_of=args.retry_of, retry_reason=args.retry_reason)))
    elif args.command == 'codex-batch':
        import codex_run
        gates = json.loads(args.gates_file.read_text())
        print(json.dumps(codex_run.execute_batch(args.experiment, args.identity, args.auth_file, gates)))
    elif args.command == 'resolve-account-stop':
        manifest = controls.validate(args.experiment, args.identity)
        if manifest['kind'] == 'injected-control-only':
            controls.resolve_nop_stop(args.experiment, args.identity, args.execution_id, args.reason)
        else:
            import codex_run
            codex_run.resolve_account_stop(args.experiment, args.identity, args.execution_id, args.reason)
        print(json.dumps({'resolved': args.execution_id}))
    else:
        result = controls.report(args.experiment, args.identity)
        content = controls.markdown(result) if args.markdown else json.dumps(result, indent=2) + '\n'
        if args.output:
            with args.output.open('x') as output:
                output.write(content)
        else:
            print(content, end='')


if __name__ == '__main__':
    main()
