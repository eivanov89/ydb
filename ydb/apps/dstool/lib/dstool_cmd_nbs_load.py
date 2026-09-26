"""Thin argparse adapter; all workflow and recovery logic lives in nbs_load."""
import json
import math
from pathlib import Path
import sys
import uuid

from ydb.apps.dstool.lib import common, nbs_load as lib


description = 'Manage dedicated NbsDbgLike load tablets and recoverable gRPC runs'


def positive(value):
    import argparse
    number = float(value)
    if number <= 0 or not math.isfinite(number):
        raise argparse.ArgumentTypeError('must be positive and finite')
    return number


def add_options(parser):
    sub = parser.add_subparsers(dest='nbs_action', required=True)
    for action in ('create', 'list', 'describe', 'delete', 'run', 'results', 'stop', 'sweep'):
        p = sub.add_parser(action)
        p.add_argument('--database')
        p.add_argument('--node-id', type=int)
        p.add_argument('--rpc-timeout', type=positive)
        p.add_argument('--startup-timeout', type=int)
        p.add_argument('--poll-interval', type=positive)
        p.add_argument('--wait-timeout', type=positive)
        p.add_argument('--format', choices=('pretty', 'json', 'jsonl') if action == 'sweep' else ('pretty', 'json'), default='pretty')
        if action in ('create', 'describe', 'delete'):
            p.add_argument('--owner-index', type=int, required=True)
        if action in ('create', 'run', 'sweep'):
            p.add_argument('--config')
            p.add_argument('--config-format', choices=('textproto', 'json'))
        if action in ('run', 'sweep'):
            p.add_argument('--tablet-id', type=int)
            p.add_argument('--output-dir')
            p.add_argument('--resume')
            p.add_argument('--allow-io-errors', action='store_true', default=None)
        if action == 'run':
            p.add_argument('--no-wait', action='store_true')
        if action == 'sweep':
            p.add_argument('--inflights', help='ordered comma-separated positive inflight values')
            p.add_argument('--trials', type=int)
        if action in ('results', 'stop'):
            p.add_argument('--request-id')
            p.add_argument('--incarnation')
            p.add_argument('--handle', help='run artifact directory (single run only)')
            p.add_argument('--allow-io-errors', action='store_true', default=None)
        if action == 'results':
            p.add_argument('--wait', action='store_true')


def pretty(value):
    if 'summary' in value:
        return '\n'.join('inflight=%s median trial=%s result=%s' % (row['inflight'], row['median_trial'], row['result'])
                         for row in value['summary'])
    if 'handle' in value:
        return 'Handle: %s\nArtifacts: %s' % (json.dumps(value['handle']), value['output_dir'])
    response = value.get('response', value)
    if 'Run' in response:
        run = response['Run']
        return ('state=%s passed=%s write IOPS=%s read IOPS=%s write errors=%s read errors=%s%s' % (
            run.get('State', 'IN_PROGRESS'), value.get('passed', 'pending'), run.get('WriteIOPS', 0),
            run.get('ReadIOPS', 0), run.get('Stats', {}).get('WritesErr', '0'),
            run.get('Stats', {}).get('ReadsErr', '0'),
            (' error=' + run['ExecutionError']) if run.get('ExecutionError') else ''))
    return json.dumps(value, indent=2)


def do(args):
    try:
        _do(args)
    except (lib.LoadError, ValueError, OSError) as error:
        # stdout remains either valid JSON/JSONL or human-oriented result output.
        print('nbs-load: %s' % error, file=sys.stderr)
        raise SystemExit(1)


