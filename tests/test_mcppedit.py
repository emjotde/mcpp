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
        for arguments, message in ((["+bad", "file"], "nonnegative integers"), (["--view"], "unknown option"),
                                   ([""], "file name is empty")):
            result = subprocess.run([BINARY, *arguments], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("mcppedit: ", result.stderr)
            self.assertIn(message, result.stderr)

    def test_unnamed_startup_and_explicit_save_as(self):
        root = self.fixture()
        path = root / "ui/named draft.txt"
        with TerminalSession(BINARY, TMUX, root / "ui", [], record_exit=True) as terminal:
            terminal.wait_editor(bytes=0, dirty=False)
            terminal.type("unnamed draft")
            terminal.press("F2")
            terminal.wait_for("Save file")
            terminal.press("Enter")
            terminal.wait_for("Save as")
            terminal.press("Enter")
            terminal.wait_for("file name is empty")
            self.assertFalse(path.exists())
            terminal.type(path.name)
            terminal.press("Enter")
            terminal.wait_file(path, b"unnamed draft")
            terminal.wait_editor(dirty=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertFalse((root / "ui/new-file").exists())

    def test_unnamed_quit_save_as_cancel_and_retry(self):
        root = self.fixture()
        path = root / "ui/saved-on-quit.txt"
        with TerminalSession(BINARY, TMUX, root / "ui", [], record_exit=True) as terminal:
            terminal.wait_editor(bytes=0)
            terminal.type("retained draft")
            terminal.press("F10")
            terminal.wait_for("Save before close?")
            terminal.type("y")
            terminal.wait_for("Save as")
            terminal.press("Escape")
            terminal.wait_for("Save as", present=False)
            terminal.wait_editor(dirty=True)
            terminal.press("F10")
            terminal.wait_for("Save before close?")
            terminal.type("y")
            terminal.wait_for("Save as")
            terminal.type(path.name)
            terminal.press("Enter")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(path.read_bytes(), b"retained draft")

    def test_quit_saves_the_inactive_unnamed_buffer_not_the_current_file(self):
        root = self.fixture()
        named = root / "existing.txt"
        named.write_bytes(b"existing unchanged\n")
        saved = root / "ui/inactive-draft.txt"
        with TerminalSession(BINARY, TMUX, root / "ui", [], record_exit=True) as terminal:
            terminal.wait_editor(bytes=0)
            terminal.type("inactive draft")
            menu(terminal, "File", "Open file...")
            terminal.wait_for("Open file")
            terminal.type(str(named))
            terminal.press("Enter")
            terminal.wait_editor(bytes=len(named.read_bytes()), dirty=False)
            terminal.press("F10")
            terminal.wait_for("Save before close?")
            terminal.type("y")
            terminal.wait_for("Save as")
            terminal.type(saved.name)
            terminal.press("Enter")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(saved.read_bytes(), b"inactive draft")
        self.assertEqual(named.read_bytes(), b"existing unchanged\n")

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

    def test_per_file_positions_and_existing_colon_filename(self):
        root = self.fixture()
        first, second, literal = root / "first", root / "second", root / "literal:3"
        for path in (first, second, literal):
            path.write_bytes(b"first\nsecond\nthird\nfourth\n")
        with TerminalSession(BINARY, TMUX, root / "ui",
                             [str(first) + ":3", str(second) + ":2:", literal], record_exit=True) as terminal:
            terminal.wait_editor(row=3, column=1)
            menu(terminal, "Window", "Next")
            terminal.wait_editor(row=2, column=1)
            self.assertIn("second", terminal.text().splitlines()[0])
            menu(terminal, "Window", "Next")
            terminal.wait_editor(row=1, column=1)
            self.assertIn("literal:3", terminal.text().splitlines()[0])
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_long_filename_header_keeps_the_complete_character_readout(self):
        root = self.fixture()
        directory = root / ("long-directory-" + "x" * 70)
        directory.mkdir()
        path = directory / "distinct-document.txt"
        path.write_bytes(b"first line\n")
        with TerminalSession(BINARY, TMUX, root / "ui", [path], record_exit=True) as terminal:
            terminal.wait_editor(offset=0)
            terminal.press("End")
            terminal.wait_for("0010 U+000A")
            header = terminal.text().splitlines()[0]
            self.assertIn("distinct-document.txt", header)
            self.assertLessEqual(len(header), 100)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_new_unnamed_buffer_keeps_its_file_and_command_base_directory(self):
        root = self.fixture()
        documents = root / "documents"
        documents.mkdir()
        original = documents / "original.txt"
        original.write_bytes(b"unchanged\n")
        saved = documents / "new.txt"
        expected = (str(documents) + "\n").encode()
        with TerminalSession(BINARY, TMUX, root / "ui", [original], record_exit=True) as terminal:
            terminal.wait_editor(offset=0)
            terminal.press("C-n")
            terminal.wait_editor(bytes=0, dirty=False)
            menu(terminal, "Format", "Paste output of...")
            terminal.wait_for("Enter shell command(s):")
            terminal.type("pwd")
            terminal.press("Enter")
            terminal.wait_editor(bytes=len(expected), dirty=True)
            terminal.press("F2")
            terminal.wait_for("Save file")
            terminal.press("Enter")
            terminal.wait_for("Save as")
            terminal.type(saved.name)
            terminal.press("Enter")
            terminal.wait_file(saved, expected)
            terminal.wait_editor(dirty=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(original.read_bytes(), b"unchanged\n")
        self.assertFalse((root / "ui/new.txt").exists())

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
