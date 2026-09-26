import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from ydb.apps.dstool.lib import nbs_load as lib
from ydb.core.protos import test_shard_control_pb2 as proto


class Clock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        return self.now

    def sleep(self, duration):
        self.now += duration


def config():
    return lib.parse_config('NbsDbgLikeLoad { NbsDbgLikeTabletId: 42 WorkloadConfig { '
                            'DurationSeconds: 1 DelayBeforeMeasurementsSeconds: 0 } }', 'textproto')


class Server:
    def __init__(self):
        self.calls = []
        self.runs = {}
        self.io_errors = 0
        self.active = False

    def __call__(self, request, deadline=None):
        self.calls.append(request.Operation)
        response = proto.TNbsLoadControlResponse(
            Status=1, ProtocolVersion=1, Database='/Root', CoordinatorNodeId=2,
            Incarnation='inc', RequestId=request.RequestId)
        if request.Operation == lib.Control.CAPABILITIES:
            response.Operations.extend(range(8))
        elif request.Operation == lib.Control.START:
            self.runs[request.RequestId] = True
            response.Run.State = lib.Result.IN_PROGRESS
        elif request.Operation in (lib.Control.GET, lib.Control.STOP):
            if request.RequestId not in self.runs:
                raise lib.LoadError('unknown or expired')
            response.Run.State = lib.Result.IN_PROGRESS if self.active else lib.Result.SUCCEEDED
            response.Run.TerminationConfirmed = not self.active
            response.Run.Stats.WritesOk = 100
            response.Run.Stats.WritesErr = self.io_errors
            response.Run.WriteIOPS = 100
            if request.Operation == lib.Control.STOP:
                self.active = False
        return response


