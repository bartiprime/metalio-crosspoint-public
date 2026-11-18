#!/usr/bin/env python3
"""Fixture tests for tools/batch0_runner.py — no compiler, no ctest, no build dir.

The gate's most expensive mistake was silent: a registered red whose binary was
never built was classified REGISTER-STALE ("no longer red, delete its line"),
which is the opposite of the truth. These cases pin the verdict for every shape
of input the runner can be handed:

  * registered + discovered + failed   -> RED-WITH-REGISTER, exit 0
  * registered + NOT discovered        -> REGISTER-UNRESOLVABLE, exit 1
  * registered + discovered + passed   -> REGISTER-STALE, exit 1
  * unregistered + failed              -> UNREGISTERED, exit 1
  * nothing discovered at all          -> NO-TESTS-DISCOVERED, exit 1

The runner's ctest access is behind an injectable probe, so this stays pure
Python: `python test/test_batch0_runner_classification.py` (also collected by
ctest as `Batch0RunnerClassification`).
"""

import contextlib
import importlib.util
import io
import itertools
import os
import shutil
import unittest

from scratch_dir import SCRATCH_ROOT

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER_PATH = os.path.join(ROOT, "tools", "batch0_runner.py")

# r165.5: the fixture registers live in _scratch/ inside the repo. They used to be mkstemp() files
# under %TEMP%, which both broke the "nothing outside the working directory" rule and left debris
# behind whenever a case failed before its os.unlink().
SCRATCH = str(SCRATCH_ROOT / "batch0_runner_classification")
shutil.rmtree(SCRATCH, ignore_errors=True)
_fixtures = itertools.count(1)

RED_A = "PolicyTests.KnownRed_FailedCompletionDropsShadowAndArmsFromWhite"
RED_B = "StandbyResumeOrder.TouchWakeIsNotIssuedBeforeTheCamRailIsRestored"
GREEN = "PolicyTests.FastAfterADiffDriveStaysFast"
UNREGISTERED_RED = "PolicyTests.SomebodyForgotTheRegister"


