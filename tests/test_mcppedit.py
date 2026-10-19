from pathlib import Path
import os
import subprocess
import sys
import unittest

from terminal_session import TerminalSession
from test_audit_ui import menu, save


class StandaloneEditor(unittest.TestCase):
    def fixture(self):
        root = ROOT / self._testMethodName
        root.mkdir(parents=True)
        return root

    def test_cli_identity_and_invalid_arguments(self):
        for option in ("--help", "--version", "-V"):
            result = subprocess.run([BINARY, option], capture_output=True, text=True, check=True)
            self.assertIn("mcppedit 0.1.0", result.stdout)
        for arguments, message in (([], "file name is required"), (["+0", "file"], "positive integers"),
                                   (["+bad", "file"], "positive integers"), (["--view"], "unknown option"),
                                   ([""], "file name is empty")):
            result = subprocess.run([BINARY, *arguments], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("mcppedit: ", result.stderr)
            self.assertIn(message, result.stderr)

    def test_named_new_file_save_and_editor_only_exit(self):
        root = self.fixture()
        path = root / "new file.txt"
        with TerminalSession(BINARY, TMUX, root / "ui", [path], record_exit=True) as terminal:
            terminal.wait_editor(offset=0, dirty=False)
            self.assertFalse(path.exists())
            self.assertNotIn("Left     File     Command", terminal.text())
            terminal.type("standalone")
            save(terminal, path, b"standalone")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(path.read_bytes(), b"standalone")

    def test_multiple_files_line_position_and_duplicate_names(self):
        root = self.fixture()
        paths = [root / "first.txt", root / "second.txt", root / "third.txt"]
        for index, path in enumerate(paths):
            path.write_text(f"file {index}\nsecond\nthird\n")
        with TerminalSession(BINARY, TMUX, root / "ui", ["+3", *paths, paths[0]], record_exit=True) as terminal:
            terminal.wait_editor(row=3, column=1)
            self.assertIn("first.txt", terminal.text().splitlines()[0])
            menu(terminal, "Window", "Next")
            terminal.wait(lambda: "second.txt" in terminal.text().splitlines()[0], "CLI order retains the second buffer")
            menu(terminal, "Window", "Next")
            terminal.wait(lambda: "third.txt" in terminal.text().splitlines()[0], "CLI order retains the third buffer")
            menu(terminal, "Window", "Next")
            terminal.wait(lambda: "first.txt" in terminal.text().splitlines()[0], "Duplicate file arguments do not create duplicate buffers")
            terminal.wait_editor(row=3, column=1)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_end_of_options_and_dirty_close(self):
        root = self.fixture()
        path = root / "ui/-draft.txt"
        path.parent.mkdir()
        path.write_text("original\n")
        with TerminalSession(BINARY, TMUX, root / "ui", ["--", "-draft.txt"], record_exit=True) as terminal:
            terminal.wait_editor(offset=0)
            terminal.type("X")
            terminal.press("F10")
            terminal.wait_for("Save before close?")
            terminal.type("c")
            terminal.wait_editor(dirty=True)
            terminal.press("F10")
            terminal.wait_for("Save before close?")
            terminal.type("n")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(path.read_text(), "original\n")


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Standalone editor checks require the owned non-root disposable container")
    BINARY, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    ROOT = Path(sys.argv[3]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