def _do(args):
    action = args.nbs_action
    values = []

    def emit(value):
        if args.format == 'json':
            values.append(value)
        elif args.format == 'jsonl':
            print(json.dumps(value, sort_keys=True), flush=True)
        else:
            print(pretty(value), flush=True)

    def flush():
        if args.format == 'json':
            print(json.dumps(values if action == 'sweep' else (values[0] if len(values) == 1 else values), sort_keys=True))

    if args.startup_timeout is not None and args.startup_timeout <= 0:
        raise lib.LoadError('--startup-timeout must be positive')
    client = lib.Client(lib.GrpcTransport(common.connection_params, args.rpc_timeout or 90),
                        args.database, args.node_id or 0, poll=args.poll_interval or 2)
    if action in ('run', 'sweep'):
        if args.resume:
            conflicts = ('config', 'config_format', 'tablet_id', 'output_dir', 'database', 'node_id',
                         'startup_timeout', 'poll_interval', 'wait_timeout', 'allow_io_errors', 'inflights', 'trials', 'rpc_timeout')
            if any(getattr(args, name, None) is not None for name in conflicts):
                raise lib.LoadError('--resume uses saved settings; new run settings are forbidden')
            runner = lib.Runner.resume(client, args.resume, action, reconcile=not args.dry_run)
            if args.dry_run:
                emit({'checkpoint': runner.checkpoint})
                flush()
                return
            if any(entry['state'] != 'complete' for entry in runner.checkpoint['trials']):
                client.pin((lib.Control.START, lib.Control.GET, lib.Control.STOP))
        else:
            if not args.config or not args.database:
                raise lib.LoadError('--config and --database are required')
            config = lib.read_config(args.config, args.config_format, tablet_id=args.tablet_id)
            inflights = None
            trials = 1
            if action == 'sweep':
                if not args.inflights or not args.trials:
                    raise lib.LoadError('sweep requires --inflights and positive --trials')
                inflights = [int(item) for item in args.inflights.split(',')]
                trials = args.trials
                if trials <= 0 or any(item <= 0 for item in inflights):
                    raise lib.LoadError('inflight values and trials must be positive')
            if args.dry_run:
                emit({'request': lib.as_json(client.request(lib.Control.START, Load=config,
                      StartupTimeoutSeconds=args.startup_timeout or 60)), 'inflights': inflights, 'trials': trials})
                flush()
                return
            client.pin((lib.Control.START, lib.Control.GET, lib.Control.STOP))
            runner = lib.Runner.create(client, args.output_dir or str(Path('nbs-load-results') / str(uuid.uuid4())),
                                      config, args.startup_timeout or 60, inflights, trials,
                                      bool(args.allow_io_errors), args.wait_timeout, action)
        print('Artifacts: %s' % runner.directory, file=sys.stderr)
        try:
            passed = runner.execute(emit, getattr(args, 'no_wait', False))
        finally:
            flush()
        if not passed:
            raise SystemExit(1)
        return

    if action in ('results', 'stop'):
        runner = None
        if args.handle:
            if args.request_id or args.incarnation or args.database or args.node_id is not None:
                raise lib.LoadError('--handle conflicts with explicit run identity')
            runner = lib.Runner.from_handle(client, args.handle, reconcile=not args.dry_run)
            if args.rpc_timeout is not None:
                client.transport.timeout = args.rpc_timeout
            request = runner.trial_request()
            request_id = request.RequestId
        else:
            if not all((args.database, args.node_id, args.incarnation, args.request_id)):
                raise lib.LoadError('provide --handle or --database, --node-id, --incarnation and --request-id')
            client.incarnation = args.incarnation
            request_id = args.request_id
        request = client.request(lib.Control.STOP if action == 'stop' else lib.Control.GET, RequestId=request_id)
        if args.dry_run:
            emit({'request': lib.as_json(request)})
        else:
            response = runner.saved_response() if runner else None
            if response is None:
                client.pin((request.Operation,))
                try:
                    if action == 'stop':
                        response = client.stop(request_id, args.wait_timeout or 270)
                    elif args.wait:
                        timeout = args.wait_timeout or runner.checkpoint['wait_timeout'] if runner else args.wait_timeout
                        if not timeout:
                            timeout = (runner.checkpoint['startup'] +
                                       runner.trial_request().Load.NbsDbgLikeLoad.WorkloadConfig.DurationSeconds + 210
                                       if runner else 270)
                        deadline = client.clock() + timeout
                        current = client.get(request_id, deadline)
                        response = (current if current.Run.State in lib.TERMINAL else
                                    client.wait(request_id, timeout, deadline))
                    else:
                        response = client.get(request_id)
                except lib.LoadError as error:
                    if runner and isinstance(error, lib.WaitTimeout):
                        runner.checkpoint['trials'][0]['outcome'] = 'timed_out'
                        runner.save()
                    if (runner and error.response is not None and error.response.HasField('Run')
                            and error.response.Run.State in lib.TERMINAL):
                        runner.record_result(0, error.response)
                    raise
                except KeyboardInterrupt:
                    if runner:
                        runner.checkpoint['trials'][0]['outcome'] = 'interrupted'
                        runner.save()
                    raise
                if runner and response.Run.State in lib.TERMINAL:
                    runner.record_result(0, response)
            allow_errors = (args.allow_io_errors if args.allow_io_errors is not None else
                            runner.checkpoint['allow_io_errors'] if runner else False)
            passed = (lib.verdict(response, allow_errors) and
                      (not runner or runner.checkpoint['trials'][0].get('outcome') not in
                       ('timed_out', 'interrupted', 'unresolved; termination not confirmed')))
            emit({'response': lib.as_json(response), 'passed': passed if response.Run.State in lib.TERMINAL else None})
            flush()
            if response.Run.State in lib.TERMINAL and not passed:
                raise SystemExit(1)
            return
        flush()
        return

    if not args.database:
        raise lib.LoadError('--database is required')
    op = {'create': lib.Control.CREATE, 'list': lib.Control.LIST,
          'describe': lib.Control.DESCRIBE, 'delete': lib.Control.DELETE}[action]
    fields = {}
    if hasattr(args, 'owner_index'):
        if args.owner_index < 0:
            raise lib.LoadError('--owner-index must be nonnegative')
        fields['OwnerIndex'] = args.owner_index
    if action == 'create':
        if not args.config:
            raise lib.LoadError('--config is required')
        fields['Allocation'] = lib.read_config(args.config, args.config_format, allocation=True)
    if args.dry_run:
        emit({'request': lib.as_json(client.request(op, **fields))})
    else:
        client.pin((op,))
        response = client.call(client.request(op, **fields))
        if action == 'create':
            response = client.ready(args.owner_index, args.startup_timeout or 60)
        emit(lib.as_json(response))
    flush()