def load_runner():
    spec = importlib.util.spec_from_file_location("batch0_runner", RUNNER_PATH)
    if spec is None or spec.loader is None:  # pragma: no cover - path is fixed
        raise SystemExit("cannot load %s" % RUNNER_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNNER = load_runner()


class FakeProbe:
    """Stands in for `ctest`: says what was discovered and what failed."""

    def __init__(self, discovered, failed=()):
        self.discovered = list(discovered)
        self.failed = list(failed)
        self.ran = False

    def discover(self, build_dir):
        return 0, self._listing()

    def run(self, build_dir):
        self.ran = True
        lines = ["Test project " + str(build_dir)]
        if self.failed:
            lines.append("")
            lines.append("The following tests FAILED:")
            for i, (name, how) in enumerate(self.failed, start=1):
                lines.append("\t%3d - %s (%s)" % (i, name, how))
            lines.append("")
            lines.append("%d%% tests passed, %d tests failed out of %d"
                         % (0, len(self.failed), len(self.discovered)))
        else:
            lines.append("")
            lines.append("100% tests passed, 0 tests failed out of %d"
                         % len(self.discovered))
        return (1 if self.failed else 0), "\n".join(lines) + "\n"

    def _listing(self):
        lines = ["Test project x"]
        for i, name in enumerate(self.discovered, start=1):
            lines.append("  Test #%d: %s" % (i, name))
        lines.append("")
        lines.append("Total Tests: %d" % len(self.discovered))
        return "\n".join(lines) + "\n"


def write_register(entries, with_header=True):
    """`entries` = list of `<test> <id> <batch> [reason]` strings."""
    os.makedirs(SCRATCH, exist_ok=True)
    path = os.path.join(SCRATCH, "expected-red-%d.txt" % next(_fixtures))
    with open(path, "w", encoding="utf-8") as fh:
        if with_header:
            fh.write("# EXPECTED-RED fixture\n")
        for line in entries:
            fh.write(line + "\n")
    return path


def run_gate(build_dir, register_path, discovered, failed=(), min_tests=None):
    probe = FakeProbe(discovered, failed)
    argv = [build_dir, "--register", register_path]
    if min_tests is not None:
        argv += ["--min-tests", str(min_tests)]
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = RUNNER.main(argv, probe=probe)
    return code, buf.getvalue()


class RegisterUnresolvable(unittest.TestCase):
    """The GATE1 defect: 'never built' must never read as a verdict."""

    def setUp(self):
        self.discovered = [RED_A, RED_B, GREEN]
        self.reds = [RED_A, RED_B]

    def test_bogus_register_name_is_unresolvable_and_fails(self):
        register = write_register([
            "%s B-P2-1 Batch-1 completion drive failure branch" % RED_A,
            "PolicyTests.KnownRed_ThisCaseWasNeverWritten B-DEAD Batch-1 n/a",
            "%s TCH3 Batch-1 touch wake order" % RED_B,
        ])
        try:
            code, out = run_gate("fake-build", register, self.discovered,
                                 failed=[(RED_A, "Failed"), (RED_B, "Failed")])
        finally:
            os.unlink(register)
        self.assertEqual(1, code, out)
        self.assertIn("REGISTER-UNRESOLVABLE", out)
        self.assertIn("knownRed_ThisCaseWasNeverWritten".replace("k", "K", 1), out)
        # The wrong diagnosis must not appear for the unresolvable name.
        self.assertNotIn("REGISTER-STALE", out)
        # ...and no check may be classified while the register is unresolved.
        self.assertNotIn("RED-WITH-REGISTER", out)
        self.assertNotIn("GATE PASS", out)

    def test_bogus_name_removed_then_the_real_reds_pass_the_gate(self):
        register = write_register([
            "%s B-P2-1 Batch-1 completion drive failure branch" % RED_A,
            "%s TCH3 Batch-1 touch wake order" % RED_B,
        ])
        try:
            code, out = run_gate("fake-build", register, self.discovered,
                                 failed=[(RED_A, "Failed"), (RED_B, "Failed")])
        finally:
            os.unlink(register)
        self.assertEqual(0, code, out)
        self.assertNotIn("REGISTER-UNRESOLVABLE", out)
        self.assertEqual(2, out.count("RED-WITH-REGISTER"))
        self.assertIn("GATE PASS", out)

    def test_unbuilt_register_against_an_empty_dir_is_not_a_pass(self):
        register = write_register([
            "%s B-P2-1 Batch-1 x" % RED_A,
            "%s TCH3 Batch-1 y" % RED_B,
        ])
        try:
            code, out = run_gate("fake-build", register, discovered=[])
        finally:
            os.unlink(register)
        self.assertEqual(1, code, out)
        self.assertIn("REGISTER-UNRESOLVABLE", out)
        self.assertIn("NO-TESTS-DISCOVERED", out)

    def test_test_count_floor(self):
        register = write_register(["%s B-P2-1 Batch-1 x" % RED_A])
        try:
            code, out = run_gate("fake-build", register, [RED_A], min_tests=5)
        finally:
            os.unlink(register)
        self.assertEqual(1, code, out)
        self.assertIn("NO-TESTS-DISCOVERED", out)
        self.assertIn("floor is 5", out)


class Classification(unittest.TestCase):
    def setUp(self):
        self.discovered = [RED_A, RED_B, GREEN, UNREGISTERED_RED]

    def test_registered_that_ran_and_passed_is_stale(self):
        register = write_register([
            "%s B-P2-1 Batch-1 x" % RED_A,
            "%s TCH3 Batch-1 y" % RED_B,
        ])
        try:
            code, out = run_gate("fake-build", register, self.discovered,
                                 failed=[(RED_A, "Failed")])
        finally:
            os.unlink(register)
        self.assertEqual(1, code, out)
        self.assertIn("REGISTER-STALE", out)
        self.assertNotIn("REGISTER-UNRESOLVABLE", out)

    def test_unregistered_red_fails_the_gate(self):
        register = write_register([
            "%s B-P2-1 Batch-1 x" % RED_A,
            "%s TCH3 Batch-1 y" % RED_B,
        ])
        try:
            code, out = run_gate("fake-build", register, self.discovered,
                                 failed=[(RED_A, "Failed"), (RED_B, "Failed"),
                                         (UNREGISTERED_RED, "Failed")])
        finally:
            os.unlink(register)
        self.assertEqual(1, code, out)
        self.assertIn("UNREGISTERED", out)

    def test_missing_binary_is_not_run_and_not_a_pass(self):
        # ctest lists the case (it was discovered at build time) but says
        # "(Not Run)" when the executable is absent. That is neither red nor
        # green: it must not slide through as stale, and must not pass.
        register = write_register(["%s B-P2-1 Batch-1 x" % RED_A])
        try:
            code, out = run_gate("fake-build", register, [RED_A],
                                 failed=[(RED_A, "Not Run")])
        finally:
            os.unlink(register)
        self.assertEqual(1, code, out)
        self.assertIn("TEST-NOT-RUN", out)
        self.assertNotIn("RED-WITH-REGISTER", out)
        self.assertNotIn("REGISTER-STALE", out)
        self.assertNotIn("GATE PASS", out)

    def test_parsers_are_pure_functions(self):
        listing = ("Test project p\n  Test #10: PortMaxLoopGuard\n"
                   "  Test #614: PolicyTests.KnownRed_X\n\nTotal Tests: 2\n")
        self.assertEqual(["PortMaxLoopGuard", "PolicyTests.KnownRed_X"],
                         RUNNER.parse_discovered(listing))
        self.assertEqual([("PolicyTests.KnownRed_X", "Failed")],
                         RUNNER.parse_failures("\t614 - PolicyTests.KnownRed_X (Failed)\n"))
        self.assertEqual(24, RUNNER.parse_total("0% tests passed, 2 tests failed out of 24"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
