#!/usr/bin/env python3
"""cpd.py — drive the CrossPoint USB debug shell from a PC.

The console on this device is SHARED with the log stream: prompts, ESP_LOG lines
and app log lines all land in the same bytes. Every command therefore answers
inside a frame:

    <<<BEGIN <id> cmd=<name>
    ...payload...
    <<<END <id> rc=<n> ms=<n> heap=<n>

This tool sends a command with an id, reads until it sees the matching END,
prints ONLY the payload between the markers, and exits with the device's rc.
Everything outside the markers is device telemetry, not command output.

Usage
  python tools/cpd.py                            # show help
  python tools/cpd.py --port COM4 --baud 921600 ver
  python tools/cpd.py logtail 60                 # port saved in the state file, or auto-detected
  python tools/cpd.py shot /cover.bmp
  python tools/cpd.py --send-only refresh full    # fire and do not wait
  python tools/cpd.py --raw logtail 5             # dump every byte received
  python tools/cpd.py --timeout 20 refresh full
  python tools/cpd.py --json ver                  # one JSON object on stdout, for scripts

  python tools/cpd.py serve                       # hold the port open for a whole session
  python tools/cpd.py serve --idle-timeout 60     # ... and let go after 60 min of silence
  python tools/cpd.py serve --stop                # stop the daemon (the board resets once)

WHY `serve` EXISTS
  Closing this port deasserts DTR/RTS, and that is a chip reset on this board
  (reset_reason=11, ESP_RST_USB). A one-shot invocation therefore reboots the
  device on every command: ~7 s of boot, and RAM state, the UI page and any
  reading session are gone. `serve` opens COM4 once, keeps it open, and answers
  commands over 127.0.0.1 TCP, so the device is never reset between commands.
  The one-shot form still works: it first tries a running daemon and only falls
  back to open/close when none answers — and then it says, in one line, that the
  command cost a reboot, so the cost is never silent.

Port discovery: --port, else $CPD_PORT, else the last port that worked (saved
in ~/.cpd_state.json), else the only USB-Serial/JTAG port on the machine.

Serial support: pyserial (the `serial` module). Without it, only --raw works
(it opens the port through the OS and dumps bytes), and the tool says so
instead of pretending to parse.

Exit codes: the device rc for a framed reply (0 ok, negative = errno-style),
2 for a local usage error, 3 for no reply before the timeout, 4 for no pyserial.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import socket
import sys
import threading
import time

STATE_PATH = os.path.join(os.path.expanduser("~"), ".cpd_state.json")
DEFAULT_BAUD = 921600
DEFAULT_TIMEOUT = 10.0
BEGIN_RE = re.compile(rb"<<<BEGIN\s+(\d+)\s+cmd=(\S+)")
END_RE = re.compile(rb"<<<END\s+(\d+)\s+rc=(-?\d+)")
UPTIME_RE = re.compile(r"\((\d+)\s*ms\)")

# --- the resident daemon -----------------------------------------------------
# One fixed default so `cpd.py ver` finds the daemon without being told; --listen
# (server) and --server-port (client) override it, and $CPD_SERVER_PORT does too.
DEFAULT_SERVER_HOST = "127.0.0.1"
DEFAULT_SERVER_PORT = 45123
# '[CP] loop tick' is printed from loop() after setup() started the REPL, so it is
# the honest "the application is up" marker after a reset (see _open_ready).
BOOT_MARKER = b"loop tick #"
DEFAULT_IDLE_MINUTES = 30.0
MAX_REQUEST_BYTES = 64 * 1024


def _serial_error_types() -> tuple:
    """The exceptions a dead USB-Serial/JTAG port raises, without requiring pyserial."""
    try:
        import serial  # type: ignore

        return (serial.SerialException, OSError)
    except Exception:  # noqa: BLE001
        return (OSError,)


SERIAL_ERRORS = _serial_error_types()

# Injection seam for the offline tests: (port, baud, timeout, write_timeout) -> serial-like.
_SERIAL_FACTORY = None


def open_serial(port: str, baud: int, timeout: float = 0.05, write_timeout: float = 2):
    if _SERIAL_FACTORY is not None:
        return _SERIAL_FACTORY(port, baud, timeout, write_timeout)
    import serial  # type: ignore

    return serial.Serial(port, baud, timeout=timeout, write_timeout=write_timeout)


def load_state() -> dict:
    try:
        with open(STATE_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(state: dict) -> None:
    try:
        with open(STATE_PATH, "w", encoding="utf-8") as f:
            json.dump(state, f)
    except Exception:
        pass


def candidate_ports() -> list[str]:
    """USB-Serial/JTAG is VID:PID 303A:1001 on this board (design doc §1.2)."""
    matches = []
    try:
        from serial.tools import list_ports  # type: ignore

        for p in list_ports.comports():
            if (p.vid, p.pid) == (0x303A, 0x1001):
                matches.append(p.device)
        if matches:
            return matches
        return [p.device for p in list_ports.comports() if p.device]
    except Exception:
        pass
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.usbmodem*"):
        matches.extend(sorted(glob.glob(pattern)))
    return matches


def pick_port(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    env = os.environ.get("CPD_PORT")
    if env:
        return env
    saved = load_state().get("port")
    if saved:
        return saved
    found = candidate_ports()
    if len(found) == 1:
        return found[0]
    return None


def have_pyserial() -> bool:
    try:
        import serial  # noqa: F401

        return True
    except Exception:
        return False


def raw_mode(port: str, baud: int, timeout: float, command: str, capture: str | None) -> int:
    """No pyserial: open the port through the OS and dump bytes for `timeout`."""
    print("cpd: pyserial is not installed — --raw mode: writing the command and dumping bytes.",
          file=sys.stderr)
    print("cpd: install it with 'pip install pyserial' to get framed replies.", file=sys.stderr)
    payload = (command + "\n").encode()
    try:
        if os.name == "posix":
            import termios
            import tty

            fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
            tty.setraw(fd)
            os.write(fd, payload)
            deadline = time.time() + timeout
            chunks = []
            while time.time() < deadline:
                data = os.read(fd, 4096)
                if data:
                    chunks.append(data)
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
            os.close(fd)
            blob = b"".join(chunks)
        else:
            # Windows needs the \\.\ prefix for COM10 and above; harmless below it.
            win_port = port if port.startswith("\\\\.\\") else "\\\\.\\" + port
            fd = os.open(win_port, os.O_RDWR)
            os.write(fd, payload)
            deadline = time.time() + timeout
            chunks = []
            while time.time() < deadline:
                try:
                    data = os.read(fd, 4096)
                except OSError:
                    break
                if data:
                    chunks.append(data)
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
            os.close(fd)
            blob = b"".join(chunks)
    except Exception as exc:  # noqa: BLE001
        print(f"cpd: raw open of {port} failed: {exc}", file=sys.stderr)
        return 3
    if capture:
        with open(capture, "wb") as f:
            f.write(blob)
        print(f"\ncpd: raw capture saved to {capture}", file=sys.stderr)
    return 0


class DeviceNoAnswer(Exception):
    """A device-side failure. Reported as one sentence; never as a traceback."""


def _reset_input(ser) -> None:
    """Drop everything already in the pipe: stale replies AND log spam.

    Called before a command is written and again once a frame has been matched,
    so one invocation can never hand its leftovers to the next one.
    """
    try:
        ser.reset_input_buffer()
    except Exception:  # noqa: BLE001 - a port that will not flush is a dead port
        pass


def _read_frame(ser, frame_id: int, timeout: float, raw: bool, capture: bytearray) -> tuple[bytes, int]:
    """Read until `<<<END <frame_id> rc=n` and return (payload, rc).

    Desync-proof by construction:
      * bytes arriving BEFORE `<<<BEGIN <our id>` are DISCARDED, never parsed;
      * the END we accept must carry OUR id, so a late reply to an earlier
        command (which is what made `ver` print the previous `spine` body) is
        ignored instead of answered;
      * the input buffer is cleared again after the match, so the next run starts
        clean even if more bytes (a second reply, log spam) were still queued.

    Raises DeviceNoAnswer on timeout; SerialException/OSError propagate to the
    caller, which closes and retries once.
    """
    begin_re = re.compile(rb"<<<BEGIN\s+%d\b[^\n]*\n" % frame_id)
    end_re = re.compile(rb"<<<END\s+%d\s+rc=(-?\d+)" % frame_id)
    started = False
    buf = bytearray()
    start = time.time()
    while time.time() - start < timeout:
        # Scan what is already buffered FIRST, then read: a BEGIN and its END can
        # arrive in the same read (a fast command), and re-reading before
        # rescanning is how a reply gets timed out with the answer in hand.
        if started:
            m = end_re.search(buf)
            if m is not None:
                body = bytes(buf[:m.start()])
                rc = int(m.group(1))
                _reset_input(ser)  # nothing after our END may be read as the next reply
                return body, rc
        elif buf:
            m = begin_re.search(buf)
            if m is not None:
                del buf[:m.end()]
                started = True
                continue  # rescan the same buffer for the END
            elif len(buf) > 256:
                # Keep a small tail only: a BEGIN split across two reads must survive.
                del buf[:-256]
        try:
            chunk = ser.read(4096)
        except Exception as exc:  # noqa: BLE001 - ClearCommError etc. surface here
            raise serial_exception(exc) from None
        if not chunk:
            continue
        capture += chunk
        if raw:
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
        buf += chunk
    if not started:
        raise DeviceNoAnswer(f"no '<<<BEGIN {frame_id}' within {timeout:.1f}s")
    raise DeviceNoAnswer(f"the command started but no '<<<END {frame_id}' arrived within {timeout:.1f}s")


def serial_exception(exc: Exception) -> Exception:
    try:
        import serial  # type: ignore

        if isinstance(exc, (serial.SerialException, OSError)):
            return exc
    except Exception:  # noqa: BLE001
        pass
    return DeviceNoAnswer(str(exc))


def _open_ready(port: str, baud: int, limit: float = 15.0):
    """Open the port and wait until the APPLICATION is up; None on timeout.

    Retry path only. A close of this port reboots the board (the USB-Serial/JTAG
    peripheral resets the chip on the host's DTR/RTS line drop, which the firmware
    reports as ESP_RST_USB), so a plain reopen lands in the bootloader's silence and
    the command would be written into nothing. '[CP] loop tick' is printed from
    loop(), i.e. after setup() has started the shell REPL, so it is the honest
    readiness signal.

    NOTE: never set dtr/rts to False on this port. Measured on the device: asserting
    them (pyserial's default) is harmless, but DEASSERTING them resets the chip
    immediately (boot banner within 60 ms, port drops at ~1.7 s). The reset at close
    comes from the driver dropping the lines, not from anything this tool can hold
    on to.
    """
    deadline = time.time() + limit
    while time.time() < deadline:
        try:
            ser = open_serial(port, baud)
        except SERIAL_ERRORS:
            time.sleep(0.2)
            continue
        try:
            start = time.time()
            while time.time() - start < limit:
                chunk = ser.read(4096)
                if chunk and BOOT_MARKER in chunk:
                    _reset_input(ser)
                    return ser
        except SERIAL_ERRORS:
            pass
        try:
            ser.close()
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.2)
    return None


def _exchange(port: str, baud: int, line: bytes, frame_id: int, timeout: float, raw: bool,
              capture: bytearray, send_only: bool) -> tuple[bytes | None, int]:
    """One command, with the port opened, closed and (on a dropped port) retried ONCE.

    A dropped USB-Serial/JTAG port raises SerialException ("ClearCommError failed ...
    The device does not recognize the command") and looks exactly like a device
    reset. The retry therefore does not just reopen: it waits for the board to finish
    booting before resending (_open_ready), and the firmware keeps the session across
    the ESP_RST_USB reset this tool causes.
    """
    last: Exception | None = None
    timeout_seen = False
    for attempt in (1, 2):
        ser = None
        try:
            if attempt == 1:
                ser = open_serial(port, baud)
                # Kill anything already queued BEFORE the write (a late reply to an
                # older command, or boot log spam).
                _reset_input(ser)
            else:
                ser = _open_ready(port, baud)
                if ser is None:
                    raise DeviceNoAnswer(f"{port} never came back after the reset")
            ser.write(line)
            ser.flush()
            if send_only:
                return None, 0
            return _read_frame(ser, frame_id, timeout, raw, capture)
        except DeviceNoAnswer as exc:
            # A timeout can also mean the command line was mangled in the pipe by a
            # leftover prompt; one clean retry is cheap, more than one is not.
            if attempt == 1 and not timeout_seen:
                timeout_seen = True
                print(f"cpd: {exc}; waiting for {port} to reboot and retrying once",
                      file=sys.stderr)
                last = exc
            else:
                raise
        except SERIAL_ERRORS as exc:  # noqa: BLE001
            last = exc
            if attempt == 1:
                print(f"cpd: {port}: {exc} — waiting for the board to reboot and retrying once",
                      file=sys.stderr)
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:  # noqa: BLE001
                    pass
    raise DeviceNoAnswer(f"{port} failed twice (last: {last})")


def _hints(capture: bytes, frame_id: int, command: str) -> None:
    """Human diagnosis for 'no answer'. Never a traceback, never a guess that hides data."""
    if not capture:
        print("cpd: nothing at all arrived. Is the shell switched on? Debug page -> 'USB debug shell' "
              "(persisted key usbDebugShell); it also starts OFF after a cold boot, a manual sleep, "
              "a power-off or USB drive mode.", file=sys.stderr)
        return
    if b"<<<BEGIN" in capture:
        print("cpd: a BEGIN arrived but no END for this id — the command started and did not finish "
              "(main loop busy, or the device reset mid-command). Try a longer --timeout.", file=sys.stderr)
    else:
        print("cpd: bytes arrived but none of them was this command's frame — the port is carrying "
              "logs only. Is the shell still on?", file=sys.stderr)


def _uptime_ms(payload: str) -> int | None:
    """Last '(NNN ms)' in a `ver` payload, else None. Used for the serve proof."""
    found = UPTIME_RE.findall(payload or "")
    return int(found[-1]) if found else None


# ---------------------------------------------------------------------------
# Resident mode: one port, held open, commands over 127.0.0.1 TCP
# ---------------------------------------------------------------------------

class DeviceLink:
    """The single owner of the serial port in serve mode.

    Contract:
      * one command in flight ever — `self.lock` covers write+read, so two clients
        can never interleave frames;
      * the port is opened ONCE and not closed between commands (that close is
        what reboots the board);
      * DTR/RTS are never touched: pyserial asserts them on open, and deasserting
        them resets the chip (measured, USB_DEBUG_CHANNEL §9.3);
      * if the device resets anyway (panic, watchdog, a manual power cycle), the
        port loss is caught, the port re-opened and the boot marker awaited, and
        the caller is told (`rebooted: true`) instead of the server dying;
      * an idle timeout may close the port on purpose; the next command re-opens
        it, and the caller is told that the re-open costs a reset.
    """

    def __init__(self, port: str, baud: int, idle_timeout: float | None = None, log=print):
        self.port = port
        self.baud = baud
        self.idle_timeout = idle_timeout  # seconds, or None
        self.ser = None
        self.lock = threading.Lock()
        self.last_used = time.time()
        self.idle_closed = False
        self.stop = threading.Event()
        self.served = 0
        self.reboots = 0
        self.opens = 0
        self.last_error: str | None = None
        self._ever_open = False
        self._frame_seq = int(time.time() * 1000) % 100000
        self._next_retry = 0.0
        self._thread: threading.Thread | None = None
        self._log = log

    # -- logging ---------------------------------------------------------
    def log(self, msg: str) -> None:
        try:
            self._log(f"cpd: serve: {msg}")
        except Exception:  # noqa: BLE001
            pass

    # -- port handling (call with self.lock held) ------------------------
    def _drop(self) -> None:
        ser, self.ser = self.ser, None
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass

    def _attach_plain(self) -> bool:
        try:
            ser = open_serial(self.port, self.baud)
        except SERIAL_ERRORS as exc:
            self.last_error = f"{self.port}: {exc}"
            return False
        _reset_input(ser)
        self.ser = ser
        self.opens += 1
        self._ever_open = True
        self.idle_closed = False
        self.last_error = None
        return True

    def _attach_ready(self, limit: float = 15.0) -> bool:
        ser = _open_ready(self.port, self.baud, limit=limit)
        if ser is None:
            self.last_error = f"{self.port} did not come back within {limit:.0f}s"
            return False
        self.ser = ser
        self.opens += 1
        self._ever_open = True
        self.idle_closed = False
        self.last_error = None
        return True

    # -- the command path ------------------------------------------------
    def run(self, cmd: str, timeout: float = DEFAULT_TIMEOUT, send_only: bool = False) -> dict:
        result = {
            "ok": False, "rc": -1, "payload": "", "ms": 0,
            "rebooted": False, "reopened": False, "attempts": 0,
            "via": "serve", "port": self.port, "error": None, "uptime_ms": None,
        }
        started = time.time()
        with self.lock:
            self.served += 1
            self.last_used = time.time()
            try:
                if self.ser is None:
                    why = "idle re-open" if self.idle_closed else "port re-open"
                    self.log(f"{why}: opening {self.port}; closing a port resets the board once")
                    ok = self._attach_plain() if not self._ever_open else self._attach_ready()
                    if not ok:
                        result["error"] = self.last_error or f"{self.port} is not available"
                        return result
                    result["reopened"] = True

                ser = self.ser
                if ser is None:  # only reachable if a re-open raced the monitor
                    result["error"] = self.last_error or f"{self.port} is not available"
                    return result

                frame_id = (self._frame_seq + 1) % 100000
                self._frame_seq = frame_id
                line = f"{cmd} --id {frame_id}"
                capture = bytearray()
                timeout_seen = False
                for attempt in (1, 2):
                    result["attempts"] = attempt
                    ser = self.ser  # a retry may follow a re-open, so re-read it
                    if ser is None:
                        result["error"] = self.last_error or f"{self.port} is not available"
                        return result
                    try:
                        # Never inherit a previous command's leftovers, and never a
                        # backlog of log spam either.
                        _reset_input(ser)
                        ser.write((line + "\r\n").encode())
                        ser.flush()
                        if send_only:
                            result.update(ok=True, rc=0)
                            return result
                        body, rc = _read_frame(ser, frame_id, timeout, False, capture)
                        text = body.decode("utf-8", "replace")
                        result.update(ok=True, rc=rc, payload=text, uptime_ms=_uptime_ms(text))
                        return result
                    except DeviceNoAnswer as exc:
                        # Retry ONCE on the same open port: re-opening would cost a
                        # reboot, which is the whole thing serve mode exists to avoid.
                        if attempt == 1 and not timeout_seen:
                            timeout_seen = True
                            self.log(f"{exc}; retrying once on the open port")
                            continue
                        result["error"] = str(exc)
                        return result
                    except SERIAL_ERRORS as exc:  # noqa: BLE001
                        self.last_error = str(exc)
                        self.reboots += 1
                        result["rebooted"] = True
                        self._drop()
                        if attempt == 2:
                            result["error"] = f"{self.port} failed twice (last: {exc})"
                            return result
                        self.log(f"port lost mid-command ({exc}); waiting for {self.port} "
                                 f"and the '[CP] loop tick' boot marker")
                        if not self._attach_ready():
                            result["error"] = f"{self.port} did not return after the reset"
                            return result
                        result["reopened"] = True
                result["error"] = "unreachable"
                return result
            finally:
                result["ms"] = int((time.time() - started) * 1000)

    # -- health + idle watchdog -----------------------------------------
    def start_monitor(self) -> None:
        self._thread = threading.Thread(target=self._monitor, name="cpd-monitor", daemon=True)
        self._thread.start()

    def _monitor(self) -> None:
        while not self.stop.wait(1.0):
            try:
                with self.lock:
                    now = time.time()
                    if (self.ser is not None and self.idle_timeout
                            and now - self.last_used >= self.idle_timeout):
                        self.log(f"idle timeout ({self.idle_timeout / 60:.1f} min with no "
                                 f"command): closing {self.port}; the board will reset once")
                        self._drop()
                        self.idle_closed = True
                        continue
                    if self.ser is None:
                        if self.idle_closed or now < self._next_retry:
                            continue
                        ok = self._attach_plain() if not self._ever_open else self._attach_ready()
                        if ok:
                            self.log(f"re-opened {self.port} and waited for the boot marker")
                        else:
                            self._next_retry = now + 5.0
                            self.log(f"{self.last_error}; retrying in 5 s")
                        continue
                    try:
                        _ = self.ser.in_waiting
                    except SERIAL_ERRORS as exc:  # noqa: BLE001
                        self.log(f"port dropped while idle ({exc}); waiting for it to return")
                        self._drop()
                        self.reboots += 1
                        self._next_retry = 0.0
            except Exception as exc:  # noqa: BLE001 - the watchdog must never die
                self.log(f"monitor: {type(exc).__name__}: {exc}")

    def close(self, why: str) -> None:
        self.stop.set()
        with self.lock:
            if self.ser is not None:
                self.log(f"{why}: closing {self.port} — the board will reset once (DTR/RTS deassert)")
            self._drop()

    def status(self) -> dict:
        return {
            "port": self.port, "open": self.ser is not None, "opens": self.opens,
            "reboots": self.reboots, "served": self.served,
            "idle_seconds": round(time.time() - self.last_used, 1),
            "idle_timeout": self.idle_timeout, "idle_closed": self.idle_closed,
            "last_error": self.last_error, "pid": os.getpid(),
        }


class DebugServer:
    """Single-threaded TCP front end: one JSON request per connection, one reply.

    The socket outlives the serial port — a device reset or an idle close never
    closes it, so a client never has to re-discover the daemon.
    """

    def __init__(self, link: DeviceLink, host: str = DEFAULT_SERVER_HOST, port: int = DEFAULT_SERVER_PORT):
        self.link = link
        self.stop = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(8)
        self.host, self.port = self.sock.getsockname()[:2]

    def serve_forever(self) -> None:
        self.sock.settimeout(0.5)
        while not self.stop.is_set():
            try:
                conn, _addr = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._client, args=(conn,), name="cpd-client", daemon=True).start()

    def _client(self, conn) -> None:
        try:
            conn.settimeout(5.0)
            buf = bytearray()
            while b"\n" not in buf and len(buf) < MAX_REQUEST_BYTES:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
            line = bytes(buf).split(b"\n", 1)[0].decode("utf-8", "replace").strip()
            if not line:
                return
            try:
                req = json.loads(line) if line.startswith("{") else {"cmd": line}
                if not isinstance(req, dict):
                    raise ValueError("request must be a JSON object")
            except Exception:  # noqa: BLE001
                self._send(conn, {"ok": False, "rc": -1,
                                  "error": "bad request: expected a JSON object or a raw command line"})
                return
            cmd = str(req.get("cmd") or "").strip()
            if not cmd:
                self._send(conn, {"ok": False, "rc": -1, "error": "empty command"})
                return
            if cmd in ("shutdown", "stop", "quit") and not req.get("as_command"):
                self._send(conn, {"ok": True, "rc": 0, "stopping": True, "via": "serve",
                                  "note": "closing the serial port — the board will reset once"})
                self.stop.set()
                self.link.stop.set()
                return
            timeout = float(req.get("timeout") or DEFAULT_TIMEOUT)
            reply = self.link.run(cmd, timeout=timeout, send_only=bool(req.get("send_only")))
            reply["served"] = self.link.served
            self._send(conn, reply)
        except Exception as exc:  # noqa: BLE001 - never a traceback, never a dropped client
            try:
                self._send(conn, {"ok": False, "rc": -1, "error": f"{type(exc).__name__}: {exc}"})
            except Exception:  # noqa: BLE001
                pass
        finally:
            try:
                conn.close()
            except Exception:  # noqa: BLE001
                pass

    @staticmethod
    def _send(conn, obj: dict) -> None:
        conn.sendall((json.dumps(obj) + "\n").encode("utf-8"))

    def close(self) -> None:
        self.stop.set()
        try:
            self.sock.close()
        except Exception:  # noqa: BLE001
            pass


def parse_listen(value: str | None) -> tuple[str, int]:
    if not value:
        return DEFAULT_SERVER_HOST, int(os.environ.get("CPD_SERVER_PORT", DEFAULT_SERVER_PORT))
    value = value.strip()
    if ":" in value:
        host, _, port = value.rpartition(":")
        return (host or DEFAULT_SERVER_HOST), int(port)
    return DEFAULT_SERVER_HOST, int(value)


def server_request(addr: tuple[str, int], request: dict, timeout: float = 5.0) -> dict | None:
    """One request to a running daemon; None means 'nothing answered there'."""
    try:
        with socket.create_connection(addr, timeout=1.5) as s:
            s.settimeout(timeout)
            s.sendall((json.dumps(request) + "\n").encode("utf-8"))
            buf = bytearray()
            while b"\n" not in buf:
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
                if len(buf) > MAX_REQUEST_BYTES * 64:
                    break
    except OSError:
        return None
    if not buf:
        return None
    try:
        return json.loads(bytes(buf).split(b"\n", 1)[0].decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001
        return None


def serve_main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="cpd.py serve",
        description="Hold the debug port open for a whole session and answer commands over 127.0.0.1 TCP.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "The port is opened ONCE. Closing it deasserts DTR/RTS, which resets the chip\n"
            "(ESP_RST_USB) — that is what this mode exists to avoid. One-shot commands\n"
            "(python tools/cpd.py ver) find the daemon automatically.\n\n"
            "  python tools/cpd.py serve --port COM4 --idle-timeout 60\n"
            "  python tools/cpd.py serve --stop\n"
            "  printf 'ver\\n' | nc 127.0.0.1 45123       # raw line protocol also works\n"
        ),
    )
    ap.add_argument("--port", help="serial port (default: --port/$CPD_PORT/saved/last)")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"baud (default {DEFAULT_BAUD})")
    ap.add_argument("--listen", default=None,
                    help=f"PORT or HOST:PORT to listen on (default {DEFAULT_SERVER_HOST}:{DEFAULT_SERVER_PORT})")
    ap.add_argument("--idle-timeout", type=float, default=DEFAULT_IDLE_MINUTES, metavar="MINUTES",
                    help="close the serial port after this many minutes with no command "
                         f"(default {DEFAULT_IDLE_MINUTES:g}; 0 = never). Closing it resets the board once.")
    ap.add_argument("--stop", action="store_true", help="stop a running daemon (no serial port is opened)")
    args = ap.parse_args(argv)

    try:
        host, listen_port = parse_listen(args.listen)
    except ValueError:
        print(f"cpd: serve: --listen wants PORT or HOST:PORT, got {args.listen!r}", file=sys.stderr)
        return 2

    if args.stop:
        reply = server_request((host, listen_port), {"cmd": "shutdown"})
        if reply is None:
            print(f"cpd: serve: nothing is listening on {host}:{listen_port} — no daemon to stop.",
                  file=sys.stderr)
            return 1
        print(f"cpd: serve: stopped the daemon on {host}:{listen_port}. It closed the serial port, "
              f"so the board reset once (DTR/RTS deasserted).", file=sys.stderr)
        return 0

    port = pick_port(args.port)
    if port is None:
        found = candidate_ports()
        print("cpd: serve: no port. Pass --port COM<n>, or set $CPD_PORT. Seen: "
              + (", ".join(found) or "none"), file=sys.stderr)
        return 2
    if not have_pyserial():
        print("cpd: serve: pyserial is required ('pip install pyserial').", file=sys.stderr)
        return 4

    idle = args.idle_timeout * 60.0 if args.idle_timeout and args.idle_timeout > 0 else None
    link = DeviceLink(port, args.baud, idle_timeout=idle,
                      log=lambda m: print(m, file=sys.stderr, flush=True))

    with link.lock:
        opened = link._attach_plain()
    if opened:
        link.log(f"holding {port} at {args.baud}; DTR/RTS left ASSERTED (deasserting them resets the chip)")
    else:
        link.log(f"{port} is not open yet ({link.last_error}); the daemon is listening and will "
                 f"re-open it as soon as it appears")

    try:
        server = DebugServer(link, host, listen_port)
    except OSError as exc:
        print(f"cpd: serve: cannot listen on {host}:{listen_port}: {exc}", file=sys.stderr)
        link.close("startup failed")
        return 3

    save_state({"port": port, "baud": args.baud})
    link.log(f"listening on {server.host}:{server.port} — one command at a time, one open port. "
             f"Ctrl-C or 'cpd.py serve --stop' to stop.")
    if idle:
        link.log(f"idle timeout: {args.idle_timeout:g} min. Reaching it CLOSES the port, which resets "
                 f"the board once; the next command re-opens it.")

    link.start_monitor()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        link.log("interrupted")
    finally:
        server.close()
        link.close("stopping")
        link.log("stopped")
    return 0


# ---------------------------------------------------------------------------
# One-shot mode (behaviour unchanged; it only prefers a running daemon)
# ---------------------------------------------------------------------------

def _fallback_warning(addr: tuple[str, int]) -> str:
    return (f"cpd: WARNING: no serve daemon on {addr[0]}:{addr[1]} — using the one-shot path: "
            f"closing the port deasserts DTR/RTS and RESETS the board (ESP_RST_USB), so this "
            f"command cost a reboot. Run 'python tools/cpd.py serve' to keep the device alive.")


def main(argv: list[str]) -> int:
    if argv and argv[0] == "serve":
        return serve_main(argv[1:])

    ap = argparse.ArgumentParser(
        prog="cpd.py",
        description="Send one command to the CrossPoint USB debug shell and print only its framed payload.",
        epilog=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("command", nargs="+", help="command and arguments, e.g. 'logtail 60'")
    ap.add_argument("--port", help="serial port (e.g. COM4, /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"baud (default {DEFAULT_BAUD})")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                    help=f"hard timeout in seconds for the whole exchange (default {DEFAULT_TIMEOUT})")
    ap.add_argument("--id", type=int, default=None, help="frame id to send (default: derived from the clock)")
    ap.add_argument("--send-only", action="store_true", help="write the command and exit; do not wait for a reply")
    ap.add_argument("--raw", action="store_true",
                    help="dump every byte received (log spam included) and save it with --capture")
    ap.add_argument("--capture", help="save the full raw capture to this file (also on timeout)")
    ap.add_argument("--json", action="store_true", help="print one JSON object instead of the payload")
    ap.add_argument("--no-server", action="store_true",
                    help="ignore any running daemon and take the one-shot path (costs a reboot)")
    ap.add_argument("--server-port", type=int,
                    default=int(os.environ.get("CPD_SERVER_PORT", DEFAULT_SERVER_PORT)),
                    help=f"daemon port (default {DEFAULT_SERVER_PORT})")
    args = ap.parse_args(argv)

    command = " ".join(args.command)
    if not command.strip():
        ap.print_help()
        return 2

    addr = (DEFAULT_SERVER_HOST, args.server_port)

    # Prefer the resident daemon: it holds the port open, so nothing reboots.
    if not args.no_server:
        reply = server_request(addr, {"cmd": command, "timeout": args.timeout,
                                      "send_only": args.send_only}, timeout=args.timeout + 20.0)
        if reply is not None:
            if args.send_only and reply.get("ok"):
                print(f"cpd: sent '{command}' via the daemon on {addr[0]}:{addr[1]}", file=sys.stderr)
                return 0
            if args.json:
                print(json.dumps(reply, ensure_ascii=False))
            elif reply.get("ok"):
                payload = reply.get("payload") or ""
                sys.stdout.write(payload if payload.endswith("\n") or not payload else payload + "\n")
                sys.stdout.flush()
            if not reply.get("ok"):
                print(f"cpd: serve: {reply.get('error') or 'no answer'}", file=sys.stderr)
            if reply.get("rebooted"):
                print("cpd: note: the device reset mid-command; the port was re-opened, the boot "
                      "marker awaited and the command retried once (rebooted=true).", file=sys.stderr)
            elif reply.get("reopened"):
                print("cpd: note: the daemon had to re-open the port (an idle timeout or a port "
                      "loss); that re-open resets the board once.", file=sys.stderr)
            return int(reply.get("rc") if reply.get("ok") else 3)

        print(_fallback_warning(addr), file=sys.stderr)

    port = pick_port(args.port)
    if port is None:
        found = candidate_ports()
        print("cpd: no port. Pass --port COM<n>, or set $CPD_PORT. Seen: " + (", ".join(found) or "none"),
              file=sys.stderr)
        return 2

    if not have_pyserial():
        return raw_mode(port, args.baud, args.timeout,
                        command + (f" --id {args.id}" if args.id is not None else ""), args.capture)

    frame_id = args.id if args.id is not None else int(time.time() * 1000) % 100000
    line = f"{command} --id {frame_id}"
    capture = bytearray()
    try:
        body, rc = _exchange(port, args.baud, (line + "\r\n").encode(), frame_id, args.timeout, args.raw,
                             capture, args.send_only)
    except DeviceNoAnswer as exc:
        if args.capture:
            with open(args.capture, "wb") as f:
                f.write(capture)
        print(f"cpd: device did not answer — {exc}", file=sys.stderr)
        print(f"cpd: received {len(capture)} bytes; "
              + (f"raw capture saved to {args.capture}" if args.capture else "re-run with --capture F.bin to keep it"),
              file=sys.stderr)
        _hints(bytes(capture), frame_id, command)
        if args.json:
            print(json.dumps({"ok": False, "rc": 3, "error": str(exc), "via": "oneshot",
                              "rebooted": True, "payload": ""}))
        return 3
    except KeyboardInterrupt:
        print("cpd: interrupted", file=sys.stderr)
        return 130
    except Exception as exc:  # noqa: BLE001 - a device problem is never a traceback
        if args.capture:
            with open(args.capture, "wb") as f:
                f.write(capture)
        print(f"cpd: {port}: {type(exc).__name__}: {exc}", file=sys.stderr)
        _hints(bytes(capture), frame_id, command)
        if args.json:
            print(json.dumps({"ok": False, "rc": 3, "error": f"{type(exc).__name__}: {exc}",
                              "via": "oneshot", "rebooted": True, "payload": ""}))
        return 3

    save_state({"port": port, "baud": args.baud})
    if args.send_only:
        print(f"cpd: sent '{line}' to {port} (not waiting for a reply)", file=sys.stderr)
        return 0
    if args.capture:
        with open(args.capture, "wb") as f:
            f.write(capture)
    payload = (body or b"").decode("utf-8", "replace")
    if args.json:
        print(json.dumps({"ok": True, "rc": rc, "payload": payload, "via": "oneshot",
                          "rebooted": True, "port": port, "uptime_ms": _uptime_ms(payload)},
                         ensure_ascii=False))
        return rc if rc is not None else 0
    if not args.raw:
        # The device's payload may contain log lines that landed inside the
        # frame; they are marked so a human can tell them apart, but they are
        # printed because suppressing them would hide real diagnostics.
        sys.stdout.write(payload if payload.endswith("\n") or not payload else payload + "\n")
        sys.stdout.flush()
    return rc if rc is not None else 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
