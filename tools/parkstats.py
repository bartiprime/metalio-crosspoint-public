#!/usr/bin/env python3
"""parkstats — offline analyser for the standby journal on this fork.

Input: a pasted or copied log that contains the lines CrossPoint writes to
`/.crosspoint/sleep.txt` on this board (plus whatever else was on the console
around them). Nothing is filtered silently: every line that looks like one of
ours and does not parse is listed at the end, with its line number and why.

    python tools/parkstats.py sleep.txt
    python tools/parkstats.py - < pasted.txt          # stdin
    python tools/parkstats.py --selftest              # embedded r101.18 excerpt

What it prints:

  PARKS    one row per `wakerec park` -> `wakerec resume` pair: when it parked,
           how long, how many slices, the coulomb-counter delta and the mA that
           follows from it, which trims were live, and whether the P0.5 CAM_SCR
           line appeared and whether it carried `[runtime override]` or
           `[build default]`. That suffix is the ONLY proof that a RAM-only arm
           was still live when the park ran -- sleep-23.txt (r101.20) is a park
           credited to an arm that had already been wiped by a reset.

  SUMMARY  mA per configuration arm (pa, sd, cam), weighted the same way
           BACKLOG.md row PWR3 weights it: total mAh / total hours, never a mean
           of per-park mA (the counter has a 1 mAh LSB, so a short park's own
           mA is noise).

  BOOTS    the `reset=...` decoder, using the enum in esp_system.h:25-40 --
           correct values, not the legend at src/main.cpp:2951-2952, which is
           shifted and reads a brownout (9) as a deep-sleep wake.

Field formats are the ones the firmware actually emits (src/main.cpp:1514,
:1562, :1705-1770, :1901-1920, :3636-3650):

  reset=%d(%s) cause=%d aborted=%d abortCause=%d abortPin=%d touch=%d armedLevel=%d armErr=%d uph=%lu
  t=<ms> standby enter timeout=... touch=... light=... reader=...
  t=<ms> standby park: P0.5 CAM_SCR rail cut (P1.4 still up) [vendor order][ [build default]|[ [runtime override]]
  t=<ms> standby park: P1.4 BT/PA rail cut (P0.5 untouched) [build default]
  t=<ms> wakerec park rc=0x10 <mAh> mAh trim_pa=<0|1> trim_sd=<0|1> slices=<n> up=<ms> ms
  t=<ms> wakerec resume rc=0x10 <mAh> mAh d=<+-n> mAh over <ms> ms (<x.xx> mA) trim_pa=.. trim_sd=.. slices_at_park=..
  t=<ms> standby resume: SD remount ok in <ms> ms after <n> slices (settle budget <ms> ms)

Truncated lines, CRLF, a missing `t=`, and a log that starts mid-park are all
expected: a pair whose half is missing is reported, not dropped.
"""

import re
import sys

# esp_reset_reason_t, components/esp_system/include/esp_system.h:25-40.
RESET_REASONS = {
    0: "UNKNOWN",
    1: "POWERON",
    2: "EXT",
    3: "SW",            # esp_restart() -- also the aborted-sleep path
    4: "PANIC",
    5: "INT_WDT",
    6: "TASK_WDT",
    7: "WDT",
    8: "DEEPSLEEP",
    9: "BROWNOUT",
    10: "SDIO",
    11: "USB",
    12: "JTAG",
    13: "EFUSE",
    14: "PWR_GLITCH",
    15: "CPU_LOCKUP",
}

# esp_sleep_wakeup_cause_t on this target, per
# docs/engineering/DEEP_SLEEP_FAST_RESUME_AUDIT.md §7.1 (which cites
# esp_hw_support/sleep_modes.c:2521-2535). Only the values that document pins
# down are named; anything else is printed as a bare number on purpose.
WAKE_CAUSES = {
    0: "UNDEFINED",
    4: "TIMER",
    6: "EXT1",
}

