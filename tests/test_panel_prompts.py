from contextlib import contextmanager
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

from terminal_session import TerminalSession


@contextmanager
def session_case(binary, tmux):
    root = Path(tempfile.mkdtemp(prefix="mcpp-panel-", dir=".")).resolve()
    directory = root.relative_to(Path.cwd())
    passed = False
    try:
        (directory / "left/nested").mkdir(parents=True)
        (directory / "right").mkdir()
        (directory / "left/alpha.txt").write_text("ALPHA CONTENT\n")
        (directory / "left/other.txt").write_text("OTHER CONTENT\n")
        (directory / "left/nested/target.txt").write_text("NESTED CONTENT\n")
        (directory / "right/right.txt").write_text("RIGHT CONTENT\n")
        left, right = root / "left", root / "right"
        # Relative socket paths also fit deeply nested build directories.
        with TerminalSession(
            binary, tmux, directory / "ui", [left, right]
        ) as terminal:
            try:
                terminal.wait_for("alpha.txt")
                yield terminal, left, right
            except BaseException:
                terminal.save("failure")
                raise
        passed = True
    finally:
        if passed:
            shutil.rmtree(directory)
        else:
            print(f"Panel prompt failure artifacts: {root}", file=sys.stderr)


def styled(terminal, needle, attribute=7, reset=27):
    enabled = False
    for line in terminal.text(ansi=True).splitlines():
        plain, flags = "", []
        for token in re.split(r"(\x1b\[[0-9;:]*m)", line):
            if token.startswith("\x1b["):
                values = [int(value or 0) for value in
                          token[2:-1].replace(":", ";").split(";")]
                index = 0
                while index < len(values):
                    value = values[index]
                    if value in (38, 48) and index + 1 < len(values):
                        index += 3 if values[index + 1] == 5 else 5
                        continue
                    if value in (0, reset):
                        enabled = False
                    elif value == attribute:
                        enabled = True
                    index += 1
            else:
                plain += token
                flags.extend([enabled] * len(token))
        start = plain.find(needle)
        if start >= 0 and all(flags[start:start + len(needle)]):
            return True
    return False


def focus(terminal, label):
    terminal.wait(lambda: styled(terminal, label), f"Focus on {label}")


def click(terminal, column, row):
    terminal.raw(
        f"\x1b[<0;{column};{row}M\x1b[<0;{column};{row}m".encode()
    )


def click_label(terminal, label):
    lines = terminal.text().splitlines()
    row = next(index for index, line in enumerate(lines) if label in line)
    click(terminal, lines[row].index(label) + 2, row + 1)


def menu(terminal, category, label):
    line = terminal.text().splitlines()[0]
    click(terminal, line.index(category) + 2, 1)
    terminal.wait_for(label)
    click_label(terminal, label)


def mkdir_prompt(terminal, value):
    terminal.press("F7")
    terminal.wait_for("Directory name:")
    terminal.type(value)
    terminal.wait_for("> " + value)


def view_prompt(terminal):
    menu(terminal, "File", "View file...")
    terminal.wait_for("Filename:")


def find_alpha(terminal):
    terminal.press("C-s")
    terminal.wait_for("Quick search:")
    terminal.type("alpha.txt")
    terminal.wait_for("Quick search: alpha.txt")
    terminal.press("Enter")
    terminal.wait_for("Quick search:", present=False)


