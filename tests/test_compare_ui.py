import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import compare_ui  # noqa: E402


class CompareUiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.demo = self.root / "demo"
        self.app = self.root / "app"
        self.demo.mkdir()
        self.app.mkdir()
        for index, view in enumerate(compare_ui.VIEWS):
            image = Image.new("RGB", (80, 60), (16, 24, 32))
            ImageDraw.Draw(image).rectangle((index * 8, 2, index * 8 + 15, 40), fill=(180, 70, 20))
            for prefix, folder in (("demo", self.demo), ("screenshot", self.app)):
                image.save(folder / f"{prefix}-{view}.png")

    def run_check(self, *extra):
        args = ["compare_ui.py", "--demo", str(self.demo), "--app", str(self.app), "--out",
                str(self.root / "pairs"), "--width", "80", "--height", "60", "--check", *extra]
        output = io.StringIO()
        with patch.object(sys, "argv", args), contextlib.redirect_stdout(output):
            status = compare_ui.main()
        return status, output.getvalue()

    def test_good_pairs(self):
        status, text = self.run_check("--max-score", "0")
        self.assertEqual(status, 0, text)
        self.assertEqual(len(list((self.root / "pairs").glob("pair-*.png"))), 6)

    def test_missing_pair_fails(self):
        (self.app / "screenshot-system.png").unlink()
        status, text = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("system: нет пары", text)

    def test_duplicate_view_fails(self):
        (self.demo / "demo-overlay.png").write_bytes((self.demo / "demo-system.png").read_bytes())
        status, text = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("повторён кадр", text)

    def test_blank_view_fails(self):
        Image.new("RGB", (80, 60), "black").save(self.app / "screenshot-settings.png")
        status, text = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("однородное", text)

    def test_visual_threshold_fails(self):
        changed = self.app / "screenshot-settings.png"
        with Image.open(changed) as image:
            inverted = Image.eval(image, lambda value: 255 - value)
            inverted.save(changed)
        status, text = self.run_check("--max-score", "10")
        self.assertEqual(status, 1)
        self.assertIn("расхождение", text)


if __name__ == "__main__":
    unittest.main()
