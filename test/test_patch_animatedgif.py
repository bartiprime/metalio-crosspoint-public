import importlib.util
from pathlib import Path
import shutil
import unittest

from scratch_dir import SCRATCH_ROOT

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "patch_animatedgif.py"
SPEC = importlib.util.spec_from_file_location("patch_animatedgif", SCRIPT)
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class Environment:
    def __init__(self, root, name="active"):
        self.root = root
        self.name = name

    def subst(self, value):
        return {"$PROJECT_LIBDEPS_DIR": str(self.root), "$PIOENV": self.name}[value]


class PatchAnimatedGifTest(unittest.TestCase):
    def setUp(self):
        # r165.5: scratch inside the repo, one directory per test method (parallel-safe), never %TEMP%.
        self.root = SCRATCH_ROOT / "patch_animatedgif" / self._testMethodName
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir(parents=True, exist_ok=True)
        self.addCleanup(shutil.rmtree, self.root, True)

    def library(self, name="active"):
        library = self.root / name / "AnimatedGIF"
        source = library / "src"
        source.mkdir(parents=True)
        (source / "AnimatedGIF.h").write_text(PATCH.OLD_SNIPPET, newline="\n")
        (source / "gif.inl").write_text(PATCH.PALETTE_OLD + PATCH.COPY_OLD, newline="\n")
        return library

    def test_active_environment_and_idempotence(self):
        active = self.library()
        sibling = self.library("sibling")
        env = Environment(self.root)
        self.assertTrue(PATCH.patch_animatedgif(env, require_found=True))
        first = {p.name: p.read_bytes() for p in (active / "src").iterdir()}
        self.assertTrue(PATCH.patch_animatedgif(env, require_found=True))
        self.assertEqual(first, {p.name: p.read_bytes() for p in (active / "src").iterdir()})
        self.assertEqual((sibling / "src" / "AnimatedGIF.h").read_text(), PATCH.OLD_SNIPPET)
        self.assertEqual((sibling / "src" / "gif.inl").read_text(), PATCH.PALETTE_OLD + PATCH.COPY_OLD)
        # The patcher writes via text mode, so on Windows the file comes back
        # CRLF while its snippets are LF; compare content, not line endings.
        patched = chr(10).join(first["gif.inl"].decode().splitlines()) + chr(10)
        self.assertIn(PATCH.PALETTE_NEW, patched)
        self.assertIn(PATCH.COPY_NEW, patched)

    def test_unknown_decoder_source_fails_without_rewriting_it(self):
        source = self.library() / "src" / "gif.inl"
        source.write_text("unknown upstream implementation", newline="\n")
        with self.assertRaises(RuntimeError):
            PATCH._patch_decoder(source)
        self.assertEqual(source.read_text(), "unknown upstream implementation")

    def test_missing_active_dependency_cannot_be_satisfied_by_sibling(self):
        self.library("sibling")
        self.assertFalse(PATCH.patch_animatedgif(Environment(self.root)))
        with self.assertRaises(SystemExit):
            PATCH.patch_animatedgif(Environment(self.root), require_found=True)


if __name__ == "__main__":
    unittest.main()
