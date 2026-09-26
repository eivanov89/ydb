"""gRPC-only NbsDbgLike client and crash-recoverable sequential runner."""
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import uuid

from google.protobuf import json_format, text_format
from ydb.core.protos import load_test_pb2, test_shard_control_pb2 as control

Control = control.TNbsLoadControl
Result = control.TNbsLoadResult
TERMINAL = (Result.SUCCEEDED, Result.FAILED, Result.CANCELLED)


class LoadError(Exception):
    def __init__(self, message, response=None):
        super().__init__(message)
        self.response = response


class WaitTimeout(LoadError):
    pass


def as_json(message):
    # Protobuf JSON encodes all uint64 values as strings.
    return json_format.MessageToDict(message, preserving_proto_field_name=True)


def parse_config(data, fmt, allocation=False, tablet_id=None):
    message = (load_test_pb2.TEvLoadTestRequest.TNbsDbgLikeLoad.TAllocConfig()
               if allocation else load_test_pb2.TEvLoadTestRequest())
    try:
        if fmt == 'json':
            json_format.Parse(data, message)
        elif fmt == 'textproto':
            text_format.Parse(data, message)
        else:
            raise LoadError('configuration format must be json or textproto')
    except (ValueError, text_format.ParseError, json_format.ParseError) as error:
        # Never include the supplied configuration in a diagnostic.
        raise LoadError('invalid configuration for the selected protobuf schema') from error
    if allocation:
        if (not message.NumDirectBlockGroups or not message.TargetNumVChunks
                or not message.VChunkSizeBytes or message.VChunkSizeBytes % 4096
                or not 3 <= message.HostsPerDbg <= 5):
            raise LoadError('invalid allocation geometry')
        if message.HasField('TabletId'):
            raise LoadError('allocation TabletId is assigned by Hive')
        return message
    if message.WhichOneof('Command') != 'NbsDbgLikeLoad':
        raise LoadError('configuration must wrap only NbsDbgLikeLoad in TEvLoadTestRequest')
    if any(field.name != 'NbsDbgLikeLoad' for field, _ in message.ListFields()):
        raise LoadError('client-supplied service bookkeeping is forbidden')
    cmd = message.NbsDbgLikeLoad
    wc = cmd.WorkloadConfig
    if (cmd.HasField('Tag') or cmd.HasField('RequireReady') or cmd.HasField('StartupTimeoutSeconds')
            or wc.HasField('Tag') or wc.TabletConfig.HasField('ConfigurationId')):
        raise LoadError('client-supplied service bookkeeping is forbidden')
    if tablet_id is not None:
        if cmd.NbsDbgLikeTabletId or cmd.Targets:
            raise LoadError('--tablet-id is allowed only when the file supplies no targets')
        cmd.NbsDbgLikeTabletId = tablet_id
    targets = [item.TabletId for item in cmd.Targets]
    if cmd.NbsDbgLikeTabletId:
        if targets:
            raise LoadError('specify single tablet ID or Targets, not both')
        targets = [cmd.NbsDbgLikeTabletId]
    if not targets or any(not item for item in targets) or len(targets) != len(set(targets)):
        raise LoadError('nonzero unique target tablet IDs are required')
    if not wc.DurationSeconds or wc.DelayBeforeMeasurementsSeconds >= wc.DurationSeconds:
        raise LoadError('positive DurationSeconds greater than DelayBeforeMeasurementsSeconds required')
    if wc.TabletConfig.DisableReplication and wc.ReadRatio:
        raise LoadError('invalid read ratio or reads with replication disabled')
    if wc.ReadWriteSizeKiB < 4 or wc.ReadWriteSizeKiB % 4:
        raise LoadError('ReadWriteSizeKiB must be a positive multiple of 4')
    if wc.MaxInFlight <= 0:
        raise LoadError('MaxInFlight must be positive')
    return message


def read_config(path, fmt=None, allocation=False, tablet_id=None):
    if path == '-':
        if not fmt:
            raise LoadError('--config-format is required for stdin')
        data = sys.stdin.read()
    else:
        data = Path(path).read_text()
        fmt = fmt or ('json' if Path(path).suffix == '.json' else 'textproto')
    return parse_config(data, fmt, allocation, tablet_id)


