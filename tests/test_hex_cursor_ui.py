from pathlib import Path
import os
import sys
import unittest

from terminal_session import TerminalSession
from test_audit_ui import mouse
from test_skins_ui import cells


class HexCursorUI(unittest.TestCase):
    def assert_pair(self, terminal, row, byte, ascii_active, editing=False, low_nibble=False):
        line = terminal.text().splitlines()[row]
        prefix = line.index("  ") + 2
        hexadecimal = prefix + byte * 3
        plain = prefix + 49 + byte
        rendered = cells(terminal)[row]
        active = plain if ascii_active else hexadecimal + int(low_nibble)
        counterpart = hexadecimal if ascii_active else plain
        self.assertEqual(rendered[active][1:], rendered[counterpart][1:])
        self.assertNotEqual(rendered[active][2], rendered[hexadecimal - 1][2])
        spans = terminal.styled_rows()[row]["spans"]
        self.assertTrue(any(begin <= active < end for begin, end in spans))
        self.assertFalse(any(begin <= counterpart < end for begin, end in spans))
        if ascii_active or not editing:
            self.assertEqual(rendered[hexadecimal][1:], rendered[hexadecimal + 1][1:])

    def test_keyboard_mouse_and_nibble_keep_both_columns_highlighted(self):
        root = ROOT / self._testMethodName
        root.mkdir(parents=True)
        path = root / "bytes.bin"
        original = bytes(range(64, 96)) + b"\x00\x01\x02\xff"
        path.write_bytes(original)
        with TerminalSession(BINARY, TMUX, root / "ui", ["--view", path],
                             width=80, height=24, record_exit=True) as terminal:
            terminal.wait_for("bytes.bin")
            terminal.press("F4", "Right", "Right", "Right")
            terminal.wait_for("@3/36")
            terminal.wait(lambda: len(terminal.styled_rows()[1]["spans"]) == 1, "Hex cursor is rendered")
            self.assert_pair(terminal, 1, 3, False)
            terminal.press("Tab")
            terminal.wait(lambda: terminal.styled_rows()[1]["focused"] == "C", "ASCII input column is active")
            self.assert_pair(terminal, 1, 3, True)
            terminal.press("Right")
            terminal.wait_for("@4/36")
            self.assert_pair(terminal, 1, 4, True)
            prefix = terminal.text().splitlines()[1].index("  ") + 2
            mouse(terminal, prefix + 6 * 3 + 1, 1)
            mouse(terminal, prefix + 6 * 3 + 1, 1, motion="m")
            terminal.wait_for("@6/36")
            terminal.press("F2")
            terminal.wait_for("2View")
            mouse(terminal, prefix + 6 * 3 + 1, 1)
            mouse(terminal, prefix + 6 * 3 + 1, 1, motion="m")
            terminal.wait(lambda: terminal.styled_rows()[1]["focused"] == "6", "Low nibble receives input")
            self.assert_pair(terminal, 1, 6, False, editing=True, low_nibble=True)
            mouse(terminal, prefix + 49 + 9, 1)
            mouse(terminal, prefix + 49 + 9, 1, motion="m")
            terminal.wait_for("@9/36")
            self.assert_pair(terminal, 1, 9, True, editing=True)
            terminal.raw(b"\x1b[1;5F")
            terminal.wait_for("@35/36")
            self.assert_pair(terminal, 3, 3, True, editing=True)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(path.read_bytes(), original)

    def test_counterpart_survives_horizontal_crop(self):
        root = ROOT / self._testMethodName
        root.mkdir(parents=True)
        path = root / "narrow.bin"
        original = b"ABCDEFGHIJKLMNOP"
        path.write_bytes(original)
        with TerminalSession(BINARY, TMUX, root / "ui", ["--view", path],
                             width=40, height=12, record_exit=True) as terminal:
            terminal.wait_for("narrow.bin")
            terminal.press("F4", *(["Right"] * 15), "Tab")
            terminal.wait(lambda: terminal.styled_rows()[1]["focused"] == "P", "ASCII cursor scrolls into view")
            line = terminal.text().splitlines()[1]
            hexadecimal, plain = line.index("50"), line.rindex("P")
            rendered = cells(terminal)[1]
            self.assertEqual(rendered[hexadecimal][1:], rendered[plain][1:])
            mouse(terminal, hexadecimal, 1)
            mouse(terminal, hexadecimal, 1, motion="m")
            terminal.wait(lambda: terminal.styled_rows()[1]["focused"] == "50", "Mouse returns focus to hex byte")
            rendered = cells(terminal)[1]
            self.assertEqual(rendered[hexadecimal][1:], rendered[plain][1:])
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(path.read_bytes(), original)


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Hex cursor checks require the owned non-root disposable container")
    BINARY = str(Path(sys.argv[1]).resolve())
    TMUX = sys.argv[2]
    ROOT = Path(sys.argv[3]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
