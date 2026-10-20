from pathlib import Path
import os
import sys
import unittest
from unittest.mock import patch

from terminal_session import TerminalSession
from test_audit_ui import click
from test_skins_ui import cells


SKIN = """[skin]
description=Independent L23 sample
truecolors=true
[aliases]
accent=brightcyan
focus=yellow
[core]
_default_=lightgray;#102030;none
selected=black;accent;none
markselect=black;focus;none
shadow=black;black;none
[menu]
_default_=white;#405060;none
menusel=black;accent;none
menuhot=focus;;none
[dialog]
_default_=white;#304050;none
dfocus=black;focus;none
[filehighlight]
notes=#aabbcc;;none
"""


class NativeSkinsUI(unittest.TestCase):
    def fixture(self, native=False):
        root = ROOT / self._testMethodName
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        (left / "sample.l23").write_text("small sample\n")
        config = root / "ui/config/mcpp"
        (config / "skins").mkdir(parents=True)
        skin = config / "skins/independent.ini"
        skin.write_text(SKIN)
        (config / "filehighlight.ini").write_text("[notes]\nextensions=l23\n")
        (config / "config").write_text(
            f"theme={skin if native else 'forest'}\nuse_subshell=false\n"
            "escape_timeout=50000\neditor_check_newline=false\neditor_confirm_save=false\n"
        )
        return root, left, right, config, skin

    def appearance(self, terminal):
        click(terminal, "Options", row=0)
        terminal.wait_for("Appearance...")
        click(terminal, "Appearance...")
        terminal.wait_for("Skin:")

    def choose_user_skin(self, terminal):
        terminal.press("Enter")
        terminal.wait_for("Skins")
        terminal.press("Home", *(["Down"] * 16))
        terminal.wait(lambda: any("independent" in value for value in terminal.focused_text()),
                      "the original user fixture is selected")
        terminal.press("Enter")

    def pair_at(self, terminal, needle):
        lines = terminal.text().splitlines()
        row = next(index for index, line in enumerate(lines) if needle in line)
        column = lines[row].index(needle)
        cell = cells(terminal)[row][column]
        return cell[1], cell[2]

    def test_native_cli_rendering_and_appearance(self):
        root, left, right, config, skin = self.fixture()
        with TerminalSession(MANAGER, TMUX, root / "ui", ["-S", skin, left, right],
                             record_exit=True) as terminal:
            terminal.wait_for("sample.l23")
            self.assertEqual(self.pair_at(terminal, "sample.l23"), ((170, 187, 204), (16, 32, 48)))
            click(terminal, "Options", row=0)
            terminal.wait_for("Configuration...")
            self.assertEqual(self.pair_at(terminal, "Configuration..."), ((0, 0, 0), (0, 255, 255)))
            terminal.press("Escape")
            terminal.wait_for("Configuration...", present=False)
            terminal.press("F7")
            terminal.wait_for("Directory name:")
            self.assertEqual(self.pair_at(terminal, "> "), ((0, 0, 0), (255, 255, 0)))
            terminal.press("Escape")
            terminal.wait_for("Directory name:", present=False)
            self.appearance(terminal)
            terminal.press("Enter")
            terminal.wait_for("Skins")
            terminal.press("Home", "Down", "Down", "Enter")
            terminal.wait_for("Skin:")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Skin:", present=False)
            self.assertEqual(self.pair_at(terminal, "sample.l23"), ((170, 187, 204), (16, 32, 48)))
            self.appearance(terminal)
            self.choose_user_skin(terminal)
            terminal.wait_for("Independent L23 sample")
            click(terminal, "[< OK >]")
            terminal.wait_for("Skin:", present=False)
            terminal.save("native-skin-rendered")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertIn(f"theme={skin}\n", (config / "config").read_text())

    def test_rejected_skin_keeps_the_appearance_draft(self):
        root, left, right, config, skin = self.fixture()
        skin.write_text("[aliases]\na=b\nb=a\n[core]\n_default_=a;black\n")
        with TerminalSession(MANAGER, TMUX, root / "ui", [left, right], record_exit=True) as terminal:
            terminal.wait_for("sample.l23")
            before = self.pair_at(terminal, "sample.l23")
            self.appearance(terminal)
            self.choose_user_skin(terminal)
            terminal.wait_for("Skin error")
            terminal.wait_for("alias cycle")
            terminal.save("rejected-skin")
            terminal.press("Enter")
            terminal.wait_for("Skin:")
            self.assertIn("forest", terminal.text())
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Skin:", present=False)
            self.assertEqual(self.pair_at(terminal, "sample.l23"), before)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertIn("theme=forest\n", (config / "config").read_text())

    def test_bad_highlight_reload_keeps_the_last_colors(self):
        root, left, right, config, skin = self.fixture(native=True)
        with TerminalSession(MANAGER, TMUX, root / "ui", [left, right], record_exit=True) as terminal:
            terminal.wait_for("sample.l23")
            before = self.pair_at(terminal, "sample.l23")
            click(terminal, "Command", row=0)
            terminal.wait_for("Edit highlighting group file")
            click(terminal, "Edit highlighting group file")
            terminal.wait_for("extensions=l23")
            terminal.press("C-End")
            terminal.type("\nregexp=[")
            terminal.press("F2")
            terminal.wait_for("Saved; configuration error")
            terminal.wait_for("filename regexp")
            terminal.save("rejected-highlight-reload")
            terminal.press("Enter", "F10")
            terminal.wait_for("sample.l23")
            self.assertEqual(self.pair_at(terminal, "sample.l23"), before)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertIn("regexp=[", (config / "filehighlight.ini").read_text())


if __name__ == "__main__":
    if os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Native skin checks require the existing UID1001 isolated container")
    MANAGER, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    ROOT = Path(sys.argv[3]).resolve() / f"run-{os.getpid()}"
    if not str(ROOT).startswith("/review/cases/native-skins/"):
        raise SystemExit("Use /review/cases/native-skins for these focused cases")
    ROOT.mkdir(parents=True)
    with patch.dict(os.environ, {"COLORTERM": "truecolor"}):
        unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
