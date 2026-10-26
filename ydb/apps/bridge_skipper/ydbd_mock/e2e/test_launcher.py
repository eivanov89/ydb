"""Launch the actual playground and check its advertised services and cleanup."""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

import harness


class SkipperLauncherTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="skipper_launcher_")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.run = self.root / "playground"
        self.output = self.root / "launcher.log"
        self.mock = harness.MOCK_BINARY
        self.skipper = os.path.join(harness.REPO, "ydb/apps/bridge_skipper/bridge_skipper")
        if getattr(sys, "is_standalone_binary", False):
            import yatest.common
            self.skipper = yatest.common.binary_path("ydb/apps/bridge_skipper/bridge_skipper")
        python = shutil.which("python3")
        self.assertIsNotNone(python, "the playground launcher requires Python 3")
        self.env = dict(os.environ, YDBD_MOCK_BASE_PYTHON=python, PYTHONDONTWRITEBYTECODE="1")
        self.command = [os.path.join(harness.SCRIPTS, "skipper.sh"), "--run-dir", str(self.run),
                        "--mock-binary", self.mock, "--skipper-binary", self.skipper]
        self.process = None
        self.local_http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.addCleanup(self.cleanup)

    def cleanup(self):
        if self.process is not None and self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=65)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if (self.run / "mock/topology.yaml").exists():
            subprocess.run([self.mock, "cluster", "--run-dir", str(self.run / "mock"), "down"],
                           env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)

    def start(self, extra=()):
        with self.output.open("wb") as output:
            self.process = subprocess.Popen(self.command + list(extra), env=self.env, stdout=output,
                                            stderr=subprocess.STDOUT, start_new_session=True)

    def text(self):
        return self.output.read_text()

    def wait_ready(self):
        deadline = time.monotonic() + 80
        while time.monotonic() < deadline and self.process.poll() is None:
            if "Press Ctrl+C" in self.text():
                return
            time.sleep(0.1)
        self.fail("launcher did not become ready:\n" + self.text())

    def assert_closed(self, ports):
        for port in ports:
            with socket.socket() as sock:
                sock.settimeout(1)
                self.assertNotEqual(sock.connect_ex(("127.0.0.1", port)), 0, "listener left running: %s" % port)

    def test_links_controls_and_ctrl_c_cleanup(self):
        self.start()
        self.wait_ready()
        text = self.text()
        # Every printed GET link works; POST-only URLs are labeled separately.
        urls = re.findall(r"http://127\.0\.0\.1:\d+/(?:api/v1/(?:state|events|manifest)|api/state|state|events)(?=\s|$)", text)
        self.assertEqual(len(urls), 8, text)
        for url in urls:
            with self.local_http.open(url, timeout=10) as response:
                self.assertEqual(response.status, 200, url)
        manifest = json.loads((self.run / "mock/endpoint_manifest.json").read_text())
        config = json.loads((self.run / "skipper.json").read_text())
        self.assertEqual(config["cluster_id"], manifest["cluster_id"])
        self.assertEqual(len(config["ydb_endpoints"]), 6)
        # Execute the advertised request through the real supervisor API.
        curl = next(line.strip() for line in text.splitlines() if line.strip().startswith("curl "))
        args = shlex.split(curl)
        payload = json.loads(args[args.index("-d") + 1])
        for action in ("kill", "revive"):
            payload["action"] = action
            request = urllib.request.Request(args[-1], data=json.dumps(payload).encode(),
                                             headers={"Content-Type": "application/json"})
            with self.local_http.open(request, timeout=30) as response:
                self.assertEqual(response.status, 200)
        self.process.send_signal(signal.SIGINT)
        self.assertEqual(self.process.wait(timeout=65), 0, self.text())
        ports = {int(re.search(r":(\d+)/", url).group(1)) for url in urls}
        ports.update(int(node["dial_endpoint"].rsplit(":", 1)[1]) for node in manifest["nodes"])
        self.assert_closed(ports)
        self.assertTrue((self.run / "state/skipper.sqlite3").is_file())

    def test_failed_skipper_start_cleans_up_mocks(self):
        self.start(["--skipper-binary", "/usr/bin/false"])
        self.assertEqual(self.process.wait(timeout=80), 1, self.text())
        manifest = json.loads((self.run / "mock/endpoint_manifest.json").read_text())
        ports = [int(node["dial_endpoint"].rsplit(":", 1)[1]) for node in manifest["nodes"]]
        ports += [int(pile["control_endpoint"].rsplit(":", 1)[1]) for pile in manifest["piles"]]
        self.assert_closed(ports)

    def test_existing_run_directory_is_preserved(self):
        self.run.mkdir()
        marker = self.run / "keep"
        marker.write_text("existing state")
        self.start()
        self.assertEqual(self.process.wait(timeout=10), 2, self.text())
        self.assertEqual(marker.read_text(), "existing state")
        self.assertFalse((self.run / "mock").exists())