T_RESET = re.compile(r"reset=(\d+)(?:\((.+?)\))?\s+cause=(\d+)")
T_BOOT_FIELD = re.compile(r"(\w+)=([-\w]+)")

T_PARK_START = re.compile(r"^t=(\d+)\s+standby enter\b")
T_PARK_RAIL = re.compile(r"^t=(\d+)\s+standby park: (.*)$")
T_PARK_RESUME = re.compile(r"^t=(\d+)\s+standby resume: (.*)$")

T_WAKEREC_PARK = re.compile(
    r"^t=(\d+)\s+wakerec park rc=0x10 (?:(\d+) mAh|FAIL \(([^)]*)\))"
    r"(?: trim_pa=(\d+) trim_sd=(\d+) slices=(\d+) up=(\d+) ms)?"
)
T_WAKEREC_RESUME = re.compile(
    r"^t=(\d+)\s+wakerec resume rc=0x10 (?:(\d+) mAh|FAIL \(([^)]*)\))"
    r"(?: d=([+-]?\d+) mAh over (\d+) ms)?"
    r"(?: \(([-\d.]+) mA\))?"
    r"(?: trim_pa=(\d+) trim_sd=(\d+))?"
    r"(?: slices_at_park=(\d+))?"
)
T_RUNTIME = re.compile(r"\[(runtime override|build default)\]")

# The two park lines are NOT distinguishable by which pin they mention: the
# PA-only line says `P1.4 BT/PA rail cut (P0.5 untouched)` -- it contains "P0.5"
# as metadata -- and the vendor-order CAM line says
# `P0.5 CAM_SCR rail cut (P1.4 still up)` -- it contains "P1.4" as metadata.
# Classify by the CUT clause, never by pin presence. Getting this wrong in either
# direction makes the tool report a CAM cut that never ran, or hide one that did.
def classify_rail(text):
    t = " ".join(text.split())
    if "CAM_SCR rail cut" in t:
        return "cam"
    if "BT/PA rail cut" in t or "P1.4 BT/PA rail cut" in t:
        return "pa"
    return None

# A park shorter than this cannot support a current figure: the coulomb counter
# resolves 1 mAh, so sub-ten-minute parks divide a 1-2 LSB delta by a tiny window
# and produce hundreds of mA. The delta is still printed; the rate is not.
MIN_ATTRIB_MS = 10 * 60 * 1000
T_SD_REMOUNT = re.compile(r"SD remount ok in (\d+) ms after (\d+) slices")

# Prefixes we own. A line starting with `t=` (or carrying one of these markers)
# that matches nothing is reported rather than counted as noise.
OURS = ("t=", "reset=", "wakerec", "standby", "trim:")

# Journal messages that are deliberately not tabulated. They are still listed in
# the report under INFORMATIONAL -- recognised, not silently dropped.
INFORMATIONAL_TAGS = (
    "autoflush", "standby enter", "standby park: parking SD", "wake hold",
    "slp level", "standby abort", "trim:",
)


def strip_cr(line):
    return line.rstrip("\r\n").replace("\r", "")


def arm_label(pa, sd, cam):
    def v(x):
        return "?" if x is None else str(x)
    return "pa=%s sd=%s cam=%s" % (v(pa), v(sd), v(cam))


