#!/usr/bin/env python3
"""Offline tests for `tools/cpd.py` serve mode. No hardware, no pyserial needed.

Run:  python tools/test_cpd_serve.py [-v]

The device is replaced by FakeDevice, injected through cpd._SERIAL_FACTORY, which
speaks the real framed protocol (<<<BEGIN id cmd=name ... <<<END id rc=n ...),
grows its uptime on every command, and can be told to drop the port mid-command
or to be absent for a while. The daemon's socket is a real 127.0.0.1 socket.
"""

import contextlib
import io
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpd  # noqa: E402


class FakeDevice:
    """A model of the board's USB-Serial/JTAG port, good enough for these paths."""

    def __init__(self, start_uptime_ms=100_000, boot_ms=6800):
        self.lock = threading.Lock()
        self.uptime_ms = start_uptime_ms
        self.boot_ms = boot_ms
        self.opened = 0                 # successful opens
        self.closed = 0
        self.commands = []              # every command line received
        self.ids_answered = []
        self.booted = True              # False => open() succeeds but nothing answers
        self.drop_next_read = False     # raise on the next read (a port that dies)
        self.absent = False             # open() raises
        self.rx = bytearray()
        self.pending = None             # frame id written but not answered
        self.interleaved = []           # writes that arrived while a frame was in flight
        self.open_failures_left = 0

    # -- serial surface ---------------------------------------------------
    def open(self, port, baud, timeout, write_timeout):
        with self.lock:
            if self.absent:
                raise OSError("could not open port %s: FileNotFoundError(2)" % port)
            if self.open_failures_left > 0:
                self.open_failures_left -= 1
                raise OSError("could not open port %s: Access is denied" % port)
            self.opened += 1
            self.rx.clear()
            self.pending = None
            # A port that is (re)opened after a loss is a board that just booted.
            if self.opened > 1:
                self.uptime_ms = 10
                self.rx += b"\x1b[0mESP-ROM:esp32s31\r\n[CP] loop tick #0 t=120 ms\r\n"
            return FakeSerial(self)

    def close(self):
        with self.lock:
            self.closed += 1

    # -- protocol ---------------------------------------------------------
    def feed(self, data: bytes):
        with self.lock:
            self.rx += data

    def handle_write(self, data: bytes):
        with self.lock:
            if self.pending is not None:
                self.interleaved.append((self.pending, data))
            text = data.decode("utf-8", "replace").strip()
            if not text:
                return
            self.commands.append(text)
            cmd, _, rest = text.partition(" --id ")
            try:
                fid = int(rest.strip())
            except ValueError:
                fid = 0
            self.pending = fid
            if self.drop_next_read:
                # The port dies on the host's next read; the command never answers.
                self.pending = None
                return
            name = cmd.split()[0] if cmd.split() else "?"
            if name == "ver":
                self.uptime_ms += 500
                body = ("uptime:   0d 00:00:%02d (%d ms)\n"
                        "version:  MetalioEinkPlus r91\n"
                        "heap:     123456 free\n") % (self.uptime_ms // 1000, self.uptime_ms)
            else:
                body = "ok: %s\n" % cmd
            frame = ("<<<BEGIN %d cmd=%s\n%s<<<END %d rc=0 ms=2 heap=1\n"
                     % (fid, name, body, fid)).encode()
            self.rx += frame
            self.ids_answered.append(fid)
            self.pending = None

    def read(self, n):
        with self.lock:
            if self.drop_next_read:
                self.drop_next_read = False
                raise OSError("ClearCommError failed (PermissionError(13))")
            if not self.booted:
                time.sleep(0.002)
                return b""
            if not self.rx:
                time.sleep(0.001)
                return b""
            out = bytes(self.rx[:n])
            del self.rx[:n]
            return out

    @property
    def in_waiting(self):
        with self.lock:
            if self.absent:
                raise OSError("ClearCommError failed (PermissionError(13))")
            return len(self.rx)


class FakeSerial:
    def __init__(self, dev: FakeDevice):
        self.dev = dev
        self.timeout = 0.05

    def read(self, n=1):
        return self.dev.read(n)

    def write(self, data):
        self.dev.handle_write(data)
        return len(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.dev.feed(b"")

    def close(self):
        self.dev.close()

    @property
    def in_waiting(self):
        return self.dev.in_waiting


def wait_for(pred, timeout=8.0, step=0.02):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return False


class ServeHarness:
    """A DeviceLink + DebugServer on an ephemeral port, torn down by the test."""

    def __init__(self, dev, idle_timeout=None, port="COM4"):
        self.dev = dev
        self.link = cpd.DeviceLink(port, 921600, idle_timeout=idle_timeout, log=lambda m: None)
        if not self.link._attach_plain():
            self.link.ser = None
        self.server = cpd.DebugServer(self.link, "127.0.0.1", 0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.link.start_monitor()
        self.thread.start()

    @property
    def addr(self):
        return ("127.0.0.1", self.server.port)

    def ask(self, cmd, timeout=5.0, **kw):
        req = {"cmd": cmd, "timeout": timeout}
        req.update(kw)
        return cpd.server_request(self.addr, req, timeout=timeout + 10)

    def stop(self):
        self.server.close()
        self.link.close("test teardown")
        self.thread.join(timeout=3)


class CpdServeTests(unittest.TestCase):
    def setUp(self):
        self.dev = FakeDevice()
        cpd._SERIAL_FACTORY = self.dev.open
        cpd._STATE_BACKUP = None
        self._state_tmp = tempfile.mkdtemp(prefix="cpdtest-")
        self._real_state = cpd.STATE_PATH
        cpd.STATE_PATH = os.path.join(self._state_tmp, "state.json")
        self._real_pick = cpd.pick_port
        cpd.pick_port = lambda explicit: explicit or "COM4"

    def tearDown(self):
        cpd._SERIAL_FACTORY = None
        cpd.STATE_PATH = self._real_state
        cpd.pick_port = self._real_pick

    # -- helpers ----------------------------------------------------------
    def run_main(self, argv, server_port):
        out, err = io.StringIO(), io.StringIO()
        argv = [a if a != "--server-port" else a for a in argv]
        if "--server-port" not in argv:
            argv = ["--server-port", str(server_port)] + list(argv)
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                rc = cpd.main(list(argv))
            except SystemExit as exc:  # argparse
                rc = int(exc.code or 0)
        return rc, out.getvalue(), err.getvalue()

    # -- 1. serve keeps ONE open across five commands ---------------------
    def test_serve_keeps_port_open_and_uptime_is_monotonic(self):
        h = ServeHarness(self.dev)
        try:
            uptimes = []
            for _ in range(5):
                reply = h.ask("ver")
                self.assertIsNotNone(reply, "no reply from the daemon")
                self.assertTrue(reply["ok"], reply)
                uptimes.append(reply["uptime_ms"])
            self.assertEqual(self.dev.opened, 1, "the daemon re-opened the port between commands")
            self.assertEqual(self.dev.closed, 0)
            self.assertEqual(uptimes, sorted(uptimes))
            self.assertEqual(len(set(uptimes)), 5)
        finally:
            h.stop()

    # -- 2. one-shot via the daemon: no reboot, no warning ----------------
    def test_one_shot_uses_the_daemon_when_it_is_running(self):
        h = ServeHarness(self.dev)
        try:
            rc, out, err = self.run_main(["ver"], h.server.port)
            self.assertEqual(rc, 0, err)
            self.assertIn("MetalioEinkPlus r91", out)
            self.assertNotIn("WARNING", err)
            self.assertEqual(self.dev.opened, 1)
            self.assertEqual(self.dev.closed, 0)
        finally:
            h.stop()

    # -- 3. fallback still works and is loud about the reboot -------------
    def test_one_shot_falls_back_with_a_warning(self):
        rc, out, err = self.run_main(["ver"], 9)  # nothing listening on :9
        self.assertEqual(rc, 0, err)
        self.assertIn("uptime:", out)
        self.assertIn("WARNING", err)
        self.assertIn("cost a reboot", err)
        self.assertEqual(self.dev.opened, 1)
        self.assertEqual(self.dev.closed, 1)

    def test_fallback_json_mode(self):
        rc, out, err = self.run_main(["ver", "--json"], 9)
        self.assertEqual(rc, 0)
        doc = json.loads(out.strip().splitlines()[-1])
        self.assertTrue(doc["ok"])
        self.assertEqual(doc["via"], "oneshot")
        self.assertTrue(doc["rebooted"])
        self.assertGreater(doc["uptime_ms"], 0)
        self.assertIn("WARNING", err)

    # -- 4. concurrent clients never interleave frames --------------------
    def test_commands_are_serialised(self):
        h = ServeHarness(self.dev)
        try:
            results = []

            def worker(n):
                for _ in range(3):
                    results.append(h.ask("ver", timeout=5.0))

            threads = [threading.Thread(target=worker, args=(i,)) for i in range(3)]
            [t.start() for t in threads]
            [t.join(10) for t in threads]
            self.assertEqual(len(results), 9)
            self.assertTrue(all(r and r["ok"] for r in results), results)
            self.assertEqual(self.dev.interleaved, [], "two frames were in flight at once")
            self.assertEqual(len(self.dev.ids_answered), 9)
            self.assertEqual(len(set(self.dev.ids_answered)), 9, "a frame id was reused")
        finally:
            h.stop()

    # -- 5. a reboot mid-command is survived, reported and retried -------
    def test_mid_command_reboot_is_survived_and_reported(self):
        h = ServeHarness(self.dev)
        try:
            self.dev.drop_next_read = True
            reply = h.ask("ver", timeout=6.0)
            self.assertIsNotNone(reply)
            self.assertTrue(reply["ok"], reply)
            self.assertTrue(reply["rebooted"])
            self.assertTrue(reply["reopened"])
            self.assertEqual(reply["attempts"], 2)
            self.assertEqual(self.dev.opened, 2, "the port was not re-opened")
            self.assertEqual(self.dev.closed, 1)
            # and the daemon is still serving the same socket afterwards
            again = h.ask("ver")
            self.assertTrue(again["ok"], again)
            self.assertEqual(self.dev.opened, 2)
        finally:
            h.stop()

    # -- 6. port loss while idle: same socket, port comes back -----------
    def test_port_loss_while_idle_is_recovered(self):
        h = ServeHarness(self.dev)
        try:
            self.assertTrue(h.ask("ver")["ok"])
            self.dev.absent = True          # the device vanished (unplug / ROM mode)
            self.assertTrue(wait_for(lambda: h.link.ser is None, timeout=6.0))
            self.dev.absent = False
            self.assertTrue(wait_for(lambda: h.link.ser is not None, timeout=12.0),
                            "the daemon did not re-open the port on its own")
            reply = h.ask("ver")
            self.assertTrue(reply["ok"], reply)
        finally:
            h.stop()

    # -- 7. idle timeout closes the port and says so ---------------------
    def test_idle_timeout_closes_the_port_and_reopens_on_demand(self):
        h = ServeHarness(self.dev, idle_timeout=0.5)
        try:
            self.assertTrue(h.ask("ver")["ok"])
            self.assertTrue(wait_for(lambda: h.dev.closed >= 1, timeout=6.0),
                            "the idle timeout never closed the port")
            self.assertIsNone(h.link.ser)
            reply = h.ask("ver")
            self.assertTrue(reply["ok"], reply)
            self.assertTrue(reply["reopened"])
            self.assertEqual(self.dev.opened, 2)
        finally:
            h.stop()

    # -- 8. shutdown stops the daemon, socket included -------------------
    def test_shutdown_stops_the_daemon(self):
        h = ServeHarness(self.dev)
        try:
            reply = h.ask("shutdown")
            self.assertTrue(reply["stopping"])
            h.thread.join(timeout=3)
            self.assertFalse(h.thread.is_alive())
            self.assertIsNone(cpd.server_request(h.addr, {"cmd": "ver"}, timeout=1.0))
        finally:
            h.stop()

    # -- 9. a dead port never produces a traceback ----------------------
    def test_unavailable_port_is_an_error_not_a_traceback(self):
        self.dev.absent = True
        self.dev.open_failures_left = 99
        rc, out, err = self.run_main(["ver"], 9)
        self.assertEqual(rc, 3)
        self.assertNotIn("Traceback", err)
        self.assertIn("cpd:", err)

    def test_server_with_absent_port_keeps_listening(self):
        self.dev.absent = True
        h = ServeHarness(self.dev)
        try:
            reply = h.ask("ver", timeout=1.0)
            self.assertFalse(reply["ok"])
            self.assertIn("error", reply)
            self.dev.absent = False
            self.assertTrue(wait_for(lambda: h.link.ser is not None, timeout=12.0))
            self.assertTrue(h.ask("ver")["ok"])
        finally:
            h.stop()

    # -- 10. raw line protocol, for bash --------------------------------
    def test_raw_line_protocol(self):
        h = ServeHarness(self.dev)
        try:
            with socket.create_connection(h.addr, timeout=3) as s:
                s.sendall(b"ver\n")
                data = b""
                while b"\n" not in data:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    data += chunk
            doc = json.loads(data.split(b"\n", 1)[0])
            self.assertTrue(doc["ok"])
            self.assertGreater(doc["uptime_ms"], 0)
        finally:
            h.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
