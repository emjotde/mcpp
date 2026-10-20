from pathlib import Path
import os
import subprocess
import sys
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click


class KeymapUI(unittest.TestCase):
    def fixture(self, resource, config=""):
        root = ROOT / self._testMethodName
        root.mkdir(parents=True)
        ui = root / "ui"
        directory = ui / "config/mcpp"
        directory.mkdir(parents=True)
        (directory / "mcpp.keymap").write_text(resource)
        (directory / "config").write_text("editor_confirm_save=false\n" + config)
        return root, ui

    def test_editor_save_quit_and_removed_defaults(self):
        root, ui = self.fixture("[editor]\nSave=ctrl-s\nQuit=alt-q\n")
        path = root / "document"
        path.write_bytes(b"original\n")
        with TerminalSession(EDITOR, TMUX, ui, [path], record_exit=True) as terminal:
            terminal.wait_editor(dirty=False)
            terminal.type("X")
            terminal.press("F2")
            terminal.type("Y")
            terminal.wait_editor(bytes=11, dirty=True)
            self.assertEqual(path.read_bytes(), b"original\n")
            terminal.press("C-s")
            terminal.wait_file(path, b"XYoriginal\n")
            terminal.press("F10")
            terminal.type("Z")
            terminal.wait_editor(bytes=12, dirty=True)
            self.assertFalse(terminal.exit_file.exists())
            terminal.press("M-q")
            terminal.wait_for("Save before close?")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Save before close?", present=False)
            terminal.press("M-q")
            terminal.wait_for("Save before close?")
            click(terminal, "[ No ]")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(path.read_bytes(), b"XYoriginal\n")

    def test_manager_panel_and_extended_contexts(self):
        root, ui = self.fixture(
            "[filemanager]\nView=v\nQuit=alt-q\nExtendedKeyMap=ctrl-k\n"
            "[panel]\nDown=j\nUp=k\n"
            "[filemanager:xmap]\nMakeDir=m\n"
        )
        left, right = root / "left", root / "right"
        left.mkdir()
        right.mkdir()
        (left / "a.txt").write_bytes(b"selected first document\n")
        (left / "b.txt").write_bytes(b"selected second document\n")
        with TerminalSession(MANAGER, TMUX, ui, [left, right], record_exit=True) as terminal:
            terminal.wait_for("a.txt")
            terminal.type("j")
            terminal.type("v")
            terminal.wait_for("selected first document")
            terminal.press("F10")
            terminal.wait_for("selected first document", present=False)
            terminal.press("C-k")
            terminal.type("m")
            terminal.wait_for("New directory")
            terminal.type("created-by-prefix")
            terminal.press("Enter")
            terminal.wait(lambda: (left / "created-by-prefix").is_dir(), "Prefix creates the directory")
            terminal.wait_for("New directory", present=False)
            terminal.press("M-q")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_editor_extended_keymap_is_one_shot_and_escape_cancels(self):
        root, ui = self.fixture(
            "[editor]\nExtendedKeyMap=ctrl-x\nQuit=alt-q\n"
            "[editor:xmap]\nSave=s\nQuit=q\n"
        )
        path = root / "document"
        path.write_bytes(b"draft\n")
        with TerminalSession(EDITOR, TMUX, ui, [path], record_exit=True) as terminal:
            terminal.wait_editor(dirty=False)
            terminal.type("X")
            terminal.press("C-x")
            terminal.type("s")
            terminal.wait_file(path, b"Xdraft\n")
            terminal.type("s")
            terminal.wait_editor(bytes=8, dirty=True)
            terminal.press("C-x")
            terminal.raw(b"\x1b[27u")
            terminal.type("q")
            terminal.wait_editor(bytes=9, dirty=True)
            self.assertFalse(terminal.exit_file.exists())
            terminal.press("C-x")
            terminal.type("s")
            terminal.wait_file(path, b"Xsqdraft\n")
            terminal.press("C-x")
            terminal.type("q")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_dialog_and_input_bindings_name_an_unnamed_draft(self):
        root, ui = self.fixture(
            "[editor]\nSaveAs=ctrl-w\nQuit=alt-q\n"
            "[input]\nClear=ctrl-l\nHome=ctrl-b\n"
            "[dialog]\nOk=ctrl-o\nCancel=ctrl-g\n"
        )
        with TerminalSession(EDITOR, TMUX, ui, [], record_exit=True) as terminal:
            terminal.wait_editor(bytes=0)
            terminal.type("kept draft")
            terminal.press("C-w")
            terminal.wait_for("Save as")
            terminal.type("wrong.txt")
            terminal.press("C-l")
            terminal.type("actual.txt")
            terminal.press("C-b")
            terminal.type("prefix-")
            terminal.press("C-o")
            terminal.wait_file(ui / "prefix-actual.txt", b"kept draft")
            terminal.press("M-q")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertFalse((ui / "wrong.txt").exists())
        self.assertFalse((ui / "actual.txt").exists())

    def test_viewer_and_hex_contexts_are_separate(self):
        root, ui = self.fixture(
            "[viewer]\nQuit=alt-q\nHexMode=ctrl-h\n"
            "[viewer:hex]\nHexEditMode=ctrl-e\nSave=ctrl-s\n"
        )
        path = root / "document"
        path.write_bytes(b"viewer bytes\n")
        with TerminalSession(VIEWER, TMUX, ui, [path], record_exit=True) as terminal:
            terminal.wait_for("viewer bytes")
            terminal.press("C-h")
            terminal.wait_for("00000000")
            terminal.press("C-e")
            terminal.wait_for("6Save")
            terminal.type("41")
            terminal.press("C-s")
            terminal.wait_file(path, b"Aiewer bytes\n")
            terminal.press("M-q")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_menu_and_listbox_bindings_apply_to_native_controls(self):
        root, ui = self.fixture(
            "[filemanager]\nOptionsAppearance=alt-a\n"
            "[menu]\nQuit=ctrl-g\n"
            "[listbox]\nDown=j\nEnter=ctrl-o\n"
        )
        with TerminalSession(MANAGER, TMUX, ui, [root, root], record_exit=True) as terminal:
            terminal.wait_for("Name")
            terminal.press("F9", "Down")
            terminal.wait_for("Listing format...")
            terminal.press("C-g")
            terminal.wait_for("Listing format...", present=False)
            terminal.press("M-a")
            terminal.wait_for("Skin:")
            terminal.press("Enter")
            terminal.wait_for("Skins")
            terminal.type("j")
            terminal.press("C-o")
            terminal.wait_for("Skins", present=False)
            terminal.press("Tab", "Tab", "Enter")
            terminal.wait_for("Skin:", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertIn("theme=amber\n", (ui / "config/mcpp/config").read_text())

    def test_diff_actions_and_radio_dialog_keys(self):
        root, ui = self.fixture(
            "[diffviewer]\nMerge=ctrl-m\nSave=ctrl-s\nQuit=alt-q\nOptions=alt-o\n"
            "[radio]\nDown=ctrl-n\nUp=ctrl-p\n"
            "[dialog]\nOk=ctrl-o\n"
        )
        first, second = root / "first", root / "second"
        first.write_bytes(b"LEFT\n")
        second.write_bytes(b"RIGHT\n")
        with TerminalSession(DIFF, TMUX, ui, [first, second], record_exit=True) as terminal:
            terminal.wait_for("Dif 1")
            terminal.press("M-o")
            terminal.wait_for("Diff Options")
            terminal.press("C-n", "C-o")
            terminal.wait_for("Diff Options", present=False)
            terminal.press("C-m")
            terminal.wait_for("Dif 0")
            terminal.press("C-s")
            terminal.wait_file(second, b"LEFT\n")
            terminal.press("M-q")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_explicit_file_and_nokeymap_are_launch_only(self):
        root, ui = self.fixture("[viewer]\nQuit=alt-q\n")
        path = root / "document"
        path.write_bytes(b"default keys\n")
        custom = root / "custom.keymap"
        custom.write_text("[viewer]\nQuit=ctrl-q\n")
        with TerminalSession(VIEWER, TMUX, ui, ["--keymap", custom, path], record_exit=True) as terminal:
            terminal.wait_for("default keys")
            terminal.press("C-q")
            terminal.wait_file(terminal.exit_file, b"0\n")
        config = (ui / "config/mcpp/config").read_text()
        self.assertNotIn(str(custom), config)
        with TerminalSession(VIEWER, TMUX, root / "disabled", [
            "-K", root / "missing.keymap", "--nokeymap", path
        ], record_exit=True) as terminal:
            terminal.wait_for("default keys")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_cli_keymap_errors_are_explicit_before_startup(self):
        root, _ = self.fixture("")
        path = root / "document"
        path.write_bytes(b"unchanged\n")
        bad = root / "bad.keymap"
        bad.write_text("[editor]\nSave=not-a-key\n")
        environment = dict(os.environ, XDG_CONFIG_HOME=str(root / "isolated-config"),
                           HOME=str(root / "isolated-home"))
        for binary, files in ((MANAGER, []), (EDITOR, [path]), (VIEWER, [path]), (DIFF, [path, path])):
            for arguments in (["-K"], ["--keymap="], ["-K", root / "missing.keymap"], ["-K", bad]):
                result = subprocess.run([binary, *map(str, arguments), *map(str, files)],
                                        env=environment, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1, (binary, arguments, result.stderr))
                self.assertIn(Path(binary).name + ":", result.stderr)
            result = subprocess.run([binary, "-K", str(bad), *map(str, files)], env=environment,
                                    capture_output=True, text=True, timeout=10)
            self.assertIn(str(bad) + ":2:", result.stderr)
        self.assertEqual(path.read_bytes(), b"unchanged\n")


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Keymap workflows require the owned non-root disposable container")
    MANAGER, EDITOR, VIEWER, DIFF = (str(Path(value).resolve()) for value in sys.argv[1:5])
    TMUX = sys.argv[5]
    ROOT = Path(sys.argv[6]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[7:]], verbosity=2)