class Park:
    """One park/resume pair, plus everything the journal said around it."""

    def __init__(self, lineno):
        self.lineno = lineno
        self.t_park = None
        self.start_ms = None       # millis() at park (`up=`), if the log has it
        self.open_ms = None        # t= of the wakerec park line
        self.mah_park = None
        self.trim_pa = None
        self.trim_sd = None
        self.slices = None
        self.pa_line = None        # text of the P1.4 park line
        self.cam_line = None       # text of the P0.5 park line, when present
        self.cam_suffix = None     # "[runtime override]" / "[build default]"
        self.weights = []          # arm suffixes seen on the P1.4/P0.5 lines

        self.t_resume = None
        self.mah_resume = None
        self.delta_mah = None
        self.over_ms = None
        self.ma = None
        self.slices_at_resume = None
        self.sd_remount = None     # (ms, slices) from the resume line
        self.notes = []

    # ---- derived ------------------------------------------------------------
    @property
    def duration_ms(self):
        if self.over_ms is not None:
            return self.over_ms
        if self.t_park is not None and self.t_resume is not None:
            return self.t_resume - self.t_park
        return None

    @property
    def cam(self):
        if self.cam_line is not None:
            return 1
        if self.pa_line is not None and "P0.5 untouched" in self.pa_line:
            return 0
        return None

    @property
    def close_line(self):
        return "wakerec resume" if self.mah_resume is not None or self.notes else "missing"

    def arm(self):
        return arm_label(self.trim_pa, self.trim_sd, self.cam)

    def effective_ma(self):
        if self.ma is not None:
            return self.ma
        d, ms = self.delta_mah, self.duration_ms
        if d is None or not ms:
            return None
        return (float(d) * 3600000.0) / float(ms)

    def effective_mah(self):
        if self.delta_mah is not None:
            return float(self.delta_mah)
        if self.mah_park is not None and self.mah_resume is not None:
            return float(self.mah_resume - self.mah_park)
        return None

    def is_closed(self):
        return self.t_resume is not None


class Boot:
    def __init__(self, lineno, raw, reset, name, cause, fields):
        self.lineno = lineno
        self.raw = raw
        self.reset = reset
        self.name = name
        self.cause = cause
        self.fields = fields


