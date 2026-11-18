#!/usr/bin/env python3
"""HOST gate runner -- one command, and it knows which reds are registered.

`ctest` exits 1 whenever a check fails, which is correct for a suite with no
expected failures and useless for a suite that deliberately holds contracts red
until a scheduled batch fixes the seams they need. This runner is the single
entry point:

    python tools/batch0_runner.py [build-dir] [--label batch0]

GATE2 -- SCOPE. The runner used to execute ONLY the `batch0` label (45 checks)
and report GATE PASS over it, while the other ~937 tests in the same tree were
never run at all. A gate that passes over a suite it does not run is worse than
no gate: `recursive_dictionary` and
`CssParserTest.FailedPromotionRestoresTheOnlyBackupCache` both sat red outside
that label and were invisible. The default is now the WHOLE ctest tree. `--label`
narrows the scope on purpose, and says so in its own output; a narrowed run is a
developer convenience, not a release verdict.

Classification, unchanged, over whatever the scope runs:

  * red AND registered   -> reported as RED-WITH-REGISTER, does not fail the run
  * red and NOT registered -> reported as UNREGISTERED, exit 1
  * registered but GREEN -> reported as REGISTER-STALE, exit 1
    (fixed? delete the line. disabled? that is the rot this exists to stop.)
  * everything else passes -> exit 0

Before any of that it proves the register is *resolvable*: every registered
name must appear in the list of tests `ctest -N` discovers in this scope. A
registered name that resolves to nothing was never built (or its target never
ran `gtest_discover_tests`), so its absence says nothing at all about whether
the defect was fixed. Such a name is reported as

    REGISTER-UNRESOLVABLE: <name> has no matching test case ...

and the run exits 1 with the remedy "build the target". Without this preflight
a fresh or half-built directory produced `REGISTER-STALE` ("no longer red,
delete its line") for both register entries -- inviting deletion of two open
defects from the register and the claim that they were fixed.

Exit 0 therefore means exactly: "in this scope, every registered check was
actually built and discovered, every check that should be green is green, and
every check that is red is a named, owned, scheduled debt."
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTER = os.path.join(ROOT, "test", "EXPECTED_RED.txt")
# None = the whole ctest tree (see GATE2 in the module docstring). A string is a
# deliberate narrowing and is echoed in the verdict so nobody mistakes it for the gate.
DEFAULT_LABEL = None
DEFAULT_MIN_TESTS = 1

# `ctest -N -L batch0` lines look like "  Test #614: PolicyTests.KnownRed_...".
DISCOVER_RE = re.compile(r"^\s*Test\s+#\d+:\s+(.+?)\s*$")
# `ctest -L batch0` failure summary lines:
#   "   10 - Name (Failed)" / "(Not Run)" / "(SEGFAULT)" / "(Timeout)" ...
FAIL_RE = re.compile(r"^\s*\d+\s+-\s+(\S+)\s+\(([^)]*)\)")
TOTAL_RE = re.compile(r"tests failed out of (\d+)")
# Verdicts that mean "the check never ran", which is not a red check and not a pass.
NOT_RUN_MARKERS = ("not run", "unable to find", "missing")


def is_not_run(reason):
    """Pure: ctest said the check never executed (binary absent or unloadable)."""
    lowered = reason.lower()
    return any(marker in lowered for marker in NOT_RUN_MARKERS)


def load_register(path=None):
    """Parse the register: `<test> <defect-id> <batch> [reason]` per line."""
    entries = []
    path = path or REGISTER
    if not os.path.exists(path):
        return entries
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(None, 3)
            if len(parts) < 3:
                raise SystemExit("EXPECTED_RED.txt: bad line %r "
                                 "(want `<test> <defect-id> <batch> [reason]`)" % line)
            entries.append({
                "test": parts[0],
                "id": parts[1],
                "batch": parts[2],
                "reason": parts[3] if len(parts) > 3 else "",
            })
    return entries


def parse_discovered(output):
    """Pure: test names from `ctest -N` output."""
    names = []
    for line in output.splitlines():
        m = DISCOVER_RE.match(line)
        if m and m.group(1) not in names:
            names.append(m.group(1))
    return names


def parse_failures(output):
    """Pure: (name, reason) for every per-test verdict in ctest output."""
    failed = []
    seen = set()
    for line in output.splitlines():
        m = FAIL_RE.match(line)
        if m and m.group(1) not in seen:
            seen.add(m.group(1))
            failed.append((m.group(1), m.group(2)))
    return failed


def parse_total(output):
    """Pure: `tests failed out of N`, or None when ctest did not say."""
    total = None
    for cand in TOTAL_RE.finditer(output):
        total = int(cand.group(1))
    return total


def preflight(discovered, register, min_tests=DEFAULT_MIN_TESTS, scope=None):
    """Pure: the 'was this even built?' checks that run before classification.

    Returns a list of (verdict, detail) problems; empty means resolvable.
    """
    problems = []
    if len(discovered) < min_tests:
        problems.append(("NO-TESTS-DISCOVERED",
                         "ctest discovered %d test(s)%s, floor is %d. "
                         "Configure and build the test tree before running the gate."
                         % (len(discovered),
                            " on the %r label" % scope if scope else " (whole tree)",
                            min_tests)))
    known = set(discovered)
    for entry in register:
        if entry["test"] not in known:
            problems.append(("REGISTER-UNRESOLVABLE",
                             "%s has no matching test case%s -- the test was never "
                             "built/discovered, this is NOT a fix"
                             % (entry["test"],
                                " in this scope" if scope else "")))
    return problems


def classify(discovered, failed, register):
    """Pure: red/green classification, run only once preflight has passed.

    A name that is registered but was not discovered cannot reach here as
    'stale' -- preflight has already refused the run.
    """
    known = set(discovered)
    failed_names = {name for name, _ in failed}
    registered = {e["test"]: e for e in register}

    # A check ctest could not execute has no verdict at all: it is neither red
    # nor green, so it must not be classified as either.
    not_run = [(name, how) for name, how in failed if is_not_run(how)]
    verdicts = [(name, how) for name, how in failed if not is_not_run(how)]

    red_with_register = [(e, how) for e in register
                         for name, how in verdicts if name == e["test"]]
    unregistered = [(name, how) for name, how in verdicts
                    if name not in registered and name in known]
    stale = [e for e in register
             if e["test"] not in failed_names and e["test"] not in {n for n, _ in not_run}]

    return {
        "not_run": not_run,
        "red_with_register": red_with_register,
        "unregistered": unregistered,
        "stale": stale,
    }


class CtestProbe:
    """The only part that touches ctest; injectable so the test needs no compiler."""

    def __init__(self, label=None):
        # None = the whole tree (GATE2). Any label is an explicit, narrowed scope.
        self.label = label

    def _scope(self):
        return ["-L", self.label] if self.label else []

    @staticmethod
    def _env():
        # Windows resolves libstdc++-6.dll through PATH. Git-bash's older /mingw64 copy
        # lacks symbols the UCRT64 build needs (`__codecvt_utf8_utf16_base<wchar_t>`), and
        # the miss raises a MODAL "Entry Point Not Found" dialog that blocks the test until
        # a human clicks it. Put the toolchain's own bin first so ctest can never pop it.
        env = dict(os.environ)
        toolchain = r"C:\msys64\ucrt64\bin"
        if os.path.isdir(toolchain):
            env["PATH"] = toolchain + os.pathsep + env.get("PATH", "")
        return env

    def discover(self, build_dir):
        cmd = ["ctest", "--test-dir", build_dir, "-N"] + self._scope()
        print("$ " + " ".join(cmd))
        proc = subprocess.run(cmd, capture_output=True, text=True, env=self._env())
        return proc.returncode, proc.stdout + proc.stderr

    def run(self, build_dir):
        # A hard per-test timeout: a test that hangs (or pops a Windows loader error dialog)
        # used to hold the whole gate open indefinitely, which is not a verdict either.
        cmd = ["ctest", "--test-dir", build_dir] + self._scope() + \
              ["--output-on-failure", "--timeout", "300"]
        print("$ " + " ".join(cmd))
        proc = subprocess.run(cmd, capture_output=True, text=True, env=self._env())
        return proc.returncode, proc.stdout + proc.stderr


def main(argv=None, probe=None):
    parser = argparse.ArgumentParser(description="Host gate runner")
    parser.add_argument("build_dir", nargs="?", default="build-b0-tests")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="dump the full ctest output")
    parser.add_argument("--min-tests", type=int, default=DEFAULT_MIN_TESTS,
                        help="fail if fewer than N tests are discovered "
                             "(guards a half-configured build dir)")
    parser.add_argument("--label", default=None,
                        help="NARROW the scope to this ctest label for a developer loop. "
                             "Omit it for the gate: the default scope is the whole tree "
                             "(GATE2). A narrowed run is not a release verdict.")
    parser.add_argument("--register", default=None,
                        help="alternative EXPECTED_RED register (used by tests)")
    args = parser.parse_args(argv)
    probe = probe or CtestProbe(args.label)
    scope_label = args.label

    register = load_register(args.register)

    # --- Preflight: prove every registered name is a test that exists. ------
    disc_rc, disc_out = probe.discover(args.build_dir)
    discovered = parse_discovered(disc_out)

    problems = preflight(discovered, register, args.min_tests, scope_label)
    if problems:
        print()
        print("=" * 72)
        print("HOST GATE — PREFLIGHT FAILED (no verdict on any check)")
        print("=" * 72)
        for verdict, detail in problems:
            print("%-21s %s" % (verdict + ":", detail))
        print()
        print("GATE FAIL: the register cannot be classified against this build dir. "
              "Build the test tree (cmake -S test -B <dir>, cmake --build <dir>) and "
              "re-run; do NOT delete register lines to make this go away.")
        if args.verbose:
            print(disc_out)
        else:
            print("(re-run with -v to see the full ctest -N output)")
        return 1

    # --- Classification, only now that every registered name resolved. -------
    rc, out = probe.run(args.build_dir)
    total = parse_total(out)
    failed = parse_failures(out)

    # ctest can fail for reasons that are not a per-test verdict (configure
    # error, binary missing). Say so rather than reporting a clean classification.
    if rc != 0 and not failed:
        print(out)
        print("HOST GATE FAILED: ctest exited %d with no per-test verdict "
              "(build or configure problem, not a red check)." % rc)
        return 1

    verdicts = classify(discovered, failed, register)

    print()
    print("=" * 72)
    print("HOST GATE — %s discovered, %s check(s) ran, %d failed, %d registered-red"
          % (len(discovered), total if total is not None else "?",
             len(failed), len(register)))
    print("SCOPE: %s" % ("whole ctest tree" if scope_label is None
                         else "label %r ONLY (narrowed; not a release verdict)" % scope_label))
    print("=" * 72)

    for e in register:
        how = next((h for t, h in failed if t == e["test"] and not is_not_run(h)), None)
        skipped = next((h for t, h in failed if t == e["test"] and is_not_run(h)), None)
        if how:
            print("RED-WITH-REGISTER  %-62s %s (%s)" % (e["test"], e["id"], how))
            print("                   closes in %s — %s" % (e["batch"], e["reason"]))
        elif skipped:
            print("TEST-NOT-RUN       %-62s %s (%s)" % (e["test"], e["id"], skipped))
        else:
            print("REGISTER-STALE     %-62s %s" % (e["test"], e["id"]))
    for t, how in verdicts["unregistered"]:
        print("UNREGISTERED       %-62s (%s)" % (t, how))
    for t, how in verdicts["not_run"]:
        if t not in {e["test"] for e in register}:
            print("TEST-NOT-RUN       %-62s (%s)" % (t, how))

    if not verdicts["stale"] and not verdicts["unregistered"] and not verdicts["not_run"]:
        print()
        print("GATE PASS: every registered check was built and discovered; every "
              "check that should be green is green; every red one is registered "
              "with an owner and a batch.")
        print("SCOPE ADVISORY: %s" % ("the whole tree was run."
                                      if scope_label is None
                                      else "this was a NARROWED run (label %r); the release "
                                           "verdict needs the whole tree." % scope_label))
        return 0

    print()
    if verdicts["unregistered"]:
        print("GATE FAIL: %d red check(s) nobody has registered. Either fix them, or "
              "add a line to test/EXPECTED_RED.txt with the defect id and the batch "
              "that closes them." % len(verdicts["unregistered"]))
    if verdicts["stale"]:
        print("GATE FAIL: %d registered check(s) ran and did not fail. If one was "
              "fixed, delete its line from test/EXPECTED_RED.txt in the same commit; "
              "if it was disabled, that is the rot this register exists to stop."
              % len(verdicts["stale"]))
    if verdicts["not_run"]:
        print("GATE FAIL: %d batch-0 check(s) never ran (binary absent, not built or "
              "not runnable). That is not a pass and not a red -- build the target "
              "and re-run the gate." % len(verdicts["not_run"]))
    if args.verbose:
        print(out)
    else:
        print("(re-run with -v to see the full ctest output)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