class NbsLoadTest(unittest.TestCase):
    def client(self, server):
        clock = Clock()
        client = lib.Client(server, '/Root', poll=2, clock=clock, sleep=clock.sleep)
        client.pin((lib.Control.START,))
        return client

    def test_formats_defaults_and_bookkeeping(self):
        original = config()
        parsed = lib.parse_config(json.dumps(lib.as_json(original)), 'json')
        self.assertEqual(original, parsed)
        self.assertEqual(parsed.NbsDbgLikeLoad.WorkloadConfig.MaxInFlight, 32)
        for text in ('Stop {}', 'Tag: 1 NbsDbgLikeLoad {}',
                     'NbsDbgLikeLoad { RequireReady: true }'):
            with self.assertRaises(lib.LoadError):
                lib.parse_config(text, 'textproto')
        original.NbsDbgLikeLoad.WorkloadConfig.MaxInFlight = 0
        original.NbsDbgLikeLoad.WorkloadConfig.DurationSeconds = 30
        for fmt, data in (('json', json.dumps(lib.as_json(original))),
                          ('textproto', 'NbsDbgLikeLoad { NbsDbgLikeTabletId: 42 WorkloadConfig { '
                           'DurationSeconds: 30 MaxInFlight: 0 } }')):
            with self.assertRaisesRegex(lib.LoadError, 'MaxInFlight'):
                lib.parse_config(data, fmt)

    def test_submitting_checkpoint_precedes_rpc_and_result_precedes_completion(self):
        server = Server()
        client = self.client(server)
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / 'run'
            runner = lib.Runner.create(client, directory, config())
            transport = client.transport

            def checked(request):
                if request.Operation == lib.Control.START:
                    saved = json.loads((directory / 'checkpoint.json').read_text())
                    self.assertEqual(saved['trials'][0]['state'], 'submitting')
                    self.assertEqual(saved['trials'][0]['request']['RequestId'], request.RequestId)
                return transport(request)

            client.transport = checked
            self.assertTrue(runner.execute(lambda _: None))
            saved = json.loads((directory / 'checkpoint.json').read_text())
            self.assertEqual(saved['trials'][0]['state'], 'complete')
            self.assertTrue((directory / saved['trials'][0]['result']).is_file())
            self.assertNotIn('SecurityToken', (directory / 'checkpoint.json').read_text())
            with self.assertRaises(FileExistsError):
                lib.Runner.create(client, directory, config())

    def test_unknown_submission_never_replaced(self):
        server = Server()
        client = self.client(server)
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / 'run'
            runner = lib.Runner.create(client, directory, config())
            runner.checkpoint['trials'][0]['state'] = 'submitting'
            runner.save()
            resumed = lib.Runner.resume(client, directory, 'run')
            with self.assertRaisesRegex(lib.LoadError, 'unknown'):
                resumed.execute(lambda _: None)
            self.assertNotIn(lib.Control.START, server.calls)

    def test_lost_reply_resume_recovers_original_without_start(self):
        server = Server()
        client = self.client(server)
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config())
            request = runner.checkpoint['trials'][0]['request']
            server.runs[request['RequestId']] = True
            runner.checkpoint['trials'][0]['state'] = 'submitting'
            runner.save()
            self.assertTrue(lib.Runner.resume(client, runner.directory, 'run').execute(lambda _: None))
            self.assertNotIn(lib.Control.START, server.calls)

    def test_sweep_order_resume_and_fail_fast(self):
        server = Server()
        client = self.client(server)
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config(), inflights=[8, 2], trials=2, kind='sweep')
            self.assertEqual([entry['inflight'] for entry in runner.checkpoint['trials']], [8, 8, 2, 2])
            server.io_errors = 1
            self.assertFalse(runner.execute(lambda _: None))
            self.assertEqual(server.calls.count(lib.Control.START), 1)
            self.assertFalse(lib.Runner.resume(client, runner.directory, 'sweep').execute(lambda _: None))
            self.assertEqual(server.calls.count(lib.Control.START), 1)

    def test_allow_errors_does_not_allow_execution_failure(self):
        response = proto.TNbsLoadControlResponse()
        response.Run.State = lib.Result.SUCCEEDED
        response.Run.Stats.WritesErr = 1
        response.Run.TerminationConfirmed = True
        self.assertFalse(lib.verdict(response))
        self.assertTrue(lib.verdict(response, True))
        response.Run.TerminationConfirmed = False
        self.assertFalse(lib.verdict(response, True))
        response.Run.State = lib.Result.FAILED
        self.assertFalse(lib.verdict(response, True))

    def test_timeout_cancels_and_does_not_start_next_trial(self):
        server = Server()
        server.active = True
        client = self.client(server)
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config(), inflights=[1, 2], kind='sweep', wait_timeout=3)
            with self.assertRaisesRegex(lib.LoadError, 'cancellation completed'):
                runner.execute(lambda _: None)
            self.assertIn(lib.Control.STOP, server.calls)
            self.assertEqual(server.calls.count(lib.Control.START), 1)
            self.assertEqual(runner.checkpoint['trials'][0]['state'], 'complete')

    def test_incarnation_loss(self):
        client = self.client(Server())
        client.incarnation = 'old'
        with self.assertRaisesRegex(lib.LoadError, 'incarnation lost'):
            client.pin((lib.Control.GET,))

    def test_uint64_json_is_string(self):
        request = lib.Control(TabletId=2**63 + 1)
        self.assertEqual(lib.as_json(request)['TabletId'], str(2**63 + 1))

    def test_command_dry_run_no_transport_and_machine_stdout(self):
        from ydb.apps.dstool.lib import dstool_cmd_nbs_load as command
        import argparse
        parser = argparse.ArgumentParser()
        command.add_options(parser)
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'run.json'
            path.write_text(json.dumps(lib.as_json(config())))
            args = parser.parse_args(['run', '--database', '/Root', '--config', str(path), '--format', 'json'])
            args.dry_run = True
            out = io.StringIO()
            with contextlib.redirect_stdout(out), patch.object(lib.GrpcTransport, '__call__', side_effect=AssertionError('network')):
                command.do(args)
            result = json.loads(out.getvalue())
            self.assertIn('request', result)
            self.assertNotIn('response', result)
            self.assertEqual(result['request']['Load']['NbsDbgLikeLoad']['NbsDbgLikeTabletId'], '42')
            bad = config()
            bad.NbsDbgLikeLoad.WorkloadConfig.DurationSeconds = 30
            bad.NbsDbgLikeLoad.WorkloadConfig.MaxInFlight = 0
            path.write_text(json.dumps(lib.as_json(bad)))
            with patch.object(lib.GrpcTransport, '__call__', side_effect=AssertionError('network')):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    command.do(args)

    def test_transport_uses_legacy_grpc_with_deadline_and_redacts_failure(self):
        import grpc
        from types import SimpleNamespace
        from ydb.core.protos import grpc_pb2_grpc
        endpoint = SimpleNamespace(host_with_grpc_port='example:2135', protocol='grpc')
        params = SimpleNamespace(grpc_endpoints={'a': endpoint}, token='secret')
        response = __import__('ydb.core.protos.msgbus_pb2', fromlist=['TResponse']).TResponse(Status=1)
        response.NbsLoadControl.Status = 1
        captured = []

        def rpc(request, timeout):
            captured.append((request, timeout))
            return response

        with patch.object(grpc, 'insecure_channel'), patch.object(grpc_pb2_grpc, 'TGRpcServerStub') as stub:
            stub.return_value.TestShardControl.side_effect = rpc
            lib.GrpcTransport(params, timeout=17)(lib.Control(Operation=lib.Control.GET))
        self.assertEqual(captured[0][0].SecurityToken, 'secret')
        self.assertGreater(captured[0][1], 0)
        self.assertLessEqual(captured[0][1], 17)
        response.ClearField('NbsLoadControl')
        response.Status = 128
        response.ErrorReason = 'rejected secret'
        with patch.object(grpc, 'insecure_channel'), patch.object(grpc_pb2_grpc, 'TGRpcServerStub') as stub:
            stub.return_value.TestShardControl.side_effect = rpc
            with self.assertRaisesRegex(lib.LoadError, '<redacted>') as caught:
                lib.GrpcTransport(params)(lib.Control(Operation=lib.Control.GET))
            self.assertNotIn('secret', str(caught.exception))

    def test_saved_result_recovers_crash_before_checkpoint_complete(self):
        server = Server()
        client = self.client(server)
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config())
            self.assertTrue(runner.execute(lambda _: None))
            runner.checkpoint['trials'][0]['state'] = 'accepted'
            del runner.checkpoint['trials'][0]['result']
            runner.save()
            server.runs.clear()  # coordinator history can disappear after the fsync
            calls_before = len(server.calls)
            resumed = lib.Runner.resume(client, runner.directory, 'run')
            self.assertTrue(resumed.execute(lambda _: None))
            self.assertEqual(len(server.calls), calls_before)

    def test_handle_reconciles_and_serves_terminal_result_offline(self):
        server = Server()
        client = self.client(server)
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config())
            self.assertTrue(runner.execute(lambda _: None))
            runner.checkpoint['trials'][0]['state'] = 'accepted'
            runner.checkpoint['trials'][0].pop('result')
            runner.save()
            offline = lib.Runner.from_handle(client, runner.directory)
            self.assertEqual(offline.checkpoint['trials'][0]['state'], 'complete')
            self.assertEqual(offline.saved_response().Run.State, lib.Result.SUCCEEDED)
            count = len(server.calls)
            self.assertTrue(offline.execute(lambda _: None))
            self.assertEqual(len(server.calls), count)
            offline.checkpoint['trials'][0]['state'] = 'accepted'
            offline.save()
            before = (offline.directory / 'checkpoint.json').read_bytes()
            lib.Runner.from_handle(client, offline.directory, reconcile=False)
            self.assertEqual((offline.directory / 'checkpoint.json').read_bytes(), before)

    def test_handle_results_persist_then_stop_reads_offline(self):
        import argparse
        from ydb.apps.dstool.lib import dstool_cmd_nbs_load as command
        server = Server()
        client = self.client(server)
        parser = argparse.ArgumentParser()
        command.add_options(parser)
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config())
            entry = runner.checkpoint['trials'][0]
            entry['state'] = 'accepted'
            runner.save()
            server.runs[entry['request']['RequestId']] = True
            args = parser.parse_args(['results', '--handle', str(runner.directory), '--format', 'json'])
            args.dry_run = False
            with patch.object(lib.GrpcTransport, '__call__', side_effect=server), contextlib.redirect_stdout(io.StringIO()):
                command.do(args)
            self.assertEqual(json.loads((runner.directory / 'checkpoint.json').read_text())['trials'][0]['state'], 'complete')
            self.assertTrue((runner.directory / 'result-0000.json').exists())
            args = parser.parse_args(['stop', '--handle', str(runner.directory), '--format', 'json'])
            args.dry_run = False
            with patch.object(lib.GrpcTransport, '__call__', side_effect=AssertionError('network')):
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    command.do(args)
            self.assertTrue(json.loads(out.getvalue())['passed'])

    def test_handle_interruption_keeps_failed_verdict(self):
        import argparse
        from ydb.apps.dstool.lib import dstool_cmd_nbs_load as command
        server = Server()
        client = self.client(server)
        parser = argparse.ArgumentParser()
        command.add_options(parser)
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config())
            entry = runner.checkpoint['trials'][0]
            entry['state'] = 'accepted'
            runner.save()
            server.runs[entry['request']['RequestId']] = True
            args = parser.parse_args(['results', '--handle', str(runner.directory), '--format', 'json'])
            args.dry_run = False

            def interrupted(request, deadline=None):
                if request.Operation == lib.Control.GET:
                    raise KeyboardInterrupt()
                return server(request)

            with patch.object(lib.GrpcTransport, '__call__', side_effect=interrupted):
                with self.assertRaises(KeyboardInterrupt):
                    command.do(args)
            self.assertEqual(json.loads((runner.directory / 'checkpoint.json').read_text())
                             ['trials'][0]['outcome'], 'interrupted')
            with patch.object(lib.GrpcTransport, '__call__', side_effect=server):
                out = io.StringIO()
                with contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
                    command.do(args)
            self.assertFalse(json.loads(out.getvalue())['passed'])

    def test_saved_result_identity_mismatch_is_rejected(self):
        client = self.client(Server())
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config())
            wrong = proto.TNbsLoadControlResponse(
                Database='/Root', CoordinatorNodeId=2, Incarnation='inc', RequestId='wrong')
            wrong.Run.State = lib.Result.SUCCEEDED
            lib.atomic_json(runner.directory / 'result-0000.json', lib.as_json(wrong))
            with self.assertRaisesRegex(lib.LoadError, 'identity'):
                lib.Runner.from_handle(client, runner.directory)

    def test_deadline_checks_late_terminal_and_caps_rpc_timeout(self):
        clock = Clock()
        observed = []

        def late(request):
            observed.append(request.RpcTimeoutMs)
            clock.now += 3
            response = proto.TNbsLoadControlResponse(
                Status=1, Database='/Root', CoordinatorNodeId=2, Incarnation='inc', RequestId=request.RequestId)
            response.Run.State = lib.Result.SUCCEEDED
            response.Run.TerminationConfirmed = True
            return response
        client = lib.Client(late, '/Root', 2, 'inc', clock=clock, sleep=clock.sleep)
        with self.assertRaises(lib.WaitTimeout) as caught:
            client.get('run', clock() + 2)
        self.assertEqual(observed, [2000])
        self.assertEqual(caught.exception.response.Run.State, lib.Result.SUCCEEDED)

    def test_late_success_keeps_timeout_verdict_after_resume(self):
        server = Server()
        client = self.client(server)
        clock = client.clock
        original = client.transport

        def late(request):
            response = original(request)
            if request.Operation == lib.Control.GET:
                clock.now += 4
            return response

        client.transport = late
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config(),
                                       inflights=[1, 2], kind='sweep', wait_timeout=3)
            with self.assertRaisesRegex(lib.LoadError, 'timed out'):
                runner.execute(lambda _: None)
            self.assertEqual(runner.checkpoint['trials'][0]['outcome'], 'timed_out')
            self.assertEqual(runner.checkpoint['trials'][0]['state'], 'complete')
            starts = server.calls.count(lib.Control.START)
            resumed = lib.Runner.resume(client, runner.directory, 'sweep')
            self.assertFalse(resumed.execute(lambda _: None))
            self.assertEqual(server.calls.count(lib.Control.START), starts)

    def test_gateway_control_deadline_is_wait_timeout(self):
        import grpc
        from types import SimpleNamespace
        from ydb.core.protos import grpc_pb2_grpc, msgbus_pb2
        endpoint = SimpleNamespace(host_with_grpc_port='example:2135', protocol='grpc')
        params = SimpleNamespace(grpc_endpoints={'a': endpoint}, token=None)
        reply = msgbus_pb2.TResponse(
            Status=128, ErrorReason='control deadline exceeded; outcome unknown, retry the same request ID')
        with patch.object(grpc, 'insecure_channel'), patch.object(grpc_pb2_grpc, 'TGRpcServerStub') as stub:
            stub.return_value.TestShardControl.return_value = reply
            with self.assertRaises(lib.WaitTimeout):
                lib.GrpcTransport(params)(lib.Control(Operation=lib.Control.START))

    def test_transport_retries_share_one_deadline(self):
        import grpc
        from types import SimpleNamespace
        from ydb.core.protos import grpc_pb2_grpc, msgbus_pb2
        clock = Clock()

        class LostReply(grpc.RpcError):
            def code(self):
                return grpc.StatusCode.UNAVAILABLE
        endpoints = {name: SimpleNamespace(host_with_grpc_port=name, protocol='grpc')
                     for name in ('a', 'b')}
        params = SimpleNamespace(grpc_endpoints=endpoints, token=None)
        reply = msgbus_pb2.TResponse(Status=1)
        reply.NbsLoadControl.Status = 1
        timeouts = []

        def rpc(request, timeout):
            timeouts.append((timeout, request.NbsLoadControl.RpcTimeoutMs))
            if len(timeouts) == 1:
                clock.now += 4
                raise LostReply()
            return reply
        with patch.object(grpc, 'insecure_channel'), patch.object(grpc_pb2_grpc, 'TGRpcServerStub') as stub:
            stub.return_value.TestShardControl.side_effect = rpc
            lib.GrpcTransport(params, timeout=10, clock=clock)(lib.Control(Operation=lib.Control.GET))
        self.assertEqual([round(value[0]) for value in timeouts], [10, 6])
        self.assertEqual([value[1] for value in timeouts], [10000, 6000])

    def test_no_wait_terminal_failure_is_saved_and_fails(self):
        server = Server()
        client = self.client(server)
        original = client.transport

        def transport(request):
            response = original(request)
            if request.Operation == lib.Control.START:
                response.Run.State = lib.Result.FAILED
                response.Run.ExecutionError = 'startup failed'
            return response

        client.transport = transport
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config())
            self.assertFalse(runner.execute(lambda _: None, no_wait=True))
            self.assertEqual(runner.checkpoint['trials'][0]['state'], 'complete')

    def test_sweep_resumes_pending_trials_and_selects_median(self):
        server = Server()
        client = self.client(server)
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config(), inflights=[4, 1], trials=3, kind='sweep')
            first = runner.checkpoint['trials'][0]
            request = lib.json_format.ParseDict(first['request'], lib.Control())
            client.call(request)
            response = client.get(request.RequestId)
            response.Run.WriteIOPS = 90
            runner.record_result(0, response)
            resumed = lib.Runner.resume(client, runner.directory, 'sweep')
            emitted = []
            self.assertTrue(resumed.execute(emitted.append))
            self.assertEqual(server.calls.count(lib.Control.START), 6)
            self.assertEqual(len(list(runner.directory.glob('result-*.json'))), 6)
            summary = json.loads((runner.directory / 'summary.json').read_text())
            self.assertEqual([row['inflight'] for row in summary], [4, 1])
            self.assertEqual(summary[0]['median_trial'], 2)
            calls_before = len(server.calls)
            self.assertTrue(lib.Runner.resume(client, runner.directory, 'sweep').execute(lambda _: None))
            self.assertEqual(len(server.calls), calls_before)

    def test_unconfirmed_cancellation_blocks_following_trial(self):
        server = Server()
        server.active = True
        client = self.client(server)
        transport = client.transport

        def never_stops(request):
            response = transport(request)
            server.active = True
            if response.HasField('Run'):
                response.Run.State = lib.Result.STOPPING
                response.Run.TerminationConfirmed = False
            return response

        client.transport = never_stops
        with tempfile.TemporaryDirectory() as root:
            runner = lib.Runner.create(client, Path(root) / 'run', config(), inflights=[1, 2], kind='sweep', wait_timeout=3)
            with self.assertRaisesRegex(lib.LoadError, 'unresolved'):
                runner.execute(lambda _: None)
            self.assertEqual(server.calls.count(lib.Control.START), 1)
            self.assertEqual(runner.checkpoint['trials'][0]['state'], 'accepted')
            self.assertEqual(runner.checkpoint['trials'][1]['state'], 'prepared')

    def test_retry_through_second_gateway_keeps_request_identity(self):
        import grpc
        from types import SimpleNamespace
        from ydb.core.protos import grpc_pb2_grpc, msgbus_pb2

        class LostReply(grpc.RpcError):
            def code(self):
                return grpc.StatusCode.UNAVAILABLE

        endpoints = {name: SimpleNamespace(host_with_grpc_port=name + ':2135', protocol='grpc')
                     for name in ('first', 'second')}
        params = SimpleNamespace(grpc_endpoints=endpoints, token='secret')
        result = msgbus_pb2.TResponse(Status=1)
        result.NbsLoadControl.Status = 1
        with patch.object(grpc, 'insecure_channel'), patch.object(grpc_pb2_grpc, 'TGRpcServerStub') as stub:
            rpc = stub.return_value.TestShardControl
            rpc.side_effect = [LostReply(), result]
            command = lib.Control(Operation=lib.Control.START, RequestId='one',
                                  CoordinatorNodeId=2, Incarnation='inc', Database='/Root')
            lib.GrpcTransport(params)(command)
            self.assertEqual(rpc.call_count, 2)
            self.assertEqual(rpc.call_args_list[0].args[0].NbsLoadControl.RequestId,
                             rpc.call_args_list[1].args[0].NbsLoadControl.RequestId)
            self.assertLessEqual(rpc.call_args_list[1].kwargs['timeout'],
                                 rpc.call_args_list[0].kwargs['timeout'])

    def test_registered_command_exposes_actions(self):
        from ydb.apps.dstool.lib import commands
        from ydb.apps.dstool.lib.arg_parser import ArgumentParser
        parser = ArgumentParser()
        subparsers = parser.add_subparsers(dest='command', required=True)
        mapping = commands.make_command_map_by_structure(subparsers)
        self.assertIn('nbs-load', mapping)
        args = parser.parse_args(['nbs-load', 'list', '--database', '/Root', '--format', 'json'])
        self.assertEqual(args.nbs_action, 'list')
