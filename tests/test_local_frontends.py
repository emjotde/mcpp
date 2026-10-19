from pathlib import Path
import os
import subprocess
import sys
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click, menu, save


class LocalFrontends(unittest.TestCase):
    def fixture(self):
        root = ROOT / self._testMethodName
        root.mkdir(parents=True)
        return root

    def test_cli_identity_and_argument_errors(self):
        for binary, count in ((VIEWER, 1), (DIFF, 2)):
            name = Path(binary).name
            for option in ("--help", "-h", "--version", "-V"):
                result = subprocess.run([binary, option], capture_output=True, text=True, check=True)
                self.assertIn(name + " ", result.stdout)
            for arguments in ([], ["--unknown"], [""] * count, ["missing"] * (count + 1)):
                result = subprocess.run([binary, *arguments], capture_output=True, text=True)
                self.assertEqual(result.returncode, 1)
                self.assertIn(name + ": ", result.stderr)

    def test_viewer_starts_directly_and_exits_without_panels(self):
        root = self.fixture()
        path = root / "document.txt"
        path.write_bytes(b"standalone viewer\nsecond line\n")
        with TerminalSession(VIEWER, TMUX, root / "ui", [path], record_exit=True) as terminal:
            terminal.wait_for("standalone viewer")
            self.assertIn("4Hex", terminal.text())
            self.assertNotIn("Left     File     Command", terminal.text())
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_viewer_dash_filename_and_staged_quit_cancellation(self):
        root = self.fixture()
        path = root / "ui/-draft"
        path.parent.mkdir()
        path.write_bytes(b"original\n")
        with TerminalSession(VIEWER, TMUX, root / "ui", ["--", "-draft"], record_exit=True) as terminal:
            terminal.wait_for("original")
            terminal.press("F4", "F2")
            terminal.wait_for("6Save")
            terminal.type("41")
            terminal.press("F10")
            terminal.wait_for("Discard")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Discard", present=False)
            self.assertEqual(path.read_bytes(), b"original\n")
            terminal.press("F10")
            terminal.wait_for("Discard")
            click(terminal, "[ Discard ]")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(path.read_bytes(), b"original\n")

    def test_diff_merge_save_and_standalone_exit(self):
        root = self.fixture()
        first, second = root / "left", root / "right"
        first.write_bytes(b"left contents\n")
        second.write_bytes(b"right contents\n")
        with TerminalSession(DIFF, TMUX, root / "ui", [first, second], record_exit=True) as terminal:
            terminal.wait_for("9Options")
            terminal.wait_for("Dif 1")
            terminal.press("F5")
            terminal.wait_for("Dif 0")
            self.assertEqual(second.read_bytes(), b"right contents\n")
            terminal.press("F2")
            terminal.wait_file(second, first.read_bytes())
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_diff_dirty_cancel_then_discard_exit(self):
        root = self.fixture()
        first, second = root / "left", root / "right"
        first.write_bytes(b"LEFT\n")
        second.write_bytes(b"RIGHT\n")
        with TerminalSession(DIFF, TMUX, root / "ui", [first, second], record_exit=True) as terminal:
            terminal.wait_for("9Options")
            terminal.press("F5", "F10")
            terminal.wait_for("Save with exit?")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Save with exit?", present=False)
            terminal.wait_for("9Options")
            terminal.press("F10")
            terminal.wait_for("Save with exit?")
            click(terminal, "[ No ]")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(second.read_bytes(), b"RIGHT\n")

    def test_diff_editor_returns_to_diff_before_process_exit(self):
        root = self.fixture()
        first, second = root / "left", root / "right"
        first.write_bytes(b"LEFT\n")
        second.write_bytes(b"RIGHT\n")
        with TerminalSession(DIFF, TMUX, root / "ui", [first, second], record_exit=True) as terminal:
            terminal.wait_for("9Options")
            terminal.press("F4")
            terminal.wait_editor(offset=0)
            terminal.type("X")
            save(terminal, first, b"XLEFT\n")
            terminal.press("F10")
            terminal.wait_for("9Options")
            self.assertFalse(terminal.exit_file.exists())
            self.assertIn("XLEFT", terminal.text())
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_manager_edit_accepts_multiple_files_and_process_relative_names(self):
        root = self.fixture()
        ui = root / "ui"
        ui.mkdir()
        files = [ui / name for name in ("first", "second", "third")]
        for path in files:
            path.write_bytes(b"first\nsecond\nthird\n")
        other = root / "saved-pane"
        other.mkdir()
        config = ui / "config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text(f"auto_save_panels_setup=true\nsaved_left_path={other}\nsaved_right_path={other}\n")
        with TerminalSession(MANAGER, TMUX, ui,
                             ["--edit", "+2", "first:3", "second:2:", "third"], record_exit=True) as terminal:
            terminal.wait_editor(row=2, column=1)
            self.assertIn("first", terminal.text().splitlines()[0])
            menu(terminal, "Window", "Next")
            terminal.wait_editor(row=2, column=1)
            self.assertIn("second", terminal.text().splitlines()[0])
            menu(terminal, "Window", "Next")
            terminal.wait_editor(row=1, column=1)
            self.assertIn("third", terminal.text().splitlines()[0])
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_manager_edit_without_a_file_opens_an_unnamed_buffer(self):
        root = self.fixture()
        other = root / "saved-pane"
        other.mkdir()
        config = root / "ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text(f"auto_save_panels_setup=true\nsaved_left_path={other}\nsaved_right_path={other}\n")
        saved = root / "ui/unnamed.txt"
        with TerminalSession(MANAGER, TMUX, root / "ui", ["--edit"], record_exit=True) as terminal:
            terminal.wait_editor(bytes=0, dirty=False)
            terminal.type("process directory")
            terminal.press("F10")
            terminal.wait_for("Save before close?")
            terminal.type("y")
            terminal.wait_for("Save as")
            terminal.type(saved.name)
            terminal.press("Enter")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(saved.read_bytes(), b"process directory")
        self.assertFalse((other / saved.name).exists())


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Local frontend checks require the owned non-root disposable container")
    MANAGER, VIEWER, DIFF = (str(Path(value).resolve()) for value in sys.argv[1:4])
    TMUX = sys.argv[4]
    ROOT = Path(sys.argv[5]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[6:]], verbosity=2)