def parse(lines, unparsed):
    """Return (parks, boots, notes, informational). `unparsed` is appended to in place."""
    parks = []
    boots = []
    notes = []
    informational = []
    cur = None        # open Park
    recent_rails = [] # (lineno, text) of `standby park:` rail lines, unconsumed

    def close_pair(p, why=None):
        if why:
            p.notes.append(why)
        parks.append(p)

    def flush_rail_orphans(reason):
        """A rail-cut line with no wakerec park line after it is a park that never
        finished. r101.19 is exactly that shape: `P1.4 ... rail cut` then a reset
        and nothing else -- the three lines that follow a completed cut are
        missing, which is the evidence, so it must appear as a row."""
        nonlocal recent_rails
        for (rline, rtext) in recent_rails:
            p = Park(rline)
            kind = classify_rail(rtext)
            if kind == "cam":
                p.cam_line = rtext
            elif kind == "pa":
                p.pa_line = rtext
            p.notes.append("PARK DID NOT COMPLETE: rail cut line with no `wakerec park` after it (%s)" % reason)
            parks.append(p)
        recent_rails = []

    for idx, raw in enumerate(lines, start=1):
        line = strip_cr(raw)
        s = line.strip()
        if not s:
            continue
        # A leading console prefix ("I (123) tag:") is tolerated by searching
        # for the first t= / reset= we recognise.
        m_t = re.search(r"t=(\d+)\s+", s)
        norm = s
        if m_t and not s.startswith("t="):
            # Keep the tail from the FIRST t= so a prefixed line still matches.
            norm = s[s.find("t="):]

        m = T_RESET.search(norm)
        if m and "aborted=" in norm:
            fields = dict(T_BOOT_FIELD.findall(norm))
            try:
                reset = int(m.group(1))
                cause = int(m.group(3))
            except ValueError:
                unparsed.append((idx, "boot header: bad reset/cause number", norm))
                continue
            flush_rail_orphans("a boot header follows the cut: the device reset before the park finished")
            boots.append(Boot(idx, norm, reset, (m.group(2) or "").strip(), cause, fields))
            continue
        if norm.startswith("reset=") or " reset=" in norm:
            # Ours, but unreadable: a truncated capture cuts the field list off
            # mid-line (GLASS_CHECKS.md H6 quotes exactly that for r101.19).
            unparsed.append((idx, "boot header TRUNCATED (no cause=/aborted= -- cannot decode)", norm))
            continue

        m = T_WAKEREC_PARK.match(norm)
        if m:
            if cur is not None and not cur.is_closed():
                cur.notes.append("previous park never resumed before this one")
                close_pair(cur, "UNCLOSED: a new park started before a resume line")
            cur = Park(idx)
            cur.t_park = int(m.group(1))
            cur.open_ms = cur.t_park
            if m.group(2) is not None:
                cur.mah_park = int(m.group(2))
            else:
                cur.notes.append("PARK GAUGE READ FAILED (%s)" % m.group(3))
            if m.group(4) is not None:
                cur.trim_pa = int(m.group(4))
                cur.trim_sd = int(m.group(5))
                cur.slices = int(m.group(6))
                cur.start_ms = int(m.group(7))
            else:
                cur.notes.append("park line TRUNCATED: trim_pa/trim_sd/slices/up= missing")
            # Attach the rail lines this park wrote just before the recorder.
            # ORDER MATTERS: the vendor-order CAM line reads
            #   `standby park: P0.5 CAM_SCR rail cut (P1.4 still up) [vendor order]`
            # which CONTAINS the string "P1.4". Testing for P1.4 first claims that
            # line as the PA cut and leaves the CAM cut unrecorded -- the tool then
            # reports "no P0.5 line" for exactly the parks where the P0.5 cut DID
            # run, which is the one misread this tool exists to prevent.
            for (rline, rtext) in reversed(recent_rails):
                kind = classify_rail(rtext)
                if kind == "cam" and cur.cam_line is None:
                    cur.cam_line = rtext
                elif kind == "pa" and cur.pa_line is None:
                    cur.pa_line = rtext
            if cur.pa_line is None and cur.cam_line is None:
                cur.notes.append("NO rail line before this park: arm unknown (log truncated?)")
            recent_rails = []
            continue

        m = T_WAKEREC_RESUME.match(norm)
        if m:
            t = int(m.group(1))
            lost = "NO PARK RECORD" in norm
            if cur is None or cur.is_closed() or lost:
                if cur is not None and not cur.is_closed():
                    # The park half exists but the recorder state did not survive
                    # to the resume: close the park as unpaired, never invent a delta.
                    close_pair(cur, "UNCLOSED: resume says NO PARK RECORD (a reboot happened between the halves)")
                    cur = None
                orphan = Park(idx)
                orphan.t_resume = t
                if m.group(2) is not None:
                    orphan.mah_resume = int(m.group(2))
                orphan.notes.append("ORPHAN resume: no park record (a reboot happened, or the log starts mid-pair)")
                parks.append(orphan)
                continue
            cur.t_resume = t
            if m.group(2) is not None:
                cur.mah_resume = int(m.group(2))
            else:
                cur.notes.append("RESUME GAUGE READ FAILED (%s) -- no usable delta" % m.group(3))
            if m.group(4) is not None:
                cur.delta_mah = int(m.group(4))
                cur.over_ms = int(m.group(5))
            else:
                sep = " -- " if cur.notes else ""
                cur.notes.append(sep + "no delta (reboot between park and resume, or truncated line)")
            if m.group(6) is not None:
                cur.ma = float(m.group(6))
            if m.group(7) is not None and cur.trim_pa is None:
                cur.trim_pa = int(m.group(7))
                cur.trim_sd = int(m.group(8))
            if m.group(9) is not None:
                cur.slices_at_resume = int(m.group(9))
                if cur.slices is None:
                    cur.slices = cur.slices_at_resume
            if cur.over_ms is None and cur.t_park is not None and cur.t_resume is not None:
                if cur.t_resume < cur.t_park:
                    cur.notes.append("resume t= precedes park t= (log reordered or the clock restarted); "
                                     "duration withheld")
                else:
                    cur.over_ms = cur.t_resume - cur.t_park
            close_pair(cur)
            cur = None
            continue

        m = T_PARK_RAIL.match(norm)
        if m:
            text = m.group(2).strip()
            if text.startswith("parking SD"):
                # Informational, not a rail: `standby park: parking SD` shares the
                # `standby park:` prefix and must not be mistaken for a cut.
                informational.append((idx, norm))
                continue
            recent_rails.append((idx, text))
            continue

        m = T_PARK_RESUME.match(norm)
        if m:
            text = m.group(2)
            sd = T_SD_REMOUNT.search(text)
            if sd and cur is not None:
                cur.sd_remount = (int(sd.group(1)), int(sd.group(2)))
            continue

        if T_PARK_START.match(norm):
            continue

        if norm.startswith("trim:") or " trim: " in norm:
            notes.append("arm change at line %d: %s" % (idx, norm.strip()))
            continue

        if norm.startswith("t="):
            msg = norm.split(None, 1)[1] if " " in norm else ""
            if msg.startswith(INFORMATIONAL_TAGS):
                informational.append((idx, norm))
            else:
                unparsed.append((idx, "t=<ms> line matched no known format (truncated?)", norm))
        elif norm.startswith(OURS):
            unparsed.append((idx, "journal-shaped line matched no known format", norm))
        # Anything else (a panic dump, a shell prompt, a UI log line) is not ours
        # and is not counted: the analyser is fed whole console captures.

    if cur is not None and not cur.is_closed():
        close_pair(cur, "UNCLOSED: the log ends (or the device reset) before a resume line")
    if recent_rails:
        flush_rail_orphans("the log ends before the park finished")

    # Second pass: the `[runtime override]` / `[build default]` suffix, per rail line.
    for p in parks:
        for text in (p.pa_line, p.cam_line):
            if not text:
                continue
            m = T_RUNTIME.search(text)
            p.weights.append(m.group(1) if m else "no weight suffix?")
        if p.cam_line is not None:
            m = T_RUNTIME.search(p.cam_line)
            p.cam_suffix = m.group(1) if m else None

    return parks, boots, notes, informational


