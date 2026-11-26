import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "audit_r2_sound_assets.py"
SPEC = importlib.util.spec_from_file_location("audit_r2_sound_assets", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(audit)


class SoundAssetAuditTests(unittest.TestCase):
    def test_inventory_is_read_only_and_reports_kind_and_state_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = [
                "R2_Beep_Curious.MP3",
                "R2_Beep_Default.mp3",
                "R2_Beep_Acknowledgement.mp3",
                "R2_Whistle_Happy.mp3",
                "R2_Whistle_01.mp3",
                "not_a_sound.wav",
            ]
            for name in names:
                (root / name).write_bytes(b"fixture-not-a-real-mp3")
            (root / "R2_Beep_Sleepy.mp3").mkdir()
            before = {p.name: (p.is_dir(), p.read_bytes() if p.is_file() else None)
                      for p in root.iterdir()}

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(audit.audit_root(root), 0)

            report = output.getvalue()
            self.assertIn("Readable regular MP3 files: 5", report)
            self.assertIn("R2_Beep_Curious.MP3 [curious]", report)
            self.assertIn("Beep candidates: 3", report)
            self.assertIn("Whistle candidates: 2", report)
            self.assertIn("State curious: exact=beep", report)
            self.assertIn("State greeting: exact=beep", report)
            self.assertIn("State happy: exact=whistle; generic fallback=beep", report)
            self.assertIn("State hungry: generic fallback=beep; unlabelled fallback=whistle", report)
            self.assertIn("does not decode MP3s", report)
            after = {p.name: (p.is_dir(), p.read_bytes() if p.is_file() else None)
                     for p in root.iterdir()}
            self.assertEqual(before, after)

    def test_conflicting_state_label_is_not_reported_as_generic_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "R2_Beep_Sleepy_Default.mp3").write_bytes(b"fixture-not-a-real-mp3")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(audit.audit_root(root), 0)
            report = output.getvalue()
            self.assertIn("State sleepy: exact=beep", report)
            self.assertIn("State curious: no named match", report)
            self.assertIn("State happy: no named match", report)

    def test_custom_state_names_do_not_count_as_unrelated_fallbacks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("R2_Beep_Nostalgic_Default.mp3",
                         "R2_Whistle_Reflective.mp3"):
                (root / name).write_bytes(b"fixture-not-a-real-mp3")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(audit.audit_root(root), 0)
            report = output.getvalue()
            self.assertIn("R2_Beep_Nostalgic_Default.mp3 [default, nostalgic]", report)
            self.assertIn("R2_Whistle_Reflective.mp3 [reflective]", report)
            self.assertIn("State curious: no named match", report)
            self.assertIn("State happy: no named match", report)

            (root / "R2_Beep_01.mp3").write_bytes(b"fixture-not-a-real-mp3")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(audit.audit_root(root), 0)
            self.assertIn("State happy: unlabelled fallback=beep", output.getvalue())

    def test_tokenizer_retains_runtime_bounds(self):
        self.assertEqual(audit.tokens("a" * 70 + ".MP3"), {"a" * 63, "mp3"})
        self.assertNotIn("last", audit.tokens("_".join(str(i) for i in range(64)) + "_last"))
        self.assertEqual(audit.state_labels({"r2", "beep", "01", "mp3"}, "beep"), set())
        self.assertEqual(audit.state_labels({"beep", "whistle"}, "beep"), {"whistle"})

    def test_missing_directory_is_reported_without_creating_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "missing"
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(audit.audit_root(root), 2)
            self.assertIn("missing or unreadable", output.getvalue())
            self.assertFalse(root.exists())


if __name__ == "__main__":
    unittest.main()