class PanelPrompts(unittest.TestCase):
    def test_keyboard_focus_and_cancellation(self):
        with session_case(BINARY, TMUX) as (terminal, left, _):
            mkdir_prompt(terminal, "tab-cancel")
            self.assertFalse(styled(terminal, "OK"))
            terminal.press("Tab")
            focus(terminal, "OK")
            terminal.type("ignored")
            terminal.press("Tab")
            focus(terminal, "Cancel")
            self.assertNotIn("ignored", terminal.text())
            terminal.press("Enter")
            terminal.wait_for("Directory name:", present=False)
            self.assertFalse((left / "tab-cancel").exists())

            mkdir_prompt(terminal, "reverse-cancel")
            terminal.raw(b"\x1b[Z")
            focus(terminal, "Cancel")
            terminal.type(" ")
            terminal.wait_for("Directory name:", present=False)
            self.assertFalse((left / "reverse-cancel").exists())

            mkdir_prompt(terminal, "keyboard-ok")
            terminal.press("Tab")
            focus(terminal, "OK")
            terminal.press("Enter")
            terminal.wait_for("Directory created")
            self.assertTrue((left / "keyboard-ok").is_dir())

            mkdir_prompt(terminal, "escape-cancel")
            focus(terminal, "> escape-cancel")
            terminal.press("Escape")
            terminal.wait_for("Directory name:", present=False)
            self.assertFalse((left / "escape-cancel").exists())

    def test_mouse_input_and_buttons(self):
        with session_case(BINARY, TMUX) as (terminal, left, _):
            mkdir_prompt(terminal, "mouse-cancel")
            click_label(terminal, "Cancel")
            terminal.wait_for("Directory name:", present=False)
            self.assertFalse((left / "mouse-cancel").exists())

            mkdir_prompt(terminal, "mouse-ok")
            terminal.press("Tab")
            focus(terminal, "OK")
            click_label(terminal, "> mouse-ok")
            focus(terminal, "> mouse-ok")
            terminal.press("End")
            terminal.type("-edited")
            terminal.wait_for("> mouse-ok-edited")
            click_label(terminal, "OK")
            terminal.wait_for("Directory created")
            self.assertTrue((left / "mouse-ok-edited").is_dir())
            self.assertFalse((left / "mouse-ok").exists())

    def test_cursor_selection_word_editing_and_history(self):
        with session_case(BINARY, TMUX) as (terminal, left, _):
            mkdir_prompt(terminal, "abécd")
            terminal.press("Left", "Left")
            terminal.type("X")
            terminal.wait_for("> abéXcd")
            terminal.press("DC")
            terminal.wait_for("> abéXd")
            terminal.raw(b"\x1b[1;2H")
            terminal.type("z")
            terminal.wait_for("> zd")
            terminal.press("End", "BSpace", "Enter")
            terminal.wait(lambda: (left / "z").is_dir(), "Edited directory z is created")

            mkdir_prompt(terminal, "one two")
            terminal.press("Home", "C-Right", "C-k")
            terminal.wait_for("> one two", present=False)
            terminal.wait_for("> one")
            terminal.press("C-y")
            terminal.wait_for("> one two")
            terminal.press("C-u", "C-y", "Enter")
            terminal.wait(
                lambda: (left / "one two").is_dir(),
                "Kill/yank preserves the submitted directory name",
            )

            terminal.press("F7", "M-p")
            terminal.wait_for("> one two")
            terminal.press("M-p")
            terminal.wait_for("> z")
            terminal.press("M-n")
            terminal.wait_for("> one two")
            terminal.press("Escape")
            terminal.wait_for("Directory name:", present=False)

    def test_view_file_paths_errors_and_selection(self):
        with session_case(BINARY, TMUX) as (terminal, _, right):
            find_alpha(terminal)
            terminal.press("Insert")
            terminal.wait_for("1 selected,")
            terminal.press("Up")

            view_prompt(terminal)
            terminal.wait_for("> alpha.txt")
            terminal.press("Tab", "Tab", "Enter")
            terminal.wait_for("Filename:", present=False)
            self.assertNotIn("View file...", terminal.text())
            terminal.wait_for("1 selected,")

            for value, content in (
                ("nested/target.txt", "NESTED CONTENT"),
                (str(right / "right.txt"), "RIGHT CONTENT"),
            ):
                view_prompt(terminal)
                terminal.wait_for("> alpha.txt")
                terminal.press("C-u")
                terminal.type(value)
                terminal.press("Enter")
                terminal.wait_for(content)
                terminal.press("F3")
                terminal.wait_for("1 selected,")

            view_prompt(terminal)
            for value, error in (
                ("", "filename is empty"),
                ("missing.txt", "path does not exist"),
                ("nested", "not a regular file"),
            ):
                terminal.press("C-u")
                terminal.type(value)
                terminal.press("Enter")
                terminal.wait_for("Error: " + error)
                terminal.wait_for("Filename:")
                focus(terminal, "> " + value)
            click_label(terminal, "Cancel")
            terminal.wait_for("Filename:", present=False)
            terminal.wait_for("1 selected,")
            view_prompt(terminal)
            terminal.wait_for("> alpha.txt")
            terminal.press("Escape")
            terminal.wait_for("Filename:", present=False)

    def test_path_completion_and_command_line_editing(self):
        with session_case(BINARY, TMUX) as (terminal, left, _):
            find_alpha(terminal)
            view_prompt(terminal)
            terminal.press("C-u")
            terminal.type("nest")
            terminal.raw(b"\x1b\t")
            terminal.wait_for("> nested/")
            terminal.type("tar")
            terminal.raw(b"\x1b\t")
            terminal.wait_for("> nested/target.txt")
            terminal.press("Enter")
            terminal.wait_for("NESTED CONTENT")
            terminal.press("F3")
            terminal.wait_for("Name")

            (left / "apple.txt").write_text("APPLE\n")
            (left / "apricot.txt").write_text("APRICOT\n")
            view_prompt(terminal)
            terminal.press("C-u")
            terminal.type("ap")
            terminal.raw(b"\x1b\t")
            terminal.wait_for("2 paths;")
            terminal.raw(b"\x1b\t")
            terminal.wait_for("> apple.txt")
            terminal.raw(b"\x1b\t")
            terminal.wait_for("> apricot.txt")
            terminal.press("Escape")
            terminal.wait_for("Filename:", present=False)

            terminal.type("touch cmd-tes.txt")
            terminal.press("Left", "Left", "Left", "Left")
            terminal.type("t")
            terminal.wait_for("touch cmd-test.txt")
            terminal.press("Enter")
            terminal.wait(
                lambda: (left / "cmd-test.txt").is_file(),
                "Edited command creates the intended file",
            )
            terminal.press("M-p")
            terminal.wait_for("touch cmd-test.txt")
            terminal.press("Home", "C-k")
            terminal.wait_for("touch cmd-test.txt", present=False)
            terminal.press("C-y")
            terminal.wait_for("touch cmd-test.txt")
            terminal.press("Escape")
            terminal.wait_for("touch cmd-test.txt", present=False)

    def test_copy_destination_cursor_editing(self):
        with session_case(BINARY, TMUX) as (terminal, left, right):
            find_alpha(terminal)
            terminal.press("F5")
            terminal.wait_for("Source mask:")
            terminal.press("C-u")
            terminal.type(str(right / "copy-nam.txt"))
            terminal.press("Left", "Left", "Left", "Left")
            terminal.type("e")
            terminal.wait_for("copy-name.txt")
            terminal.press("Enter")
            terminal.wait_file(right / "copy-name.txt", b"ALPHA CONTENT\n")
            self.assertTrue((left / "alpha.txt").is_file())

    def test_info_home_end_preserves_listing_cursor_and_marks(self):
        with session_case(BINARY, TMUX) as (terminal, _left, _right):
            find_alpha(terminal)
            terminal.press("Insert")
            terminal.wait_for("1 selected,")
            terminal.press("Up")
            menu(terminal, "Right", "Info")
            terminal.wait_for("Name: alpha.txt")
            terminal.press("Tab")
            terminal.wait_for("Info panel focused")
            for key in ("Home", "End"):
                terminal.press(key, "Tab")
                terminal.wait_for("File listing focused")
                self.assertIn("Name: alpha.txt", terminal.text())
                self.assertIn("1 selected,", terminal.text())
                terminal.press("Tab")
                terminal.wait_for("Info panel focused")

    def test_view_file_default_for_nested_panelized_result(self):
        with session_case(BINARY, TMUX) as (terminal, _left, _right):
            menu(terminal, "Left", "Panelize by name...")
            terminal.wait_for("Panelize name pattern")
            terminal.press("C-u")
            terminal.type("target.txt")
            terminal.press("Enter")
            terminal.wait_for("1 panelized result(s)")
            terminal.wait_for("target.txt")
            terminal.press("Insert")
            terminal.wait_for("1 selected,")

            view_prompt(terminal)
            self.assertNotIn("> target.txt", terminal.text())
            terminal.press("Enter")
            terminal.wait_for("NESTED CONTENT")
            terminal.press("F3")
            terminal.wait_for("1 selected,")
            terminal.wait_for("target.txt")

            view_prompt(terminal)
            terminal.press("Escape")
            terminal.wait_for("Filename:", present=False)
            terminal.wait_for("1 selected,")

    def test_external_panelize_disabled_and_name_panelize_retained(self):
        with session_case(BINARY, TMUX) as (terminal, _, _right):
            line = terminal.text().splitlines()[0]
            click(terminal, line.index("Command") + 2, 1)
            terminal.wait_for("External panelize")
            self.assertTrue(styled(terminal, "External panelize", 2, 22))
            click_label(terminal, "External panelize")
            focus(terminal, "External panelize")
            self.assertTrue(styled(terminal, "External panelize", 2, 22))
            self.assertNotIn("Panelize name pattern", terminal.text())
            terminal.press("Escape")
            terminal.wait_for("External panelize", present=False)
            terminal.press("C-x")
            terminal.type("!")
            terminal.wait_for("External panelize is not implemented")
            self.assertNotIn("Panelize name pattern", terminal.text())

            menu(terminal, "Left", "Panelize by name...")
            terminal.wait_for("Panelize name pattern")
            terminal.press("C-u")
            terminal.type("target.txt")
            terminal.press("Enter")
            terminal.wait_for("target.txt")
            terminal.wait_for("alpha.txt", present=False)
            terminal.wait_for("other.txt", present=False)


if __name__ == "__main__":
    BINARY = str(Path(sys.argv[1]).resolve())
    TMUX = shutil.which(sys.argv[2]) or sys.argv[2]
    unittest.main(argv=[sys.argv[0], *sys.argv[3:]])