def fmt_dur(ms):
    if ms is None:
        return "-"
    total = ms / 1000.0
    if total < 120:
        return "%.1f s" % total
    if total < 7200:
        return "%.1f min" % (total / 60.0)
    return "%.2f h" % (total / 3600.0)


def summarise(parks):
    """Weighted mA per arm. Weight = mAh / hours, not the mean of per-park mA."""
    groups = {}
    for p in parks:
        if not p.is_closed():
            continue
        mah = p.effective_mah()
        dur = p.duration_ms
        if mah is None or dur is None or dur <= 0:
            continue
        key = p.arm()
        g = groups.setdefault(key, {"parks": 0, "ms": 0, "mah": 0.0, "skipped": 0})
        g["parks"] += 1
        g["ms"] += dur
        g["mah"] += mah
    out = []
    for key, g in sorted(groups.items(), key=lambda kv: -kv[1]["ms"]):
        hours = g["ms"] / 3600000.0
        ma = g["mah"] / hours if hours else None
        out.append((key, g["parks"], hours, g["mah"], ma))
    return out


def print_report(parks, boots, notes, informational, unparsed, out=None):
    w = out or sys.stdout

    w.write("PARKS — one row per wakerec park/resume pair\n")
    if not parks:
        w.write("  (no park/resume pair found)\n")
    else:
        w.write("  %-3s %-6s %-9s %-10s %-6s %-6s %-6s %-5s %-16s %-22s %s\n" % (
            "#", "line", "start(ms)", "duration", "slices", "mAh", "mA", "closed", "arm", "P0.5 line", "notes"))
        for i, p in enumerate(parks, start=1):
            start = p.start_ms if p.start_ms is not None else p.t_park
            mah = p.effective_mah()
            ma = p.effective_ma()
            # A park shorter than MIN_ATTRIB_MS cannot support a current figure:
            # the gauge is 1 mAh, so 3 mAh over 4 minutes divides to a meaningless
            # number (a 3.7 s stub park printed -4827 mA). Print the delta, refuse
            # the rate, and say why -- a nonsense mA in this column is exactly the
            # figure that gets quoted in a later document.
            short = (p.duration_ms is not None and p.duration_ms < MIN_ATTRIB_MS)
            if short and ma is not None:
                p.notes.append("TOO SHORT: %s < %s, mA not attributable"
                               % (fmt_dur(p.duration_ms), fmt_dur(MIN_ATTRIB_MS)))
                ma = None
            if p.cam_line is not None:
                camtxt = "cut [%s]" % (p.cam_suffix or "no suffix")
            elif p.cam == 0:
                m = T_RUNTIME.search(p.pa_line or "")
                camtxt = "untouched [%s]" % (m.group(1) if m else "no suffix")
            else:
                camtxt = "? (no P0.5 line; arm unknown)"
            w.write("  %-3d %-6d %-9s %-10s %-6s %-6s %-6s %-5s %-16s %-22s %s\n" % (
                i, p.lineno,
                ("%d" % start) if start is not None else "-",
                fmt_dur(p.duration_ms),
                ("%d" % p.slices) if p.slices is not None else "-",
                ("%+d" % mah) if isinstance(mah, float) and mah == int(mah) else (
                    ("%.1f" % mah) if mah is not None else "-"),
                ("%.2f" % ma) if ma is not None else "-",
                "yes" if p.is_closed() else "MISSING",
                p.arm(),
                camtxt,
                "; ".join(p.notes)))

    w.write("\nSUMMARY — weighted mA per arm (total mAh / total h, the PWR3 method)\n")
    # Only parks long enough for the rate to mean anything. A 4-minute park's
    # 2 mAh LSB divides out to hundreds of mA and poisons the weighted average.
    keep = [p for p in parks if p.duration_ms is None or p.duration_ms >= MIN_ATTRIB_MS]
    skipped = len([p for p in parks if p.duration_ms is not None and p.duration_ms < MIN_ATTRIB_MS])
    rows = summarise(keep)
    if skipped:
        w.write("  (%d park(s) shorter than %s excluded from the rate; their deltas are in the table above)\n"
                % (skipped, fmt_dur(MIN_ATTRIB_MS)))
    if not rows:
        w.write("  (no closed park with both a delta and a duration)\n")
    else:
        w.write("  %-18s %-7s %-9s %-9s %s\n" % ("arm", "parks", "hours", "mAh", "mA"))
        for key, n, hours, mah, ma in rows:
            w.write("  %-18s %-7d %-9.3f %-9.1f %s\n" % (
                key, n, hours, mah, ("%.2f" % ma) if ma is not None else "-"))
        w.write("  A/B: compare a `trim pa off` row against the pa=1 row above; anything within\n"
                "       1 mAh over a short park is one counter LSB, not a result.\n")

    w.write("\nBOOTS — reset decoder (esp_system.h:25-40; NOT the legend at main.cpp:2951)\n")
    if not boots:
        w.write("  (no reset= header found)\n")
    else:
        for b in boots:
            name = RESET_REASONS.get(b.reset, "UNKNOWN(%d)" % b.reset)
            src = b.name or "no name in line"
            cause = WAKE_CAUSES.get(b.cause, "%d (not in this target's documented table)" % b.cause)
            aborted = b.fields.get("aborted", "?")
            line = "  line %-5d reset=%-2d %-10s cause=%-2d %-12s aborted=%s touch=%s armedLevel=%s" % (
                b.lineno, b.reset, name, b.cause, cause, aborted,
                b.fields.get("touch", "?"), b.fields.get("armedLevel", "?"))
            w.write(line + "\n")
            w.write("        (line says `%s`)\n" % src)
            if name != src.split("(")[0].strip() and src:
                w.write("        NOTE: the name in the line disagrees with esp_system.h; trust the number.\n")
            if b.reset == 9:
                w.write("        VERDICT: BROWNOUT — supply was lost. This is NOT a deep-sleep wake;\n"
                        "                 main.cpp:2951 calls 9 deep-sleep and is wrong.\n")
            elif b.reset == 8 and b.cause == 6:
                w.write("        VERDICT: genuine deep-sleep wake on EXT1 (pin assertion). aborted should be 0.\n")
            elif b.reset == 8:
                w.write("        VERDICT: deep-sleep wake, cause %s.\n" % cause)
            elif aborted == "1":
                w.write("        VERDICT: SLEEP ABORTED — the sleep call returned and the wrapper esp_restart()ed\n"
                        "                 (reset 3 or 4 expected). The device never slept; discard this park.\n")
            elif b.reset == 1:
                w.write("        VERDICT: cold power-on. A RAM-only `trim` override set before this line is GONE.\n")
            elif b.reset == 3:
                w.write("        VERDICT: SW reset (esp_restart or the aborted-sleep path).\n")
            elif b.reset == 4:
                w.write("        VERDICT: PANIC — read the panic dump, and a park that ends here is UNCLOSED.\n")

    w.write("\nARM CHANGES / OTHER JOURNAL NOTES\n")
    if not notes:
        w.write("  (none)\n")
    else:
        for n in notes:
            w.write("  - %s\n" % n)

    if unparsed:
        w.write("\nUNPARSED — %d journal-shaped line(s) this analyser could not read (NOT skipped silently)\n"
                % len(unparsed))
        for idx, why, text in unparsed:
            w.write("  line %-5d %-45s | %s\n" % (idx, why, text[:110]))
    else:
        w.write("\nUNPARSED — none; every journal-shaped line was read.\n")

    if informational:
        w.write("\nINFORMATIONAL — %d recognised line(s) deliberately not tabulated (counted, not dropped)\n"
                % len(informational))
        for idx, text in informational[:8]:
            w.write("  line %-5d | %s\n" % (idx, text[:110]))
        if len(informational) > 8:
            w.write("  ... and %d more (autoflush / SD-park / wake-hold chatter)\n" % (len(informational) - 8))


