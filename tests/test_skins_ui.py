from pathlib import Path
import math
import os
import re
import subprocess
import sys
import unittest
from unittest.mock import patch

from terminal_session import TerminalSession
from test_audit_ui import click, select_file


PALETTES = {
    "default": ("000080", "dadada", "d0d0d0", "1c1c1c"),
    "grayscale": ("080808", "dadada", "d0d0d0", "1c1c1c"),
    "amber": ("171206", "f5cd73", "31250c", "ffe1a1"),
    "cobalt": ("081b36", "e2edff", "153a63", "ecf4ff"),
    "contrast": ("000000", "ffffff", "121212", "ffffff"),
    "ember": ("2b2019", "f5eadb", "4c3727", "fff1df"),
    "forest": ("14251b", "e5efdf", "2c4533", "f0f6e7"),
    "graphite": ("1c2026", "e4e7eb", "343b45", "f2f4f6"),
    "green": ("07170d", "aee3b7", "183523", "d4f2d3"),
    "ice": ("edf5f8", "193747", "d0e3ec", "183341"),
    "midnight": ("101725", "dce6f1", "22334b", "e6f1ff"),
    "ocean": ("0c252b", "def1ec", "214249", "e0f5f2"),
    "paper": ("f7f5ee", "262b32", "e0e6ed", "1c2734"),
    "plum": ("251b2c", "eee4f1", "48324e", "f8ebfb"),
    "rose": ("2d1c24", "f2e3e9", "51333f", "ffedf3"),
    "sand": ("f1e7d2", "3a3326", "dfcfaf", "342819"),
}
SKINS = list(PALETTES)
SOURCE = b"const int answer = 42; // readable\n"
SGR = re.compile(r"(\x1b\[[0-9;:]*m)")
COLORS16 = [
    (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
    (0, 0, 128), (128, 0, 128), (0, 128, 128), (192, 192, 192),
    (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0),
    (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
]


def rgb(value):
    return tuple(bytes.fromhex(value))


def color256(index):
    if index < 16:
        return COLORS16[index]
    if index >= 232:
        return (8 + 10 * (index - 232),) * 3
    index -= 16
    levels = (0, 95, 135, 175, 215, 255)
    return levels[index // 36], levels[index // 6 % 6], levels[index % 6]


def cells(terminal):
    foreground = background = None
    inverted = False
    rows = []
    for line in terminal.text(ansi=True).splitlines():
        row = []
        for token in SGR.split(line):
            if not token.startswith("\x1b["):
                pair = (background, foreground) if inverted else (foreground, background)
                row.extend((character, *pair) for character in token)
                continue
            parameters = token[2:-1].replace(":", ";").split(";")
            index = 0
            while index < len(parameters):
                code = int(parameters[index] or "0")
                index += 1
                if code == 0:
                    foreground = background = None
                    inverted = False
                elif code in (38, 48):
                    mode = int(parameters[index])
                    index += 1
                    if mode == 5:
                        color = color256(int(parameters[index]))
                        index += 1
                    elif mode == 2:
                        if parameters[index] == "":
                            index += 1
                        color = tuple(map(int, parameters[index:index + 3]))
                        index += 3
                    else:
                        raise AssertionError(f"Unexpected color mode {mode}")
                    if code == 38:
                        foreground = color
                    else:
                        background = color
                elif 30 <= code <= 37:
                    foreground = COLORS16[code - 30]
                elif 40 <= code <= 47:
                    background = COLORS16[code - 40]
                elif 90 <= code <= 97:
                    foreground = COLORS16[code - 90 + 8]
                elif 100 <= code <= 107:
                    background = COLORS16[code - 100 + 8]
                elif code == 39:
                    foreground = None
                elif code == 49:
                    background = None
                elif code == 7:
                    inverted = True
                elif code == 27:
                    inverted = False
        rows.append(row)
    return rows


def contrast(foreground, background):
    def luminance(color):
        components = [value / 255 for value in color]
        components = [value / 12.92 if value <= 0.04045 else math.pow((value + 0.055) / 1.055, 2.4)
                      for value in components]
        return sum(value * weight for value, weight in zip(components, (0.2126, 0.7152, 0.0722)))
    values = sorted((luminance(foreground), luminance(background)))
    return (values[1] + 0.05) / (values[0] + 0.05)


class SkinsUI(unittest.TestCase):
    def fixture(self, suffix="", skin="grayscale"):
        root = ROOT / self._testMethodName / suffix
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        (left / "sample.cpp").write_bytes(SOURCE)
        (left / "work").mkdir()
        config = root / "ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text(f"theme={skin}\nescape_timeout=50000\neditor_line_numbers=false\nmark_moves_down=false\n")
        return root, left, right

    def terminal(self, root, binary, arguments):
        return TerminalSession(binary, TMUX, root / "ui", arguments, record_exit=True)

    def panels(self, terminal):
        terminal.wait_for("sample.cpp")
        terminal.wait(lambda: "Options" in terminal.text().splitlines()[0], "panels are visible")

    def pair(self, terminal, x=20, y=20):
        cell = cells(terminal)[y][x]
        return cell[1], cell[2]

    def assert_readable(self, pair):
        self.assertIsNotNone(pair[0])
        self.assertIsNotNone(pair[1])
        self.assertGreaterEqual(contrast(*pair), 4.5)

    def appearance(self, terminal):
        click(terminal, "Options", row=0)
        terminal.wait_for("Appearance...")
        click(terminal, "Appearance...")
        terminal.wait_for("Skin:")

    def choose(self, terminal, skin):
        terminal.press("Enter")
        terminal.wait_for("Skins")
        terminal.press("Home", *(["Down"] * SKINS.index(skin)))
        expected = "< Default >" if skin == "default" else "gray" if skin == "grayscale" else skin
        terminal.wait(lambda: expected in terminal.focused_text(), "selected skin is visible while the list scrolls")
        terminal.press("Enter")
        terminal.wait_for("Skins", present=False)
        terminal.wait_for("Skin:")

    def test_all_palettes_color_panels_viewer_dialogs_and_marks(self):
        for skin, (background, foreground, surface, dialog_foreground) in PALETTES.items():
            with self.subTest(skin=skin):
                root, left, right = self.fixture(skin, skin)
                with self.terminal(root, MANAGER, [left, right]) as terminal:
                    self.panels(terminal)
                    self.assertEqual(self.pair(terminal), (rgb(foreground), rgb(background)))
                    self.assert_readable(self.pair(terminal))
                    self.assert_readable(self.pair(terminal, 70, 3))
                    terminal.save("panels")
                    select_file(terminal, "sample.cpp")
                    terminal.press("Insert")
                    terminal.wait_for("1 selected,")
                    row = next(index for index, line in enumerate(terminal.text().splitlines())
                               if "sample.cpp" in line and "│" in line)
                    column = terminal.text().splitlines()[row].index("sample.cpp")
                    self.assert_readable(self.pair(terminal, column, row))
                    terminal.press("F3")
                    terminal.wait_for("const int answer")
                    self.assertEqual(self.pair(terminal), (rgb(foreground), rgb(background)))
                    terminal.save("viewer")
                    terminal.press("F10")
                    self.panels(terminal)
                    terminal.press("F7")
                    terminal.wait_for("New directory")
                    lines = terminal.text().splitlines()
                    row = next(index for index, line in enumerate(lines) if "Directory name:" in line)
                    pair = self.pair(terminal, lines[row].index("Directory name:"), row)
                    self.assertEqual(pair, (rgb(dialog_foreground), rgb(surface)))
                    self.assert_readable(pair)
                    terminal.save("dialog")
                    terminal.press("F10")
                    terminal.wait_for("New directory", present=False)
                    terminal.press("F10")
                    terminal.wait_file(terminal.exit_file, b"0\n")
                    self.assertIn(f"theme={skin}\n", (terminal.root / "config/mcpp/config").read_text())
                self.assertEqual((left / "sample.cpp").read_bytes(), SOURCE)

    def test_all_editor_palettes_keep_syntax_readable_and_files_unchanged(self):
        for skin, (background, foreground, _, _) in PALETTES.items():
            with self.subTest(skin=skin):
                root, left, _ = self.fixture(skin)
                path = left / "sample.cpp"
                with self.terminal(root, EDITOR, ["--skin", skin, path]) as terminal:
                    terminal.wait_editor(offset=0)
                    pair = self.pair(terminal, 1, 1)
                    self.assertEqual(pair[1], rgb(background))
                    self.assert_readable(pair)
                    self.assertNotEqual(pair[0], rgb(foreground))
                    terminal.save("editor")
                    terminal.press("C-s")
                    terminal.wait(lambda: self.pair(terminal, 1, 1) == (rgb(foreground), rgb(background)),
                                  "turning syntax off retains the selected skin")
                    terminal.press("F10")
                    terminal.wait_file(terminal.exit_file, b"0\n")
                    self.assertIn("theme=grayscale\n", (terminal.root / "config/mcpp/config").read_text())
                self.assertEqual(path.read_bytes(), SOURCE)

    def test_picker_scroll_preview_and_cancel_restore_skin_and_shadows(self):
        root, left, right = self.fixture()
        with self.terminal(root, MANAGER, [left, right]) as terminal:
            self.panels(terminal)
            original = self.pair(terminal)
            self.appearance(terminal)
            self.choose(terminal, "sand")
            self.assertEqual(self.pair(terminal, 2, 20)[1], rgb(PALETTES["sand"][0]))
            click(terminal, "Shadows")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Skin:", present=False)
            self.assertEqual(self.pair(terminal), original)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            stored = (terminal.root / "config/mcpp/config").read_text()
            self.assertIn("theme=grayscale\n", stored)
            self.assertIn("shadows=true\n", stored)

    def test_picker_selection_survives_a_new_process(self):
        root, left, right = self.fixture()
        with self.terminal(root, MANAGER, [left, right]) as terminal:
            self.panels(terminal)
            self.appearance(terminal)
            self.choose(terminal, "forest")
            click(terminal, "[< OK >]")
            terminal.wait_for("Skin:", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            self.assertIn("theme=forest\n", (terminal.root / "config/mcpp/config").read_text())
        with self.terminal(root, MANAGER, [left, right]) as terminal:
            self.panels(terminal)
            self.assertEqual(self.pair(terminal)[1], rgb(PALETTES["forest"][0]))
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_cli_override_and_cancel_do_not_replace_the_saved_skin(self):
        root, left, right = self.fixture(skin="forest")
        with self.terminal(root, MANAGER, ["-S", "paper", left, right]) as terminal:
            self.panels(terminal)
            self.assertEqual(self.pair(terminal)[1], rgb(PALETTES["paper"][0]))
            self.appearance(terminal)
            self.choose(terminal, "ocean")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Skin:", present=False)
            self.assertEqual(self.pair(terminal)[1], rgb(PALETTES["paper"][0]))
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            self.assertIn("theme=forest\n", (terminal.root / "config/mcpp/config").read_text())

    def test_all_frontends_accept_and_validate_skin_arguments(self):
        root, left, right = self.fixture()
        path = left / "sample.cpp"
        other = right / "other.cpp"
        other.write_bytes(b"other\n")
        for binary, files in ((MANAGER, [left, right]), (EDITOR, [path]), (VIEWER, [path]), (DIFF, [path, other])):
            name = Path(binary).name
            with self.subTest(binary=name):
                for arguments in (["--skin"], ["--skin=not-a-skin", *files]):
                    result = subprocess.run([binary, *map(str, arguments)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn(name + ": ", result.stderr)
                    self.assertIn("requires a value" if len(arguments) == 1 else "unknown skin", result.stderr)
                case = root / name
                with self.terminal(case, binary, ["--skin=midnight", *files]) as terminal:
                    terminal.wait(lambda: any(row and row[0][2] == rgb(PALETTES["midnight"][0])
                                              for row in cells(terminal)[1:20]), "frontend applies its requested skin")
                    terminal.press("F10")
                    terminal.wait_file(terminal.exit_file, b"0\n")


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Skin checks require the owned non-root disposable container")
    MANAGER, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    EDITOR, VIEWER, DIFF = (str(Path(MANAGER).with_name(name)) for name in ("mcppedit", "mcppview", "mcppdiff"))
    ROOT = Path(sys.argv[3]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    with patch.dict(os.environ, {"COLORTERM": "truecolor"}):
        unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