def atomic_json(path, value):
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as output:
            json.dump(value, output, indent=2, sort_keys=True)
            output.write('\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class GrpcTransport:
    """Uses dstool's endpoint, CA and credential loading, with no HTTP discovery."""
    def __init__(self, params, timeout=90, clock=time.monotonic):
        self.params = params
        self.timeout = timeout
        self.clock = clock

    def __call__(self, command, deadline=None):
        import grpc
        from ydb.core.protos.grpc_pb2_grpc import TGRpcServerStub
        endpoints = list(self.params.grpc_endpoints.values())
        if not endpoints:
            raise LoadError('an explicit grpc:// or grpcs:// endpoint is required')
        request = control.TTestShardControlRequest(NbsLoadControl=command)
        if self.params.token is not None:
            request.SecurityToken = self.params.token
        # Lifecycle mutations have no run dedup key. Do not retry an ambiguous
        # delete: an operator could have recreated its owner index meanwhile.
        retryable = command.Operation in (Control.CAPABILITIES, Control.LIST, Control.DESCRIBE,
                                           Control.START, Control.GET, Control.STOP)
        attempts = endpoints if retryable else endpoints[:1]
        deadline = min(deadline, self.clock() + self.timeout) if deadline is not None else self.clock() + self.timeout
        for index, endpoint in enumerate(attempts):
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise WaitTimeout('TestShardControl RPC deadline expired; outcome may be unknown')
            request.NbsLoadControl.RpcTimeoutMs = max(1, int(remaining * 1000))
            options = [('grpc.max_receive_message_length', 256 << 20)]
            channel = (grpc.secure_channel(endpoint.host_with_grpc_port,
                       grpc.ssl_channel_credentials(self.params.get_cafile_data()), options)
                       if endpoint.protocol == 'grpcs' else
                       grpc.insecure_channel(endpoint.host_with_grpc_port, options))
            try:
                with channel:
                    remaining = deadline - self.clock()
                    if remaining <= 0:
                        raise WaitTimeout('TestShardControl RPC deadline expired; outcome may be unknown')
                    request.NbsLoadControl.RpcTimeoutMs = max(1, int(remaining * 1000))
                    response = TGRpcServerStub(channel).TestShardControl(request, timeout=remaining)
            except grpc.RpcError as error:
                if (retryable and index + 1 < len(attempts)
                        and error.code() in (grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.DEADLINE_EXCEEDED)):
                    continue
                # gRPC details and request debug strings may contain credentials.
                error_type = WaitTimeout if error.code() == grpc.StatusCode.DEADLINE_EXCEEDED else LoadError
                raise error_type('TestShardControl transport failure (%s); outcome may be unknown'
                                % error.code().name) from None
            if response.Status != 1 and response.HasField('NbsLoadControl'):
                if self.params.token:
                    response.NbsLoadControl.Error = response.NbsLoadControl.Error.replace(self.params.token, '<redacted>')
                return response.NbsLoadControl
            if response.Status != 1:
                reason = response.ErrorReason
                if self.params.token:
                    reason = reason.replace(self.params.token, '<redacted>')
                error_type = WaitTimeout if reason.startswith('control deadline exceeded;') else LoadError
                raise error_type(reason or 'TestShardControl rejected request')
            if not response.HasField('NbsLoadControl'):
                raise LoadError('server does not support NbsLoadControl; deploy server support first')
            return response.NbsLoadControl
        raise LoadError('no gRPC endpoints')


class Client:
    def __init__(self, transport, database, node_id=0, incarnation='', poll=2,
                 clock=time.monotonic, sleep=time.sleep):
        self.transport = transport
        self.database = database
        self.node_id = node_id
        self.incarnation = incarnation
        self.poll = poll
        self.clock = clock
        self.sleep = sleep

    def request(self, operation, **fields):
        return Control(Operation=operation, Database=self.database,
                       CoordinatorNodeId=self.node_id, Incarnation=self.incarnation,
                       RpcTimeoutMs=max(1, int(getattr(self.transport, 'timeout', 90) * 1000)), **fields)

    def call(self, request, deadline=None):
        if deadline is not None:
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise WaitTimeout('operation deadline expired; outcome may be unknown')
            request.RpcTimeoutMs = max(1, min(request.RpcTimeoutMs, int(remaining * 1000)))
        response = (self.transport(request, deadline=deadline) if isinstance(self.transport, GrpcTransport)
                    else self.transport(request))
        if deadline is not None and self.clock() >= deadline:
            raise WaitTimeout('operation deadline expired', response=response)
        if response.Status != 1:
            raise LoadError((response.Error or 'control request failed') +
                            ('; tablet IDs: ' + ', '.join(str(t.TabletId) for t in response.Tablets)
                             if response.Tablets else ''))
        if response.Database != self.database:
            raise LoadError('server database mismatch')
        if request.Operation != Control.CAPABILITIES:
            if response.Incarnation != self.incarnation or response.CoordinatorNodeId != self.node_id:
                raise LoadError('coordinator identity changed; remote workers may still be running')
        return response

    def pin(self, operations):
        if not self.database:
            raise LoadError('--database is required')
        response = self.call(self.request(Control.CAPABILITIES))
        if response.ProtocolVersion != 1 or not set(operations).issubset(response.Operations):
            raise LoadError('unsupported server capabilities')
        if self.incarnation and (response.Incarnation != self.incarnation or response.CoordinatorNodeId != self.node_id):
            raise LoadError('coordinator incarnation lost; remote workers may still be running')
        if not response.Incarnation or not response.CoordinatorNodeId:
            raise LoadError('server returned incomplete coordinator identity')
        self.node_id, self.incarnation = response.CoordinatorNodeId, response.Incarnation
        return response

    def get(self, request_id, deadline=None):
        response = self.call(self.request(Control.GET, RequestId=request_id), deadline)
        if not response.HasField('Run'):
            raise LoadError('server returned no run state')
        return response

    def wait(self, request_id, timeout, deadline=None):
        deadline = deadline if deadline is not None else self.clock() + timeout
        while True:
            response = self.get(request_id, deadline)
            if response.Run.State in TERMINAL:
                return response
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise WaitTimeout('run wait timed out')
            self.sleep(min(self.poll, remaining))

    def stop(self, request_id, timeout):
        deadline = self.clock() + timeout
        self.call(self.request(Control.STOP, RequestId=request_id), deadline)
        response = self.wait(request_id, timeout, deadline)
        if not response.Run.TerminationConfirmed:
            raise LoadError('termination not confirmed; remote workers may still be running', response=response)
        return response

    def ready(self, owner_index, timeout):
        deadline = self.clock() + timeout
        while True:
            response = self.call(self.request(Control.DESCRIBE, OwnerIndex=owner_index), deadline)
            if len(response.Tablets) != 1:
                raise LoadError('describe returned no tablet identity')
            summary = response.Tablets[0].Summary
            if (summary.AutomationProtocolVersion >= 1 and summary.NumDirectBlockGroups > 0
                    and summary.NumReadyDirectBlockGroups == summary.NumDirectBlockGroups):
                return response
            if self.clock() >= deadline:
                raise WaitTimeout('tablet readiness timed out; allocation retained')
            self.sleep(min(self.poll, deadline - self.clock()))


def verdict(response, allow_io_errors=False):
    run = response.Run
    if not response.HasField('Run') or run.State != Result.SUCCEEDED:
        return False
    if run.ExecutionError or not run.HasField('Stats') or not run.TerminationConfirmed:
        return False
    return allow_io_errors or not (run.Stats.WritesErr or run.Stats.ReadsErr)


class Runner:
    def __init__(self, client, directory, checkpoint):
        self.client = client
        self.directory = Path(directory)
        self.checkpoint = checkpoint

    @classmethod
    def create(cls, client, directory, config, startup=60, inflights=None, trials=1,
               allow_io_errors=False, wait_timeout=None, kind='run'):
        if trials <= 0 or (inflights is not None and (not inflights or any(n <= 0 for n in inflights))):
            raise LoadError('positive trial count and inflight values required')
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=False)
        values = inflights if inflights is not None else [config.NbsDbgLikeLoad.WorkloadConfig.MaxInFlight]
        entries = []
        for value in values:
            for trial in range(trials):
                trial_config = copy.deepcopy(config)
                if inflights is not None:
                    trial_config.NbsDbgLikeLoad.WorkloadConfig.MaxInFlight = value
                request = client.request(Control.START, RequestId=str(uuid.uuid4()),
                                         Load=trial_config, StartupTimeoutSeconds=startup)
                entries.append({'inflight': value, 'trial': trial + 1, 'state': 'prepared',
                                'request': as_json(request)})
        checkpoint = {'version': 1, 'kind': kind, 'database': client.database,
                      'node_id': client.node_id, 'incarnation': client.incarnation,
                      'poll': client.poll, 'startup': startup,
                      'rpc_timeout': getattr(client.transport, 'timeout', 90),
                      'allow_io_errors': allow_io_errors, 'wait_timeout': wait_timeout,
                      'trials': entries}
        runner = cls(client, directory, checkpoint)
        atomic_json(directory / 'config.json', as_json(config))
        runner.save()
        return runner

    @classmethod
    def resume(cls, client, directory, kind, reconcile=True):
        checkpoint = json.loads((Path(directory) / 'checkpoint.json').read_text())
        if checkpoint.get('version') != 1 or checkpoint.get('kind') != kind:
            raise LoadError('checkpoint version or command mismatch')
        client.database = checkpoint['database']
        client.node_id = checkpoint['node_id']
        client.incarnation = checkpoint['incarnation']
        client.poll = checkpoint['poll']
        if hasattr(client.transport, 'timeout'):
            client.transport.timeout = checkpoint['rpc_timeout']
        runner = cls(client, directory, checkpoint)
        if reconcile:
            runner.reconcile_results()
        return runner

    @classmethod
    def from_handle(cls, client, directory, reconcile=True):
        checkpoint = json.loads((Path(directory) / 'checkpoint.json').read_text())
        if checkpoint.get('kind') != 'run' or len(checkpoint.get('trials', [])) != 1:
            raise LoadError('use explicit request identity for a sweep trial')
        return cls.resume(client, directory, 'run', reconcile)

    def trial_request(self, index=0):
        entry = self.checkpoint['trials'][index]
        request = json_format.ParseDict(entry['request'], Control())
        if (request.Database != self.checkpoint['database']
                or request.CoordinatorNodeId != self.checkpoint['node_id']
                or request.Incarnation != self.checkpoint['incarnation']):
            raise LoadError('checkpoint request identity mismatch')
        return request

    def saved_response(self, index=0):
        entry = self.checkpoint['trials'][index]
        filename = entry.get('result')
        if not filename:
            if entry['state'] == 'complete':
                raise LoadError('completed checkpoint has no saved result')
            return None
        response = json_format.ParseDict(json.loads((self.directory / filename).read_text()),
                                         control.TNbsLoadControlResponse())
        request = self.trial_request(index)
        if (response.Database != request.Database or response.Incarnation != request.Incarnation
                or response.CoordinatorNodeId != request.CoordinatorNodeId
                or response.RequestId != request.RequestId or response.Run.State not in TERMINAL):
            raise LoadError('saved result does not match checkpoint identity or is not terminal')
        return response

    def reconcile_results(self):
        # A process can die after result fsync and before checkpoint fsync.
        # Recover that saved result without depending on server retention.
        changed = False
        for index, entry in enumerate(self.checkpoint['trials']):
            filename = 'result-%04d.json' % index
            path = self.directory / filename
            if entry['state'] == 'complete' or not path.exists():
                continue
            entry['result'], entry['state'] = filename, 'complete'
            self.saved_response(index)
            changed = True
        if changed:
            self.save()

    def save(self):
        atomic_json(self.directory / 'checkpoint.json', self.checkpoint)

    def record_result(self, index, response):
        entry = self.checkpoint['trials'][index]
        request = self.trial_request(index)
        if (response.Database != request.Database or response.Incarnation != request.Incarnation
                or response.CoordinatorNodeId != request.CoordinatorNodeId
                or response.RequestId != request.RequestId or response.Run.State not in TERMINAL):
            raise LoadError('terminal result identity mismatch')
        filename = 'result-%04d.json' % index
        # Result is fsynced before a checkpoint can claim it is complete.
        atomic_json(self.directory / filename, as_json(response))
        entry['result'] = filename
        entry['state'] = 'complete'
        self.save()

    def execute(self, emit, no_wait=False):
        allow_errors = self.checkpoint['allow_io_errors']
        for index, entry in enumerate(self.checkpoint['trials']):
            request = self.trial_request(index)
            timeout = self.checkpoint['wait_timeout'] or (
                request.StartupTimeoutSeconds + request.Load.NbsDbgLikeLoad.WorkloadConfig.DurationSeconds + 210)
            if entry['state'] == 'complete':
                response = self.saved_response(index)
            else:
                deadline = self.client.clock() + timeout
                try:
                    if entry['state'] == 'prepared':
                        entry['state'] = 'submitting'
                        self.save()
                        response = self.client.call(request, deadline)
                        entry['state'] = 'accepted'
                        self.save()
                    else:
                        # A submitting checkpoint may precede an ambiguous RPC.
                        # GET must resolve it; unknown never launches replacement work.
                        response = self.client.get(request.RequestId, deadline)
                    if no_wait:
                        if response.Run.State in TERMINAL:
                            self.record_result(index, response)
                        emit({'handle': {'Database': request.Database, 'CoordinatorNodeId': request.CoordinatorNodeId,
                                         'Incarnation': request.Incarnation, 'RequestId': request.RequestId},
                              'output_dir': str(self.directory), 'response': as_json(response)})
                        return (entry.get('outcome') is None and
                                (response.Run.State not in TERMINAL or verdict(response, allow_errors)))
                    if response.Run.State not in TERMINAL:
                        response = self.client.wait(request.RequestId, timeout, deadline)
                    self.record_result(index, response)
                except (WaitTimeout, KeyboardInterrupt) as error:
                    entry['outcome'] = 'timed_out' if isinstance(error, WaitTimeout) else 'interrupted'
                    self.save()
                    late = error.response if isinstance(error, WaitTimeout) else None
                    if late is not None and late.HasField('Run') and late.Run.State in TERMINAL:
                        self.record_result(index, late)
                        if late.Run.TerminationConfirmed:
                            raise LoadError('trial timed out; terminal results saved', response=late) from error
                    try:
                        stopped = self.client.stop(request.RequestId, request.StartupTimeoutSeconds + 210)
                        if stopped.Run.State in TERMINAL:
                            self.record_result(index, stopped)
                    except (LoadError, KeyboardInterrupt) as stopped_error:
                        if (isinstance(stopped_error, LoadError) and stopped_error.response is not None
                                and stopped_error.response.HasField('Run')
                                and stopped_error.response.Run.State in TERMINAL):
                            self.record_result(index, stopped_error.response)
                            if stopped_error.response.Run.TerminationConfirmed:
                                raise LoadError('trial timed out; cancellation completed; results saved',
                                                response=stopped_error.response) from error
                        entry['outcome'] = 'unresolved; termination not confirmed'
                        self.save()
                        raise LoadError('interrupted; cancellation outcome unresolved; resume saved handle') from None
                    raise LoadError('interrupted or timed out; cancellation completed; results saved') from error
            passed = verdict(response, allow_errors) and entry.get('outcome') not in (
                'timed_out', 'interrupted', 'unresolved; termination not confirmed')
            emit({'inflight': entry['inflight'], 'trial': entry['trial'], 'passed': passed,
                  'response': as_json(response)})
            if not passed:
                return False
        if self.checkpoint['kind'] == 'sweep':
            summaries = []
            # Preserve ordering and every trial, including repeated inflight values.
            entries = self.checkpoint['trials']
            groups = []
            for entry in entries:
                if entry['trial'] == 1:
                    groups.append([])
                response = json_format.ParseDict(json.loads((self.directory / entry['result']).read_text()),
                                                 control.TNbsLoadControlResponse())
                groups[-1].append((response.Run.WriteIOPS, entry))
            for group in groups:
                selected = sorted(group, key=lambda item: item[0])[len(group) // 2][1]
                summaries.append({'inflight': selected['inflight'], 'median_trial': selected['trial'],
                                  'result': selected['result']})
            atomic_json(self.directory / 'summary.json', summaries)
            emit({'summary': summaries})
        return True