SELFTEST = """\
t=609813 standby park: P1.4 BT/PA rail cut (P0.5 untouched) [build default]
t=609817 wakerec park rc=0x10 2000 mAh trim_pa=1 trim_sd=1 slices=0 up=609684 ms
t=609841 autoflush park ok 24615 B in 19 ms
t=609844 standby park: parking SD
t=2077214 standby resume: SD remount ok in 68 ms after 25 slices (settle budget 250 ms)
t=2077324 wakerec resume rc=0x10 2000 mAh d=+0 mAh over 1467636 ms (0.00 mA) trim_pa=1 trim_sd=1 slices_at_park=0
"""


def main(argv):
    args = [a for a in argv[1:] if a != "--selftest"]
    if "--selftest" in argv:
        u = []
        print_report(*parse(SELFTEST.splitlines(), u), unparsed=u)
        return 0

    if not args:
        sys.stderr.write("usage: parkstats.py <logfile|-> [--selftest]\n")
        return 2
    path = args[0]
    if path == "-":
        text = sys.stdin.read()
    else:
        try:
            with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
                text = fh.read()
        except OSError as e:
            sys.stderr.write("parkstats: cannot read %s: %s\n" % (path, e))
            return 2

    unparsed = []
    parks, boots, notes, informational = parse(text.splitlines(), unparsed)
    if not parks and not boots:
        sys.stderr.write("parkstats: no journal lines recognised in %d line(s). Nothing printed.\n"
                         % len(text.splitlines()))
        return 2
    print_report(parks, boots, notes, informational, unparsed)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
