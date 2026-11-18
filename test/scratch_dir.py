"""Build scratch directories INSIDE the working tree, never under %TEMP%.

The operator's standing rule: a test run may not write outside the working directory. The host
harnesses in this directory generate C++ sources, compile them and drop executables, and they used
to do it in `tempfile.TemporaryDirectory()` -- i.e. under %LOCALAPPDATA%\\Temp -- so a suite run
scattered artifacts outside the repo and a failing build's generated .cpp was thrown away exactly
when it was needed for the post-mortem.

    from scratch_dir import scratch_dir
    with scratch_dir("my_harness") as directory:
        ...

`name` must be unique per test: parallel ctest (`-j`) runs several at once and each wipes its own
directory on entry. Everything lands in <repo>/_scratch/<name>/ (git-ignored) and is removed on exit;
pass keep=True when the artifacts are the evidence.
"""

from contextlib import contextmanager
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
SCRATCH_ROOT = ROOT / "_scratch"


@contextmanager
def scratch_dir(name, keep=False):
    path = SCRATCH_ROOT / name
    shutil.rmtree(path, ignore_errors=True)  # a stale artefact must never look like this run's
    path.mkdir(parents=True, exist_ok=True)
    try:
        yield path
    finally:
        if not keep:
            shutil.rmtree(path, ignore_errors=True)
