"""One-command local playground using the built mock and Skipper binaries."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


REPO = Path(__file__).resolve().parents[5]


def port(value):
    number = int(value)
    if not 0 <= number <= 65535:
        raise argparse.ArgumentTypeError("port must be between 0 and 65535 (0 selects a free port)")
    return number


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--http-port", type=port, default=0, help="Skipper HTTP API port (default: a free port)")
    parser.add_argument("--supervisor-port", type=port, default=0, help="mock supervisor HTTP port (default: a free port)")
    parser.add_argument("--piles", default="pile1,pile2", help="comma-separated pile names")
    parser.add_argument("--nodes-per-pile", type=positive, default=3)
    parser.add_argument("--run-dir", type=Path, help="new or empty playground directory (default: a new temporary directory)")
    parser.add_argument("--mock-binary", type=Path, default=Path(os.environ.get(
        "YDBD_MOCK_EXE", REPO / "ydb/apps/bridge_skipper/ydbd_mock/ydbd_mock")))
    parser.add_argument("--skipper-binary", type=Path, default=Path(os.environ.get(
        "BRIDGE_SKIPPER_EXE", REPO / "ydb/apps/bridge_skipper/bridge_skipper")))
    args = parser.parse_args()
    piles = args.piles.split(",")
    if any(not name or name in (".", "..") or "/" in name for name in piles) or len(set(piles)) != len(piles):
        parser.error("--piles must contain unique nonempty pile names without path separators")
    for binary in (args.mock_binary, args.skipper_binary):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("executable not found: %s; build ydb/apps/bridge_skipper and "
                         "ydb/apps/bridge_skipper/ydbd_mock with the repository build wrapper" % binary)
    if args.run_dir is not None and args.run_dir.exists() and (
            not args.run_dir.is_dir() or any(args.run_dir.iterdir())):
        parser.error("--run-dir must be new or empty; existing state is never reset")
    root = args.run_dir.resolve() if args.run_dir else Path(tempfile.mkdtemp(prefix="bridge-skipper-"))
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    mock = [str(args.mock_binary.resolve()), "cluster", "--run-dir", str(root / "mock")]
    env = dict(os.environ, YDBD_MOCK_EXE=str(args.mock_binary.resolve()), PYTHONDONTWRITEBYTECODE="1")
    stopping = False

    def interrupted(signum, frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    skipper = None
    supervisor = None
    mock_started = False
    result = 0
    print("Run directory: %s" % root, flush=True)
    with contextlib.ExitStack() as stack:
        def reserve(requested=0):
            sock = stack.enter_context(socket.socket())
            sock.bind(("127.0.0.1", requested))
            return sock.getsockname()[1]

        try:
            # Hold every reservation until the topology is complete so no two
            # listeners in this launch accidentally receive the same free port.
            http_port = reserve(args.http_port)
            supervisor_port = reserve(args.supervisor_port)
            controls = [reserve() for _ in piles]
            nodes = [[reserve() for _ in range(args.nodes_per_pile)] for _ in piles]
            write_json(root / "topology.json", {"piles": piles, "nodes_per_pile": args.nodes_per_pile,
                                               "control_ports": controls, "grpc_ports": nodes})
            stack.close()
            with contextlib.ExitStack() as logs:
                mock_log = logs.enter_context((root / "mock-supervisor.log").open("ab"))
                skipper_log = logs.enter_context((root / "skipper.log").open("ab"))
                local_http = urllib.request.build_opener(urllib.request.ProxyHandler({}))

                def ready(process, url):
                    deadline = time.monotonic() + 40
                    while not stopping and process.poll() is None and time.monotonic() < deadline:
                        try:
                            with local_http.open(url, timeout=1) as response:
                                if response.status == 200:
                                    return
                        except (OSError, urllib.error.URLError):
                            pass
                        time.sleep(0.1)
                    if not stopping:
                        raise RuntimeError("service did not become ready: %s; see logs in %s" % (url, root))

                mock_started = True
                # Let up finish its bounded startup even after Ctrl+C; its
                # detached pile children are then removed by cluster down.
                subprocess.run(mock + ["up", "--topology", str(root / "topology.json")], env=env,
                               stdout=mock_log, stderr=subprocess.STDOUT, check=True, timeout=40,
                               start_new_session=True)
                if stopping:
                    raise KeyboardInterrupt
                manifest = json.loads((root / "mock/endpoint_manifest.json").read_text())
                write_json(root / "skipper.json", {
                    "schema_version": 1, "cluster_id": manifest["cluster_id"],
                    "ydb_endpoints": [("grpcs://" if node["tls"] else "grpc://") + node["dial_endpoint"]
                                      for node in manifest["nodes"]],
                    "http": {"listen": "127.0.0.1:%d" % http_port},
                    "evidence": {"profile": "global_bsc_v1"},
                    "polling": {"config_interval_seconds": 1}})
                supervisor_url = "http://127.0.0.1:%d" % supervisor_port
                supervisor = subprocess.Popen(mock + ["serve", "--port", str(supervisor_port)], env=env,
                                              stdout=mock_log, stderr=subprocess.STDOUT, start_new_session=True)
                ready(supervisor, supervisor_url + "/api/v1/manifest")
                if stopping:
                    raise KeyboardInterrupt
                skipper_url = "http://127.0.0.1:%d" % http_port
                skipper = subprocess.Popen([str(args.skipper_binary.resolve()), "--config", str(root / "skipper.json"),
                                            "--state-dir", str(root / "state"), "--member-id", "local-playground"],
                                           env=env, stdout=skipper_log, stderr=subprocess.STDOUT, start_new_session=True)
                ready(skipper, skipper_url + "/api/state")
                if stopping:
                    raise KeyboardInterrupt
                print("\nSkipper state:     %s/api/state" % skipper_url)
                print("Mock cluster:      %s/api/v1/state" % supervisor_url)
                print("Mock event log:    %s/api/v1/events" % supervisor_url)
                print("Mock manifest:     %s/api/v1/manifest" % supervisor_url)
                for pile in manifest["piles"]:
                    base = "http://" + pile["control_endpoint"]
                    print("\n%s state: %s/state" % (pile["name"], base))
                    print("  events: %s/events" % base)
                    print("  control API (POST JSON): %s/control" % base)
                    for node in manifest["nodes"]:
                        if node["pile"] == pile["name"]:
                            print("  %s gRPC: %s" % (node["label"], node["dial_endpoint"]))
                body = json.dumps({"pile": piles[-1], "action": "kill"})
                print("\nStop a mock pile (use action=revive to restart it):")
                print("  curl -sS -H 'Content-Type: application/json' -d %s %s/api/v1/process" % (
                    shlex.quote(body), supervisor_url))
                print("\nSkipper starts in Manual. Confirm ownership in Requested mode to enable actions.")
                print("Auto remains unqualified. Logs and state: %s" % root)
                print("Press Ctrl+C to stop Skipper, the supervisor and this mock cluster.\n", flush=True)
                while not stopping:
                    if skipper.poll() is not None or supervisor.poll() is not None:
                        raise RuntimeError("a service exited; see logs in %s" % root)
                    time.sleep(0.2)
        except KeyboardInterrupt:
            pass
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            print("skipper.sh: %s" % error, file=sys.stderr)
            result = 1
        finally:
            stop(skipper)
            stop(supervisor)
            if mock_started:
                try:
                    with (root / "mock-supervisor.log").open("ab") as log:
                        subprocess.run(mock + ["down"], env=env, stdout=log, stderr=subprocess.STDOUT,
                                       check=True, timeout=30, start_new_session=True)
                except (OSError, subprocess.SubprocessError) as error:
                    print("Mock cleanup failed: %s. Run: %s down" % (error, shlex.join(mock)), file=sys.stderr)
                    result = 1
            print("Stopped. Logs and state retained in %s" % root, flush=True)
    return result


if __name__ == "__main__":
    sys.exit(main())
