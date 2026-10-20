from pathlib import Path
import os
import re
import shlex
import sys
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click, select_file


def screen(terminal, label, kind=""):
    terminal.raw(b"\x1b`")
    terminal.wait_for("Screen list")
    entries = [
        line for line in terminal.text().splitlines()
        if "File manager" in line or re.search(r"\[(?:View|Diff|Edit)\*?\] ", line)
    ]
    index = next(index for index, line in enumerate(entries)
                 if label in line and (not kind or "[" + kind in line))
    terminal.press("Home", *("Down" for _ in range(index)), "Enter")
    terminal.wait_for("Screen list", present=False)


def compare(terminal, first, second):
    select_file(terminal, first)
    terminal.press("Tab")
    select_file(terminal, second)
    terminal.press("Tab", "C-x", "C-d")
    terminal.wait_for("9Options")
    terminal.wait_for("Dif 1")


class IndependentScreensUI(unittest.TestCase):
    def fixture(self):
        root = ROOT / self._testMethodName
        files = root / "files"
        files.mkdir(parents=True)
        return root, files

    def test_viewer_drafts_bookmarks_and_editor_screen(self):
        root, files = self.fixture()
        first, second = files / "a-view", files / "b-view"
        first.write_bytes(b"ABC\n")
        second.write_bytes(b"UVW\n")
        with TerminalSession(MANAGER, TMUX, root / "ui", [files, files],
                             record_exit=True) as t:
            t.wait_for("Name")
            select_file(t, first.name)
            t.press("F3")
            t.wait_for("ABC")
            t.press("F4", "F2")
            t.type("5a")
            t.wait_for("5a 42 43")
            t.press("F2", "Right")
            t.type("3m")
            t.wait_for("M3")
            t.wait_for("@2/4")
            screen(t, "File manager")
            t.wait_for("Name")
            select_file(t, second.name)
            t.press("F3")
            t.wait_for("55 56 57")
            t.press("F2")
            t.type("59")
            t.wait_for("59 56 57")
            t.press("F2")
            t.type("7m")
            t.wait_for("M7")
            t.raw(b"\x1b{")
            t.wait_for("M3")
            t.wait_for("@2/4")
            t.raw(b"\x1b}")
            t.wait_for("M7")
            screen(t, first.name)
            t.wait_for("5a 42 43")
            t.wait_for("M3")
            t.wait_for("@2/4")
            t.press("F1")
            t.wait_for("mcpp help: File viewer")
            t.press("F10")
            t.wait_for("mcpp help:", present=False)
            t.wait_for("5a 42 43")
            t.wait_for("M3")
            t.wait_for("@2/4")
            t.press("Home")
            t.type("r")
            t.wait_for("@0/4")
            self.assertEqual(first.read_bytes(), b"ABC\n")
            t.press("F10")
            t.wait_for("Save modified bytes?")
            click(t, "[ Cancel ]")
            t.wait_for("Save modified bytes?", present=False)
            t.press("F6")
            t.wait_file(first, b"ZBC\n")
            screen(t, second.name)
            t.wait_for("59 56 57")
            t.wait_for("M7")
            t.press("F6")
            t.wait_file(second, b"YVW\n")
            screen(t, "File manager")
            select_file(t, first.name)
            t.press("F4")
            t.wait_editor(dirty=False)
            t.press("Right", "Right")
            t.wait_editor(offset=2)
            screen(t, second.name, "View")
            t.wait_for("M7")
            screen(t, first.name, "Edit")
            t.wait_editor(offset=2, dirty=False)
            t.save("retained-viewers-and-editor")
            t.press("F10")
            t.wait_for("Name")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")

    def test_diff_drafts_linked_save_and_editor_return(self):
        root, files = self.fixture()
        first, second = files / "a-left", files / "a-right"
        third, fourth = files / "b-left", files / "b-right"
        first.write_bytes(b"LEFT_A\n")
        second.write_bytes(b"RIGHT_A\n")
        third.write_bytes(b"LEFT_B\n")
        fourth.write_bytes(b"RIGHT_B\n")
        alias = files / "b-alias"
        os.link(fourth, alias)
        with TerminalSession(MANAGER, TMUX, root / "ui", [files, files],
                             record_exit=True) as t:
            t.wait_for("Name")
            compare(t, first.name, second.name)
            t.press("F5")
            t.wait_for("Dif 0")
            screen(t, "File manager")
            compare(t, third.name, fourth.name)
            t.press("F5")
            t.wait_for("Dif 0")
            t.type("f")
            t.wait(lambda: fourth.name not in t.text().splitlines()[0],
                   "Second comparison keeps its full-width left pane")
            screen(t, second.name, "Diff")
            t.wait_for("Dif 0")
            self.assertIn(second.name, t.text().splitlines()[0])
            t.press("F4")
            t.wait_editor(dirty=False)
            t.type("X")
            t.save_editor()
            t.wait_file(first, b"XLEFT_A\n")
            screen(t, fourth.name, "Diff")
            t.wait_for("LEFT_B")
            screen(t, first.name, "Edit")
            t.wait_editor(dirty=False)
            t.press("F10")
            t.wait_for("9Options")
            t.wait_for("Dif 1")
            self.assertIn(first.name, t.text().splitlines()[0])
            self.assertIn("LEFT_A", t.text().splitlines()[2][50:])
            second.write_bytes(b"EXTERNAL_A\n")
            t.press("F2")
            t.wait_for("Diff save")
            t.press("Enter")
            t.wait_for("Diff save", present=False)
            self.assertEqual(second.read_bytes(), b"EXTERNAL_A\n")
            t.press("F10")
            t.wait_for("Save with exit?")
            click(t, "Cancel")
            t.wait_for("Save with exit?", present=False)
            screen(t, fourth.name, "Diff")
            t.wait_for("LEFT_B")
            self.assertNotIn(fourth.name, t.text().splitlines()[0])
            t.press("F2")
            t.wait_for("File has multiple hardlinks")
            click(t, "Cancel")
            t.wait_for("File has multiple hardlinks", present=False)
            self.assertEqual(alias.read_bytes(), b"RIGHT_B\n")
            t.press("F2")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Preserve links ]")
            t.wait_for("File has multiple hardlinks", present=False)
            t.wait_file(alias, b"LEFT_B\n")
            self.assertEqual(alias.stat().st_ino, fourth.stat().st_ino)
            t.save("retained-diffs-and-editor-return")
            screen(t, "File manager")
            t.press("F10")
            t.wait_for("Save with exit?")
            click(t, "Cancel")
            t.wait_for("Save with exit?", present=False)
            self.assertEqual(second.read_bytes(), b"EXTERNAL_A\n")
            t.press("F10")
            t.wait_for("Save with exit?")
            click(t, "No")
            t.wait_for("Save with exit?", present=False)
            t.wait_for("Name")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")

    def test_streamed_viewer_retains_input_and_final_exit(self):
        root, _ = self.fixture()
        ui = root / "ui"
        ui.mkdir()
        local = ui / "local-view"
        local.write_bytes(b"LOCAL_VIEW\n")
        source = root / "input.fifo"
        os.mkfifo(source)
        producer = os.open(source, os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
        first, second = b"STREAM_ONE\nq\x1b[21~\x03\n", b"STREAM_TWO\n"
        try:
            os.write(producer, first)
            command = "exec " + shlex.join([VIEWER, "/dev/stdin"]) + " < " + shlex.quote(str(source))
            with TerminalSession("/bin/bash", TMUX, ui, ["-c", command],
                                 record_exit=True, record_stderr=True) as t:
                t.wait_for("STREAM_ONE")
                t.wait_for(f"/{len(first)}+")
                screen(t, "File manager")
                t.wait_for("Name")
                select_file(t, local.name)
                t.press("F3")
                t.wait_for("LOCAL_VIEW")
                os.write(producer, second)
                screen(t, "Standard input")
                t.wait_for(f"/{len(first) + len(second)}+")
                t.press("End")
                t.wait_for("STREAM_TWO")
                screen(t, local.name)
                t.wait_for("LOCAL_VIEW")
                t.press("F10")
                t.wait_for("Standard input")
                t.wait_for("STREAM_TWO")
                t.save("retained-stream")
                t.press("F10")
                t.wait_file(t.exit_file, b"0\n")
        finally:
            os.close(producer)
            source.unlink(missing_ok=True)

    def test_standalone_diff_closes_only_selected_session(self):
        root, _ = self.fixture()
        ui = root / "ui"
        ui.mkdir()
        first, second = ui / "a-left", ui / "a-right"
        third, fourth = ui / "b-left", ui / "b-right"
        first.write_bytes(b"FIRST\n")
        second.write_bytes(b"ORIGINAL\n")
        third.write_bytes(b"OTHER_LEFT\n")
        fourth.write_bytes(b"OTHER_RIGHT\n")
        with TerminalSession(DIFF, TMUX, ui, [first, second], record_exit=True) as t:
            t.wait_for("Dif 1")
            t.press("F5")
            t.wait_for("Dif 0")
            screen(t, "File manager")
            t.wait_for("Name")
            compare(t, third.name, fourth.name)
            t.press("F10")
            t.wait_for("Dif 0")
            self.assertIn(first.name, t.text().splitlines()[0])
            self.assertEqual(second.read_bytes(), b"ORIGINAL\n")
            t.press("F10")
            t.wait_for("Save with exit?")
            click(t, "Yes")
            t.wait_file(t.exit_file, b"0\n")
            self.assertEqual(second.read_bytes(), b"FIRST\n")
            self.assertEqual(fourth.read_bytes(), b"OTHER_RIGHT\n")


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() != 1001 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Screen checks require the owned uid1001 disposable container")
    MANAGER, VIEWER, DIFF = (str(Path(argument).resolve()) for argument in sys.argv[1:4])
    TMUX = sys.argv[4]
    parent = Path(sys.argv[5]).resolve()
    if not parent.is_relative_to("/review/cases/independent-screens"):
        raise SystemExit("Use the owned independent-screens case directory")
    ROOT = parent / f"ui-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[6:]], verbosity=2)
