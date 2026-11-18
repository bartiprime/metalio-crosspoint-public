#!/usr/bin/env python3
"""Send commands to the Metalio debug shell and print what comes back. TRUSTWORTHY EDITION.

WHY THIS WAS REWRITTEN (TOOL2)
    On 4 Nov this helper returned EMPTY for `logtail` several times and hung once. Each of
    those cost a device round-trip, and one empty read nearly produced a wrong conclusion
    about the panel. The old version had three defects that made that inevitable:
      * it slept a fixed 1.6 s after sending and then read for a fixed window -- so a
        `logtail 400` whose response arrived 2 s late simply printed nothing;
      * it stopped at the end of the window whether or not the frame had finished, so a
        partial frame looked like a complete one;
      * an exception inside the read loop `break`-ed and returned whatever had been printed
        so far -- which, before the first line, is nothing at all, with exit 0.
    This version:
      * reads until the firmware's own frame terminator, `<<<END <id> rc=... ms=...`, so the
        answer is complete when it returns (a quiet period also ends the read);
      * RETRIES a command that produced zero bytes, up to RETRIES times, inside the budget;
      * enforces a hard wall-clock timeout and RETURNS PARTIAL OUTPUT rather than nothing,
        with a non-zero exit status so a caller cannot mistake partial for complete;
      * prints why the result is empty/partial on stderr, so an empty result is never silent.

BOARD SAFETY -- DO NOT CHANGE THIS ORDERING
    Asserting DTR/RTS RESETS this board, and that has already destroyed an investigation.
    dtr=False and rts=False are assigned BEFORE open(), and re-asserted right after open().
    Never reorder that.

USAGE
    python sh.py COM4 "logtail 40" 12          # one command, 12 s budget
    python sh.py COM4 "ver|panel" 15           # several, '|' separated (each gets its own frame)
    python sh.py COM4 "logtail 400"            # default budget: 12 s
    SH_SETTLE=0 python sh.py ...               # skip the 4 s post-open settle (rarely right)

EXIT STATUS
    0    output arrived and every command's frame closed
    3    no output at all (wrong port, device asleep, or the shell is not up)
    124  hard timeout / unterminated frame; whatever arrived was printed and is PARTIAL
"""

import os
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - environment guard
    print("NOPYSERIAL", file=sys.stderr)
    sys.exit(2)

SENTINEL = "<<<END "
RETRIES = 3
# A frame that has been quiet for this long is treated as finished, even without a sentinel.
# Two seconds is longer than any shell command's inter-line gap measured on this unit (the
# renderer's own `bench` line arrives ~800 ms after the body of its frame).
QUIET_S = 2.0
# Below this much remaining budget an attempt is not worth starting: it would only truncate.
MIN_ATTEMPT_S = 2.5

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
raw = sys.argv[2] if len(sys.argv) > 2 else ""
cmds = [c for c in raw.split("|") if c.strip()]
budget = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
settle = float(os.environ.get("SH_SETTLE", "4.0"))

if not cmds:
    print("usage: sh.py <port> <cmd[|cmd...]> [timeout_s]", file=sys.stderr)
    sys.exit(2)

s = serial.Serial()
s.port = port
s.baudrate = 115200
# SAFETY: before open(), per the header. These two lines must never move below s.open().
s.dtr = False
s.rts = False
s.timeout = 0.2
s.write_timeout = 2.0

got_any = False
incomplete = False
emitted = 0
t_start = time.time()


def emit(text):
    """Print a line the moment it arrives, so a timeout still leaves the evidence behind."""
    global got_any, emitted
    sys.stdout.write(text + "\n")
    sys.stdout.flush()
    got_any = True
    emitted += 1


try:
    time.sleep(0.2)
    s.open()
    # Re-assert after open: some drivers apply port defaults during open.
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    time.sleep(settle)
    try:
        s.reset_input_buffer()
    except Exception:
        pass

    t_start = time.time()  # the budget covers send+read only, not the settle

    for cmd in cmds:
        attempt = 0
        satisfied = False
        bytes_here = 0
        while attempt < RETRIES and not satisfied:
            attempt += 1
            remaining = budget - (time.time() - t_start)
            if remaining < MIN_ATTEMPT_S:
                print(f"[sh] budget exhausted before '{cmd}'", file=sys.stderr)
                incomplete = True
                break
            try:
                s.write((cmd.rstrip() + "\r\n").encode("utf-8", "replace"))
            except Exception as exc:  # port vanished mid-session
                print(f"[sh] write failed: {exc}", file=sys.stderr)
                incomplete = True
                break

            # An EMPTY attempt must not be allowed to consume the whole budget, or the retry
            # can never run: the first attempt gets at most half of what is left (floor 3 s),
            # and a later attempt gets everything that remains. Only an attempt that has
            # already received bytes is cut short, and that case never resends.
            if attempt < RETRIES:
                window = min(remaining, max(3.0, remaining * 0.5))
            else:
                window = remaining
            attempt_end = time.time() + window
            last_rx = time.time()
            saw_end = False
            while time.time() < attempt_end:
                try:
                    waiting = s.in_waiting
                except Exception as exc:
                    print(f"[sh] port read failed: {exc}", file=sys.stderr)
                    incomplete = True
                    break
                if waiting:
                    try:
                        line = s.readline().decode("utf-8", "replace").rstrip("\r\n")
                    except Exception as exc:
                        print(f"[sh] readline failed: {exc}", file=sys.stderr)
                        incomplete = True
                        break
                    if line:
                        emit(line)
                        bytes_here += len(line)
                        last_rx = time.time()
                        if line.startswith(SENTINEL):
                            saw_end = True
                            break
                else:
                    # Nothing pending: finished when the frame has gone quiet (only after at
                    # least one line, so an empty port still waits out its budget).
                    if bytes_here and (time.time() - last_rx) >= QUIET_S:
                        break
                    time.sleep(0.05)

            if saw_end:
                satisfied = True
            elif bytes_here:
                # Output arrived but the frame never closed. Do NOT resend: a resend after a
                # reboot, a drive or a half-written log doubles the damage, and the partial
                # body is usually the evidence that matters.
                incomplete = True
                break
            elif attempt < RETRIES:
                print(f"[sh] empty reply to '{cmd}' (attempt {attempt}/{RETRIES}) - resending",
                      file=sys.stderr)
                time.sleep(0.4)

        if not bytes_here:
            print(f"[sh] no output for '{cmd}' after {attempt} attempt(s)", file=sys.stderr)
finally:
    try:
        s.close()
    except Exception:
        pass

    elapsed = time.time() - t_start
    if not got_any:
        print(f"[sh] EMPTY after {elapsed:.1f}s on {port} (exit 3)", file=sys.stderr)
        sys.exit(3)
    if incomplete:
        print(f"[sh] PARTIAL after {elapsed:.1f}s on {port} ({emitted} line(s), exit 124)",
              file=sys.stderr)
        sys.exit(124)
    print(f"[sh] ok in {elapsed:.1f}s ({emitted} line(s))", file=sys.stderr)
    sys.exit(0)
