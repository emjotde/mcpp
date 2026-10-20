from pathlib import Path
import os
import stat
import sys
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click, select_file


def histories(path):
    lines = path.read_text().splitlines()
    assert lines.pop(0) == "mcpp-input-history-v1"
    result = {}
    for line in lines:
        name, value = line.split("\t")
        result.setdefault(name, []).append(bytes.fromhex(value).decode())
    return result


class InputHistoryUI(unittest.TestCase):
    def fixture(self, extra=""):
        root = ROOT / self._testMethodName
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        profile = root / "ui"
        config = profile / "config/mcpp"
        config.mkdir(parents=True)
        (config / "config").write_text(
            "# keep unrelated preferences\n"
            "auto_save_setup=false\nuse_subshell=false\nconfirm_history_cleanup=true\n" + extra
        )
        return root, left, right, profile, config

    def test_restart_purposes_draft_cursor_and_cleanup(self):
        root, left, right, profile, config = self.fixture()
        (left / "source.txt").write_text("tiny\n")
        (left / "child").mkdir()
        destination = right / "copied.txt"
        original_config = (config / "config").read_bytes()
        with TerminalSession(BINARY, TMUX, profile, [left, right], record_exit=True) as t:
            t.wait_for("Name")
            t.press("F7")
            t.wait_for("New directory")
            t.press("C-u")
            t.type("created-dir")
            t.press("Enter")
            t.wait_for("New directory", present=False)
            self.assertTrue((left / "created-dir").is_dir())
            select_file(t, "source.txt")
            t.press("F5")
            t.wait_for("Source mask:")
            t.press("C-u")
            t.type(str(destination))
            t.press("Enter")
            t.wait_for("Source mask:", present=False)
            t.wait_file(destination, b"tiny\n")
            t.wait_for("Copy complete")
            t.press("M-c")
            t.wait_for("Change directory")
            t.press("C-u")
            t.type("child")
            t.press("Enter")
            t.wait_for("Change directory", present=False)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        history = config / "input-history"
        self.assertEqual(stat.S_IMODE(history.stat().st_mode), 0o600)
        self.assertEqual((config / "config").read_bytes(), original_config)
        with TerminalSession(BINARY, TMUX, profile, [left, right], record_exit=True) as t:
            t.wait_for("Name")
            t.press("F7")
            t.wait_for("New directory")
            t.press("C-u")
            t.type("draft")
            t.press("Home", "Right", "M-p")
            t.wait_for("> created-dir")
            t.press("M-n")
            t.wait_for("> draft")
            t.type("X")
            t.wait_for("> dXraft")
            t.press("M-h")
            t.wait_for("History")
            t.press("Escape")
            t.wait_for("History", present=False)
            t.type("Y")
            t.wait_for("> dXYraft")
            t.press("M-h")
            t.wait_for("History")
            t.type("D")
            t.wait_for("Do you want clean this history?")
            t.press("Right", "Enter")
            t.wait_for("Do you want clean this history?", present=False)
            self.assertIn("directory.mkdir", histories(history))
            t.type("D")
            t.wait_for("Do you want clean this history?")
            t.press("Enter")
            t.wait_for("Do you want clean this history?", present=False)
            t.wait_for("<empty>")
            t.press("Escape")
            t.wait_for("History", present=False)
            t.wait_for("> dXYraft")
            t.save("draft-and-cleanup")
            t.press("Escape")
            t.wait_for("New directory", present=False)
            t.press("M-c")
            t.wait_for("Change directory")
            t.press("C-u", "M-p")
            t.wait_for("> child")
            t.press("Escape")
            t.wait_for("Change directory", present=False)
            select_file(t, "source.txt")
            t.press("F5")
            t.wait_for("Source mask:")
            t.press("C-u", "M-p")
            t.wait_for("right/copied.txt")
            t.press("Escape")
            t.wait_for("Source mask:", present=False)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        saved = histories(history)
        self.assertNotIn("directory.mkdir", saved)
        self.assertEqual(saved["directory.change"], ["child"])
        self.assertEqual(saved["file.copy-destination"], [str(destination)])
        self.assertEqual((config / "config").read_bytes(), original_config)

    def test_native_failure_and_editor_search_replace_goto(self):
        root, left, right, profile, config = self.fixture("ask_new_filename=true\n")
        document = left / "text.txt"
        document.write_text("needle\nneedle\nlast\n")
        original = document.read_bytes()
        with TerminalSession(BINARY, TMUX, profile, [left, right], record_exit=True) as t:
            t.wait_for("Name")
            select_file(t, "text.txt")
            t.press("F4")
            t.wait_for("Enter file name:")
            t.press("C-u")
            t.type("text.txt")
            t.press("Home", "Right")
            config.chmod(0o500)
            try:
                t.press("Enter")
                t.wait_for("input history")
                self.assertIn("Enter file name:", t.text())
                self.assertFalse((config / "input-history").exists())
                t.type("X")
                t.wait_for("tXext.txt")
                t.press("BSpace")
                t.wait_for("text.txt")
                t.save("failed-native-input-keeps-cursor")
            finally:
                config.chmod(0o700)
            t.press("Enter")
            t.wait_for("Enter file name:", present=False)
            t.wait_editor(dirty=False)
            t.press("F7")
            t.wait_for("Search for:")
            t.type("needle")
            t.press("Enter")
            t.wait_for("Search for:", present=False)
            t.press("C-Home", "F4")
            t.wait_for("Replace with:")
            t.press("Enter")
            t.type("replacement")
            t.press("Enter")
            t.wait_for("[ Replace ]")
            click(t, "[ Cancel ]")
            t.wait_for("[ Replace ]", present=False)
            t.press("M-l")
            t.wait_for("Go to line")
            t.press("C-u")
            t.type("2")
            t.press("Enter")
            t.wait_for("Go to line", present=False)
            t.press("F10")
            t.wait_for("Name")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        saved = histories(config / "input-history")
        self.assertEqual(saved["filename.edit"], ["text.txt"])
        self.assertEqual(saved["search.editor"], ["needle"])
        self.assertEqual(saved["replace.editor"], ["replacement"])
        self.assertEqual(saved["goto.editor"], ["2"])
        with TerminalSession(BINARY, TMUX, profile, [left, right], record_exit=True) as t:
            t.wait_for("Name")
            select_file(t, "text.txt")
            t.press("F4")
            t.wait_for("Enter file name:")
            t.press("C-u", "M-p")
            t.wait_for("text.txt")
            t.press("Enter")
            t.wait_for("Enter file name:", present=False)
            t.press("F4")
            t.wait_for("Replace with:")
            t.press("M-p", "Tab", "M-p")
            t.wait_for("> needle")
            t.wait_for("> replacement")
            t.press("Escape")
            t.wait_for("Replace with:", present=False)
            t.press("M-l")
            t.wait_for("Go to line")
            t.press("C-u", "M-p")
            t.wait_for("> 2")
            t.press("Escape")
            t.wait_for("Go to line", present=False)
            t.press("F10")
            t.wait_for("Name")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual(document.read_bytes(), original)


if __name__ == "__main__":
    if os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Input history UI checks require the owned uid1001 disposable container")
    BINARY, TMUX = sys.argv[1:3]
    base = Path(sys.argv[3]).resolve()
    if base != Path("/review/cases/input-histories"):
        raise SystemExit("Expected /review/cases/input-histories")
    ROOT = base / f"ui-{os.getpid()}"
    ROOT.mkdir()
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
