from pathlib import Path
from contextlib import contextmanager
import os
import pwd
import grp
import sys
import unittest

from terminal_session import TerminalSession


def mouse(terminal, x, y, button=0, motion="M"):
    terminal.raw(f"\x1b[<{button};{x + 1};{y + 1}{motion}".encode())


def click(terminal, label, row=None, twice=False, rightmost=False):
    lines = terminal.text().splitlines()
    if row is None:
        row = next(index for index, line in enumerate(lines) if label in line)
    x = (lines[row].rindex(label) if rightmost else lines[row].index(label)) + max(1, len(label) // 2)
    for _ in range(2 if twice else 1):
        mouse(terminal, x, row)
        mouse(terminal, x, row, motion="m")


def key_click(terminal, number, label):
    click(terminal, str(number) + label, row=len(terminal.text().splitlines()) - 1)


def select_file(terminal, name):
    terminal.press("C-s")
    terminal.wait_for("Quick search:")
    terminal.type(name)
    terminal.wait_for("Quick search: " + name)
    terminal.press("Enter")
    terminal.wait_for("Quick search:", present=False)
    terminal.wait(lambda: name in terminal.text().splitlines()[-5],
                  f"Listing selects {name}")


def menu(terminal, category, item):
    def menubar():
        return next((index for index, line in enumerate(terminal.text().splitlines())
                     if all(name in line for name in ("File", "Edit", "Search", "Window"))), None)
    if menubar() is None:
        terminal.press("F9")
        terminal.wait(lambda: menubar() is not None,
                      f"Editor menu category {category} appears")
    click(terminal, category, row=menubar())
    terminal.wait(lambda: any(item in line and "│" in line for line in terminal.text().splitlines()),
                  f"Editor dropdown contains {item}")
    lines = terminal.text().splitlines()
    row = next(index for index, line in enumerate(lines) if item in line and "│" in line)
    start = lines[row].rfind("│", 0, lines[row].index(item))
    end = lines[row].find("│", lines[row].index(item))
    before = lines[row][start:end + 1]
    click(terminal, item, row=row)
    terminal.wait(lambda: terminal.text().splitlines()[row][start:end + 1] != before,
                  f"Editor menu action {item} closes its dropdown")


def focused(terminal, suffix):
    return any(value.endswith(suffix) for value in terminal.focused_text())


def selected_result(terminal, suffix):
    terminal.wait(lambda: focused(terminal, suffix), f"List selects {suffix}")


def save(terminal, path, expected):
    terminal.save_editor()
    terminal.wait_file(path, expected)


class AuditUI(unittest.TestCase):
    def fixture(self):
        root = ROOT / self._testMethodName
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        return root, left, right

    @contextmanager
    def terminal(self, root, arguments, width=100, height=30, record_stderr=False):
        with TerminalSession(BINARY, TMUX, root, arguments, width, height,
                             record_exit=True, record_stderr=record_stderr) as terminal:
            yield terminal

    def test_commander_mouse_caret_selection_cd_and_exit(self):
        root, left, right = self.fixture()
        (left / "child space").mkdir()
        with self.terminal(root / "ui", ["--printwd", root / "cwd", left, right]) as t:
            t.wait_for("Name")
            key_click(t, 1, "Help")
            t.wait_for("mcpp help")
            t.press("F10")
            t.wait_for("mcpp help", present=False)
            t.type("abcdef")
            t.wait_for("abcdef")
            row = len(t.text().splitlines()) - 2
            start = t.text().splitlines()[row].index("abcdef")
            mouse(t, start + 2, row)
            mouse(t, start + 2, row, motion="m")
            t.type("X")
            t.wait_for("abXcdef")
            mouse(t, start + 2, row)
            mouse(t, start + 6, row, 32)
            mouse(t, start + 6, row, motion="m")
            t.type("Y")
            t.wait_for("abYf")
            t.press("Home", "C-k")
            t.wait_for("abYf", present=False)
            mouse(t, start, row)
            mouse(t, start, row, motion="m")
            t.type("cd 'child space'")
            t.press("Enter")
            t.wait(lambda: "child space" in t.text().splitlines()[1][:50],
                   "cd updates the active pane directory")
            t.type("cd -")
            t.press("Enter")
            t.wait(lambda: "~/left" in t.text().splitlines()[1][:50],
                   "cd - returns to the previous pane directory")
            t.type("cd 'child space'")
            t.press("Enter")
            t.wait(lambda: "child space" in t.text().splitlines()[1][:50],
                   "cd re-enters the quoted directory")
            t.type("exit")
            t.press("Enter")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text().strip(), str(left / "child space"))

    def test_stable_pane_viewport_and_unicode_quick_search(self):
        root, left, right = self.fixture()
        for index in range(80):
            (left / f"item_{index:03}.txt").write_text(str(index))
            (right / f"right_{index:03}.txt").write_text(str(index))
        (left / "Ångström.txt").write_text("unicode")
        config = root / "ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text("quick_search_mode=0\n")
        with self.terminal(root / "ui", [left, right]) as t:
            t.wait_for("item_000")
            t.press(*(["Down"] * 15))
            t.wait(lambda: "item_014" in t.text().splitlines()[-5],
                   "Fifteen arrows move the cursor, not the top row")
            self.assertIn("item_000", t.text().splitlines()[4])
            t.press("PgDn")
            t.wait(lambda: "item_035" in t.text().splitlines()[-5], "PageDown advances a page")
            before = t.text().splitlines()[3][:50]
            t.press("Tab", "Down", "Tab")
            t.wait(lambda: "item_035" in t.text().splitlines()[-5], "Pane focus returns")
            self.assertEqual(t.text().splitlines()[3][:50], before)
            t.resize(100, 20)
            t.wait(lambda: "item_035" in t.text().splitlines()[-5], "Resize keeps the cursor visible")
            t.press("C-s")
            t.wait_for("Quick search:")
            t.type("ång")
            t.press("Enter")
            t.wait_for("Quick search:", present=False)
            t.wait(lambda: "Ångström.txt" in t.text().splitlines()[-5],
                   "Case-insensitive UTF-8 prefix selects Ångström")

    def test_quick_cd_uses_the_active_directory(self):
        root, left, right = self.fixture()
        child = left / "child"
        (child / "grandchild").mkdir(parents=True)
        alias = left / "alias"
        alias.symlink_to(child, target_is_directory=True)
        (right / "other-child").mkdir()
        with self.terminal(root / "ui", ["--printwd", root / "cwd", child, right]) as t:
            t.wait_for("grandchild")
            for value, expected in ((".", child), ("..", left),
                                    ("child/grandchild", child / "grandchild"),
                                    (str(alias), alias), (".", alias), ("..", left),
                                    (str(child), child)):
                t.press("M-c")
                t.wait_for("Change directory")
                t.press("C-u")
                t.type(value)
                t.press("Enter")
                t.wait_for("Change directory", present=False)
                t.wait(lambda: expected.name in t.text().splitlines()[1][:50],
                       f"Quick cd enters {expected}")
            t.press("Tab", "M-c")
            t.wait_for("Change directory")
            t.press("C-u")
            t.type("other-child")
            t.press("Enter")
            t.wait_for("Change directory", present=False)
            t.wait(lambda: "other-child" in t.text().splitlines()[1][50:],
                   "Relative Quick cd follows the focused right pane")
            t.press("M-c")
            t.wait_for("Change directory")
            t.press("C-u")
            t.type("missing")
            t.press("Enter")
            t.wait_for("Change directory", present=False)
            t.wait_for("Error:")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_bytes(), (str(right / "other-child") + "\n").encode())

    def test_newline_cwd_keeps_prompt_and_input_on_one_row(self):
        root, left, right = self.fixture()
        directory = left / "line\nbreak"
        directory.mkdir()
        with self.terminal(root / "ui", ["--printwd", root / "cwd", directory, right]) as t:
            t.wait(lambda: "line\\nbreak$" in t.text().splitlines()[-2],
                   "The command prompt escapes the cwd newline")
            self.assertIn("line\\nbreak", t.text().splitlines()[1][:50])
            t.type("abcdef")
            t.wait_for("abcdef")
            row = len(t.text().splitlines()) - 2
            x = t.text().splitlines()[row].index("abcdef") + 2
            mouse(t, x, row)
            mouse(t, x, row, motion="m")
            t.type("X")
            t.wait_for("abXcdef")
            t.press("Home", "C-k")
            t.resize(240, 30)
            t.type("cd .")
            t.press("Enter")
            t.wait(lambda: "line\\nbreak" in t.text().splitlines()[-3],
                   "The cwd in command status is escaped too")
            t.save("escaped-cwd-prompt")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual(Path((root / "cwd").read_text().removesuffix("\n")), directory)

    def test_main_menu_mnemonics_preserve_the_command_draft(self):
        root, left, right = self.fixture()
        with self.terminal(root / "ui", [left, right]) as t:
            t.wait_for("Name")
            t.type("touch should-not-run")
            for key, item in (("f", "Quick cd"), ("C", "Find file"), ("o", "Save setup")):
                t.press("F9")
                t.type(key)
                t.wait_for(item)
                t.press("Escape")
                t.wait_for(item, present=False)
                self.assertTrue(t.text().splitlines()[-2].rstrip().endswith("touch should-not-run"))
            t.press("F9")
            t.type("F")
            t.wait_for("Quick cd")
            click(t, "Mkdir")
            t.wait_for("New directory")
            t.press("Escape")
            t.wait_for("New directory", present=False)
            self.assertTrue(t.text().splitlines()[-2].rstrip().endswith("touch should-not-run"))
            t.save("mnemonic-modal-return")
            t.press("Home", "C-k", "F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertFalse((left / "should-not-run").exists())

    def test_text_viewer_highlights_current_match_and_utf8(self):
        root, left, _ = self.fixture()
        path = left / "matches.txt"
        path.write_text("aaaaa\nab\tK界tail\n")
        with self.terminal(root / "ui", ["--view", path]) as t:
            t.wait_for("aaaaa")
            t.press("F7")
            t.wait_for("Enter search string:")
            t.type("aa")
            t.press("Enter")
            t.wait_for("Enter search string:", present=False)
            t.wait(lambda: t.styled_rows()[1]["spans"] == [(0, 2)],
                   "The first actual occurrence is highlighted")
            t.type("n")
            t.wait(lambda: t.styled_rows()[1]["spans"] == [(2, 4)],
                   "Search continuation visibly advances to the next occurrence")
            t.press("F7")
            t.wait_for("Enter search string:")
            t.press("C-u")
            t.type("k界")
            t.press("Enter")
            t.wait_for("Enter search string:", present=False)
            t.wait(lambda: t.styled_rows()[1]["focused"] == "K界",
                   "Case folding highlights the original wide UTF-8 glyphs after the tab")
            t.save("text-search-highlight")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")

    def test_editor_word_navigation_keeps_mc_stops(self):
        root, left, _ = self.fixture()
        path = left / "words.txt"
        original = "foo_bar-baz écho\nnext word\n".encode()
        path.write_bytes(original)
        with self.terminal(root / "ui", ["--edit", path]) as t:
            t.wait_editor(offset=0)
            for offset in (7, 8, 12, 17, 18, 23, 27):
                t.press("C-Right")
                t.wait_editor(offset=offset)
            for offset in (23, 18, 17, 12, 8, 7, 0):
                t.press("C-Left")
                t.wait_editor(offset=offset)
            t.save("mc-word-stops")
        self.assertEqual(path.read_bytes(), original)

    def test_shift_click_preserves_rectangle_anchor_after_horizontal_scroll(self):
        root, left, _ = self.fixture()
        path = left / "rectangle.txt"
        line = "".join(f"{value:03}" for value in range(80))
        original = (line + "\n") * 3
        path.write_text(original)
        with self.terminal(root / "ui", ["--edit", path]) as t:
            t.wait_editor(offset=0)
            t.press("Right", "Right")
            t.raw(b"\x1b[25~")
            t.press(*(["Right"] * 6), "Down")
            t.raw(b"\x1b[25~")
            t.press(*(["Right"] * 102))
            t.wait_editor(mark="C", row=2, column=111)
            visible = t.text().splitlines()[2]
            first_column = line.find(visible[:20])
            self.assertGreater(first_column, 0)
            target = first_column + 7
            mouse(t, 7, 2, button=4)
            mouse(t, 7, 2, button=4, motion="m")
            t.wait_editor(mark="C", row=2, column=target + 1, selection=2 * (target - 2) + 1)
            t.save("extended-rectangle")
            t.press("F8")
            expected = ((line[:2] + line[target:] + "\n") * 2 + line + "\n").encode()
            save(t, path, expected)

    def test_selection_drag_scrolls_at_both_edges_and_clamps_at_eof(self):
        root, left, _ = self.fixture()
        path = left / "drag.txt"
        original = "".join(f"ROW_{value:03} text\n" for value in range(120)).encode()
        path.write_bytes(original)
        with self.terminal(root / "ui", ["--edit", path], height=14) as t:
            t.wait_for("ROW_000")
            mouse(t, 3, 2)
            mouse(t, 10, 13, button=32)
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_001"),
                   "Dragging below the viewport scrolls one line")
            mouse(t, 10, 5, button=32)
            t.wait_editor(row=6, column=11, mark="B")
            mouse(t, 10, 5, motion="m")
            t.save("drag-returned")
            mouse(t, 4, 5)
            mouse(t, 10, 0, button=32)
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_000"),
                   "Dragging above the viewport scrolls upward")
            t.wait_editor(row=1, column=11, mark="B")
            mouse(t, 10, 0, motion="m")
            t.press("C-End")
            t.wait_editor(offset=len(original))
            mouse(t, 3, 2)
            mouse(t, 10, 13, button=32)
            t.wait_editor(offset=len(original), row=121)
            mouse(t, 10, 13, button=32)
            mouse(t, 10, 13, motion="m")
            t.wait_editor(offset=len(original), row=121)
            t.save("drag-eof-clamp")
        self.assertEqual(path.read_bytes(), original)

    def test_copy_defaults_and_delete_buttons(self):
        root, left, right = self.fixture()
        target = left / "a.txt"
        target.write_bytes(b"keep\n")
        with self.terminal(root / "ui", [left, right]) as t:
            t.wait_for("a.txt")
            select_file(t, "a.txt")
            t.press("F5")
            t.wait_for("Source mask:")
            destination = "renamed.txt"
            t.type(destination)
            t.wait_for("> " + destination)
            self.assertNotIn(str(right) + destination, t.text())
            t.press("Tab")
            t.type("a*.txt")
            t.wait_for("> a*.txt")
            self.assertNotIn("> *a*.txt", t.text())
            t.press("F10")
            t.wait_for("Source mask:", present=False)
            t.press("F8")
            t.wait_for("Delete 1 selected item(s)?")
            click(t, "[ No ]")
            t.wait_for("Delete 1 selected item(s)?", present=False)
            self.assertEqual(target.read_bytes(), b"keep\n")
            t.press("F8")
            t.wait_for("Delete 1 selected item(s)?")
            t.press("Tab", "Space")
            t.wait_for("Delete 1 selected item(s)?", present=False)
            t.press("F8")
            t.wait_for("Delete 1 selected item(s)?")
            t.press("F10")
            t.wait_for("Delete 1 selected item(s)?", present=False)
            self.assertEqual(target.read_bytes(), b"keep\n")
            self.assertFalse((right / "renamed.txt").exists())

    def test_new_path_save_as_overwrite_line_endings_and_long_names(self):
        root, left, _ = self.fixture()
        directory = left
        for index in range(4):
            directory /= f"long-common-directory-{index}-" + "prefix" * 5
        directory.mkdir(parents=True)
        path = directory / "distinguishing-saved-document.txt"
        with self.terminal(root / "ui", ["--edit", path]) as t:
            t.wait_editor(bytes=0, dirty=False)
            self.assertIn("saved-document.txt", t.text().splitlines()[0])
            t.type("one")
            t.press("Enter")
            t.type("two")
            t.press("Enter")
            t.wait_editor(dirty=True, row=3)
            t.press("F10")
            t.wait_for("Save before close?")
            self.assertIn("distinguishing-saved-document.txt", t.text())
            click(t, "[ Cancel ]")
            t.wait_for("Save before close?", present=False)
            key_click(t, 2, "Save")
            t.wait_for("Save file")
            click(t, "[ Save ]")
            t.wait_file(path, b"one\ntwo\n")
            for name, ending, expected in (
                ("lf.txt", 1, b"one\ntwo\n"),
                ("crlf.txt", 2, b"one\r\ntwo\r\n"),
                ("cr.txt", 3, b"one\rtwo\r"),
            ):
                t.press("F12")
                t.wait_for("Line endings: Unchanged")
                t.type(name)
                t.wait_for("> " + name)
                t.press("Tab", *(["Right"] * ending))
                t.press("Enter")
                t.wait_for("Line endings:", present=False)
                t.wait_file(directory / name, expected)
            existing = directory / "existing.txt"
            existing.write_bytes(b"untouched")
            t.press("F12")
            t.wait_for("Line endings: Unchanged")
            t.type("existing.txt")
            t.press("Enter")
            t.wait_for("Replace existing file?")
            t.press("Space")
            t.wait_for("Replace existing file?", present=False)
            self.assertEqual(existing.read_bytes(), b"untouched")
            t.press("Enter")
            t.wait_for("Replace existing file?")
            click(t, "[ Replace ]")
            t.wait_file(existing, b"one\rtwo\r")
            t.wait_for("Replace existing file?", present=False)
            t.resize(40, 12)
            t.wait_for("existing.txt")
            t.press("F12")
            t.wait_for("[ Cancel ]")
            click(t, "[ Cancel ]")
            t.wait_for("Line endings:", present=False)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")

        with self.terminal(root / "viewer", ["--view", path]) as t:
            t.wait_for("one")
            self.assertIn("distinguishing-saved-document.txt", t.text().splitlines()[0])
            mouse(t, 4, 0)
            mouse(t, 4, 0, motion="m")
            t.wait_for("Full filename")
            t.press("Home")
            t.wait_for("> " + str(path)[:40])
            t.press("End")
            t.wait_for("distinguishing-saved-document.txt")
            t.press("F10")
            t.wait_for("Full filename", present=False)
            t.press("F3")
            t.wait_file(t.exit_file, b"0\n")

    def test_editor_mouse_space_cancel_replace_and_error_acknowledgement(self):
        root, left, right = self.fixture()
        path = left / "a.txt"
        path.write_bytes(b"alpha beta\n")
        with self.terminal(root / "ui", [left, right]) as t:
            t.wait_for("a.txt")
            select_file(t, "a.txt")
            t.press("F4")
            t.wait_editor(dirty=False)
            t.type("X")
            t.wait_editor(dirty=True)
            t.press("F10")
            t.wait_for("Save before close?")
            click(t, "[ Cancel ]")
            t.wait_for("Save before close?", present=False)
            t.wait_editor(dirty=True)
            t.press("F10")
            t.wait_for("Save before close?")
            t.press("BTab", "Space")
            t.wait_for("Save before close?", present=False)
            t.press("C-u")
            t.wait_editor(dirty=True)
            t.press("F4")
            t.wait_for("Replace with:")
            t.type("alpha")
            t.press("Tab")
            t.type("omega")
            t.press("Enter")
            t.wait_for("[ Replace ]")
            click(t, "[ Cancel ]")
            t.wait_for("[ Replace ]", present=False)
            t.press("F4")
            t.wait_for("Replace with:")
            t.press("Tab")
            t.type("omega")
            t.press("Enter")
            t.wait_for("[ Replace ]")
            t.press("BTab", "Space")
            t.wait_for("[ Replace ]", present=False)
            t.press("F4")
            t.wait_for("Replace with:")
            t.press("Tab")
            t.type("omega")
            t.press("Enter")
            t.wait_for("[ Replace ]")
            click(t, "[ One ]")
            t.wait_editor(dirty=True)
            t.press("F7")
            t.wait_for("Search for:")
            t.press("C-u")
            t.type("[")
            t.press("Tab", "Right", "Enter")
            t.wait_for("Editor error")
            click(t, "[ OK ]")
            t.wait_for("Editor error", present=False)
            key_click(t, 2, "Save")
            t.confirm_editor_save()
            t.wait_file(path, b"omega beta\n")
            t.press("F10")
            t.wait_for("Name")

    def test_short_search_replace_forms_keep_buttons_and_scroll_options(self):
        root, left, _ = self.fixture()
        path = left / "short.txt"
        path.write_text("alpha beta\n")
        with self.terminal(root / "ui", ["--edit", path], 40, 12) as t:
            t.wait_for("short.txt")
            for key, title in (("F7", "Search"), ("F4", "Replace")):
                t.press(key)
                t.wait_for("Search for:")
                t.wait_for("[ OK ]")
                t.wait_for("[ Cancel ]")
                t.wait_for("Wheel/PgUp/PgDn")
                mouse(t, 20, 4, 65)
                mouse(t, 20, 4, 65)
                mouse(t, 20, 4, 65)
                t.wait_for("Whole words")
                self.assertIn("[ Cancel ]", t.text())
                t.save("short-" + title.lower())
                click(t, "[ Cancel ]")
                t.wait_for("Search for:", present=False)

    def test_short_operation_forms_keep_actions_and_scroll_options(self):
        root, left, right = self.fixture()
        path = left / "source.txt"
        path.write_bytes(b"unchanged\n")
        with self.terminal(root / "ui", [left, right], 40, 12) as t:
            t.wait_for("Name")
            for key in ("F5", "F6"):
                t.press("Home", "Down", key)
                t.wait_for("[ OK ]")
                t.wait_for("[ Cancel ]")
                t.wait_for(("Copy: " if key == "F5" else "Move / Rename: ") + path.name)
                t.wait_for("Wheel/PgUp/PgDn")
                mouse(t, 20, 4, 65)
                mouse(t, 20, 4, 65)
                t.wait_for("Run in background")
                click(t, "Run in background")
                t.wait_for("[x] Run in background")
                t.press("PgUp", "PgUp")
                t.wait_for("To:")
                t.save("short-" + key.lower())
                click(t, "[ Cancel ]")
                t.wait_for("Operation cancelled")
            self.assertEqual(path.read_bytes(), b"unchanged\n")
            self.assertEqual(list(right.iterdir()), [])

    def test_short_main_menu_keeps_selected_action_visible_and_clickable(self):
        root, left, right = self.fixture()
        with self.terminal(root / "ui", [left, right], 80, 12) as t:
            t.wait_for("Name")
            click(t, "Options", row=0)
            t.wait_for("Configuration...")
            t.press("Up")
            t.wait_for("Save setup")
            t.wait(lambda: focused(t, "Save setup"), "Last menu action is visible and focused")
            t.save("visible-save-setup")
            click(t, "Save setup")
            t.wait_for("Setup saved")
            self.assertTrue((t.root / "config" / "mcpp" / "config").is_file())

    def test_editor_path_completion_uses_edited_file_parent(self):
        root, left, right = self.fixture()
        outside = root / "outside"
        outside.mkdir()
        path, sibling = outside / "original.txt", outside / "edit-sibling.txt"
        path.write_text("original\n")
        sibling.write_text("OUTSIDE SIBLING\n")
        (left / "edit-pane-only.txt").write_text("wrong base\n")
        with self.terminal(root / "ui", ["--edit", path, left, right]) as t:
            t.wait_editor(dirty=False)
            t.press("F12")
            t.wait_for("Line endings:")
            t.type("edit-s")
            t.raw(b"\x1b\t")
            t.wait_for("> edit-sibling.txt")
            self.assertNotIn("edit-pane-only", t.text())
            t.press("F10")
            t.wait_for("Line endings:", present=False)
            menu(t, "File", "Open file...")
            t.type("edit-s")
            t.raw(b"\x1b\t")
            t.wait_for("> edit-sibling.txt")
            t.press("Enter")
            t.wait_editor(bytes=len(b"OUTSIDE SIBLING\n"))
            t.wait_for("OUTSIDE SIBLING")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")

    def test_editor_selection_return_and_tab_variants(self):
        root, left, _ = self.fixture()
        path = left / "selection.txt"
        path.write_bytes(b"alpha beta\nsecond\nthird\n")
        with self.terminal(root / "selection", ["--edit", path]) as t:
            t.wait_editor(row=1, column=1)
            t.raw(b"\x1b[1;6C")
            t.wait_editor(selection=6)
            t.raw(b"\x1b[1;6D")
            t.wait_editor(selection=0, column=1)
            t.raw(b"\x1b[1;6C")
            t.wait_editor(selection=6)
            t.raw(b"\x1b[1;6F")
            t.wait_editor(selection=path.stat().st_size)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        path = left / "tabs.txt"
        with self.terminal(root / "variants", ["--edit", path]) as t:
            t.wait_editor(bytes=0)
            t.press("BTab")
            t.wait_editor(bytes=4, column=5)
            t.press("BSpace")
            t.wait_editor(bytes=0, column=1)
            t.press("BTab")
            t.raw(b"\x1b[9;5u\x1b[9;6u")
            t.type("alpha")
            t.raw(b"\x1b[13;2u")
            t.wait_editor(row=2, column=1)
            t.type("beta")
            t.raw(b"\x1b[13;6u\x1b[13;5u")
            t.wait_editor(row=4, column=1)
            t.type("gamma")
            save(t, path, b"\t    alpha\nbeta\n\ngamma")
        path = left / "columns.txt"
        path.write_text("abcdef\n" * 45)
        with self.terminal(root / "columns", ["--edit", path]) as t:
            t.wait_editor(row=1, column=1)
            t.raw(b"\x1b[6;3~\x1b[1;3C")
            t.wait_editor(mark="C", row=28, selection=55)
            t.press("C-End", "End")
            t.wait_editor(row=46, column=1)
            t.raw(b"\x1b[1;6H")
            t.wait_editor(row=1, column=1, selection=315, mark="B")
            t.press("M-l")
            t.wait_for("Go to line")
            t.type("15")
            t.press("Enter")
            t.wait_for("Go to line", present=False)
            t.wait_editor(row=15, column=1)
            t.raw(b"\x1b[6;6~")
            t.wait_editor(row=28, selection=91)
            t.raw(b"\x1b[5;6~")
            t.wait_editor(row=1, selection=98)
            t.press("M-l")
            t.wait_for("Go to line")
            t.type("15")
            t.press("Enter")
            t.wait_for("Go to line", present=False)
            t.wait_editor(row=15)
            t.raw(b"\x1b[1;6A")
            t.wait_editor(row=14, selection=7)
            t.raw(b"\x1b[1;6B")
            t.wait_editor(row=15, selection=0)

    def test_find_list_view_edit_and_return_to_same_result(self):
        root, left, right = self.fixture()
        (left / "nested").mkdir()
        for index in range(35):
            (left / "nested" / f"a{index:02}.txt").write_text(f"result {index}\n")
        target = left / "nested/b-target.txt"
        target.write_bytes(b"RESULT TARGET\n")
        (left / "mark.txt").write_bytes(b"mark")
        with self.terminal(root / "ui", ["--printwd", root / "cwd", left, right]) as t:
            t.wait_for("mark.txt")
            select_file(t, "mark.txt")
            t.press("Insert")
            t.wait_for("1 selected,")
            t.raw(b"\x1b?")
            t.wait_for("Recursive name pattern")
            t.type("[ab]*.txt")
            t.press("Enter")
            t.wait_for("Search results")
            selected_result(t, "a00.txt")
            t.press("PgDn")
            t.wait(lambda: not focused(t, "a00.txt"), "Results PageDown changes selection")
            t.press("End")
            selected_result(t, "b-target.txt")
            click(t, target.name)
            selected_result(t, "b-target.txt")
            key_click(t, 3, "View")
            t.wait_for("RESULT TARGET")
            t.type("q")
            t.wait_for("Search results")
            selected_result(t, "b-target.txt")
            t.press("F4")
            t.wait_editor(bytes=len(b"RESULT TARGET\n"))
            t.type("X")
            t.press("Escape")
            t.wait_for("Save before close?")
            click(t, "[ Cancel ]")
            t.wait_for("Save before close?", present=False)
            t.wait_editor(dirty=True)
            t.press("Escape")
            t.wait_for("Save before close?")
            click(t, "[ No ]")
            t.wait_for("Search results")
            selected_result(t, "b-target.txt")
            self.assertEqual(target.read_bytes(), b"RESULT TARGET\n")
            t.press("F4")
            t.wait_editor(dirty=False)
            t.press("Home")
            t.type("X")
            save(t, target, b"XRESULT TARGET\n")
            t.press("F10")
            t.wait_for("Search results")
            selected_result(t, "b-target.txt")
            t.press("Home")
            selected_result(t, "a00.txt")
            mouse(t, 20, 8, 65)
            t.wait(lambda: not focused(t, "a00.txt"), "Results wheel changes selection")
            t.press("Escape")
            t.wait_for("Search results", present=False)
            t.wait_for("1 selected,")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text().strip(), str(left))

    def test_buffer_history_pointer_and_escape_quits_all_buffers(self):
        root, left, _ = self.fixture()
        first, second = left / "first.txt", left / "second.txt"
        first.write_bytes(b"first\n")
        second.write_bytes(b"second\n")
        with self.terminal(root / "ui", ["--edit", first]) as t:
            t.wait_editor(dirty=False)
            t.type("X")
            menu(t, "File", "Open file...")
            t.wait_for("Open file")
            t.type(str(second))
            t.press("Enter")
            t.wait_editor(bytes=7, dirty=False)
            t.type("Y")
            menu(t, "Window", "List...")
            t.wait_for("Open files")
            click(t, first.name)
            selected_result(t, "first.txt")
            t.press("Enter")
            t.wait_editor(bytes=7, dirty=True)
            t.raw(b"\x1bE")
            t.wait_for("History")
            t.press("End")
            selected_result(t, "second.txt")
            click(t, second.name, twice=True)
            t.wait_for("History", present=False)
            t.wait_editor(bytes=8, dirty=True)
            t.press("Escape")
            t.wait_for("Save before close?")
            self.assertIn("second.txt", t.text())
            click(t, "[ No ]")
            t.wait(lambda: "first.txt" in t.text() and "Save before close?" in t.text(),
                   "Escape also checks the inactive dirty buffer")
            click(t, "[ Cancel ]")
            t.wait_for("Save before close?", present=False)
            menu(t, "Window", "List...")
            t.wait_for("Open files")
            t.press("End")
            selected_result(t, "first.txt")
            t.press("PgUp")
            selected_result(t, "second.txt")
            t.press("F10")
            t.wait_for("Open files", present=False)
            t.press("Escape")
            t.wait_for("Save before close?")
            click(t, "[ No ]")
            t.wait(lambda: "first.txt" in t.text() and "Save before close?" in t.text(),
                   "Cancelled Quit retained both buffers")
            click(t, "[ No ]")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual(first.read_bytes(), b"first\n")
        self.assertEqual(second.read_bytes(), b"second\n")

    def test_marked_chmod_chown_never_target_parent(self):
        root, left, right = self.fixture()
        paths = [left / "a.txt", left / "b.txt"]
        for path in paths:
            path.write_bytes(b"metadata")
        parent_mode = left.parent.stat().st_mode
        with self.terminal(root / "ui", [left, right]) as t:
            t.wait_for("a.txt")
            t.press("C-a", "Home", "C-x")
            t.type("c")
            t.wait_for("Octal mode:")
            t.type("0600")
            t.press("Enter")
            t.wait(lambda: all(path.stat().st_mode & 0o777 == 0o600 for path in paths),
                   "Chmod changes every stable marked target")
            self.assertEqual(left.parent.stat().st_mode, parent_mode)
            before = [path.stat().st_ctime_ns for path in paths]
            parent_ctime = left.parent.stat().st_ctime_ns
            t.press("C-x")
            t.type("o")
            t.wait_for("Owner:group:")
            t.type(
                pwd.getpwuid(os.getuid()).pw_name + ":" +
                grp.getgrgid(os.getgid()).gr_name
            )
            t.press("Enter")
            t.wait(lambda: all(path.stat().st_ctime_ns > old for path, old in zip(paths, before)),
                   "Chown visits every marked file even with cursor on ..")
            self.assertEqual(left.parent.stat().st_ctime_ns, parent_ctime)
            t.type("-")
            t.wait_for("Unselect")
            t.press("Enter")
            t.wait_for("Selection updated")
            t.press("Home", "C-x")
            t.type("c")
            t.wait_for("no files selected")
            self.assertNotIn("Octal mode:", t.text())
            self.assertEqual(left.parent.stat().st_mode, parent_mode)

    def test_hotlist_pointer_wheel_pages_and_activation(self):
        root, left, right = self.fixture()
        paths = [left / f"hot-{index:02}" for index in range(30)]
        for path in paths:
            path.mkdir()
        config = root / "ui/config/mcpp"
        config.mkdir(parents=True)
        (config / "config").write_text("".join(f"bookmark={path}\n" for path in paths))
        with self.terminal(root / "ui", ["--printwd", root / "cwd", left, right]) as t:
            t.wait_for("Name")
            t.raw(b"\x1c")
            t.wait_for("Directory hotlist")
            selected_result(t, "hot-00")
            t.press("PgDn")
            t.wait(lambda: not focused(t, "hot-00"), "Hotlist PageDown moves selection")
            t.press("End")
            selected_result(t, "hot-29")
            t.press("DC")
            selected_result(t, "hot-28")
            t.press("Home")
            selected_result(t, "hot-00")
            mouse(t, 25, 8, 65)
            selected_result(t, "hot-03")
            click(t, "hot-04", rightmost=True)
            selected_result(t, "hot-04")
            click(t, "hot-05", twice=True, rightmost=True)
            t.wait_for("Directory hotlist", present=False)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text().strip(), str(paths[5]))

    def test_hardlink_choices_and_read_only_save_as(self):
        root, left, _ = self.fixture()
        for choice in ("Detach", "Preserve links"):
            path = left / (choice.replace(" ", "-") + ".txt")
            alias = path.with_suffix(".alias")
            path.write_bytes(b"original\n")
            os.link(path, alias)
            with self.terminal(root / choice.replace(" ", "-"), ["--edit", path]) as t:
                t.wait_editor(dirty=False)
                t.type("X")
                t.press("F2")
                t.wait_for("Save file")
                t.press("Enter")
                t.wait_for("File has multiple hardlinks")
                t.press("F10")
                t.wait_for("File has multiple hardlinks", present=False)
                t.wait_editor(dirty=True)
                self.assertEqual(alias.read_bytes(), b"original\n")
                t.press("F2")
                t.wait_for("Save file")
                t.press("Enter")
                t.wait_for("File has multiple hardlinks")
                click(t, "[ " + choice + " ]")
                t.wait_editor(dirty=False)
                t.wait_file(path, b"Xoriginal\n")
                self.assertEqual(alias.read_bytes(),
                                 b"original\n" if choice == "Detach" else b"Xoriginal\n")
                self.assertEqual(path.stat().st_ino == alias.stat().st_ino,
                                 choice == "Preserve links")
        path = left / "read-only.txt"
        path.write_bytes(b"original\n")
        path.chmod(0o400)
        with self.terminal(root / "read-only", ["--edit", path]) as t:
            t.wait_editor(dirty=False)
            t.type("X")
            t.press("F2")
            t.wait_for("Save file")
            t.press("Enter")
            t.wait_for("Editor error")
            click(t, "[ OK ]")
            t.wait_editor(dirty=True)
            self.assertEqual(path.read_bytes(), b"original\n")
            t.press("F12")
            t.wait_for("Line endings:")
            t.type("writable.txt")
            t.press("Enter")
            t.wait_file(left / "writable.txt", b"Xoriginal\n")
            self.assertEqual(path.stat().st_mode & 0o777, 0o400)

    def test_save_as_linked_target_and_same_path_conversion_preserve_links(self):
        root, left, _ = self.fixture()
        original, target, alias = left / "original.txt", left / "target.txt", left / "alias.txt"
        original.write_bytes(b"one\ntwo\n")
        target.write_bytes(b"old target\n")
        os.link(target, alias)
        with self.terminal(root / "ui", ["--edit", original]) as t:
            t.wait_editor(dirty=False)
            t.press("F12")
            t.wait_for("Line endings:")
            t.type("target.txt")
            t.press("Enter")
            t.wait_for("Replace existing file?")
            click(t, "[ Replace ]")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Preserve links ]")
            t.wait_file(target, b"one\ntwo\n")
            self.assertEqual(alias.read_bytes(), b"one\ntwo\n")
            self.assertEqual(target.stat().st_ino, alias.stat().st_ino)
            t.press("F12")
            t.wait_for("Line endings: Unchanged")
            t.press("Tab", "Right", "Right", "Enter")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Preserve links ]")
            t.wait_file(target, b"one\r\ntwo\r\n")
            self.assertEqual(alias.read_bytes(), b"one\r\ntwo\r\n")
            self.assertEqual(target.stat().st_ino, alias.stat().st_ino)
            changed, changed_alias = left / "changed.txt", left / "changed-alias.txt"
            changed.write_bytes(b"old")
            os.link(changed, changed_alias)
            t.press("F12")
            t.wait_for("Line endings:")
            t.type("changed.txt")
            t.press("Enter")
            t.wait_for("Replace existing file?")
            click(t, "[ Replace ]")
            t.wait_for("File has multiple hardlinks")
            changed_alias.write_bytes(b"EXTERNAL CHANGE\n")
            click(t, "[ Preserve links ]")
            t.wait_for("Editor error")
            self.assertEqual(changed.read_bytes(), b"EXTERNAL CHANGE\n")
            self.assertEqual(changed_alias.read_bytes(), b"EXTERNAL CHANGE\n")
            t.press("C-g")
            t.wait_for("Editor error", present=False)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")

    def test_find_keeps_accessible_results_and_warns_about_unreadable_paths(self):
        root, left, right = self.fixture()
        (left / "accessible").mkdir()
        (left / "locked").mkdir()
        (left / "accessible/found.txt").write_text("accessible result")
        (left / "locked/hidden.txt").write_text("unreadable result")
        (left / "locked").chmod(0)
        with self.terminal(root / "ui", [left, right]) as t:
            t.wait_for("Name")
            t.raw(b"\x1b?")
            t.wait_for("Recursive name pattern")
            t.type("*.txt")
            t.press("Enter")
            t.wait_for("Search results")
            t.wait_for("found.txt")
            t.wait_for("1 unreadable path(s)")
            self.assertNotIn("hidden.txt", t.text())
            t.press("F3")
            t.wait_for("accessible result")
            t.press("F3")
            t.wait_for("Search results")
            t.wait_for("1 unreadable path(s)")

    def test_hardlink_close_yes_and_inactive_quit_yes(self):
        root, left, _ = self.fixture()
        clean = left / "clean.txt"
        clean.write_text("clean\n")
        for inactive in (False, True):
            path = left / ("inactive.txt" if inactive else "active.txt")
            alias = path.with_suffix(".alias")
            path.write_bytes(b"linked\n")
            os.link(path, alias)
            with self.terminal(root / path.stem, ["--edit", path]) as t:
                t.wait_editor(dirty=False)
                t.type("X")
                t.wait_editor(dirty=True)
                if inactive:
                    menu(t, "File", "Open file...")
                    t.type(str(clean))
                    t.press("Enter")
                    t.wait_editor(dirty=False)
                    t.press("F10")
                else:
                    t.press("F9", "Enter", "Down", "Down", "Enter")
                t.wait_for("Save before close?")
                self.assertIn(path.name, t.text())
                click(t, "[ Yes ]")
                t.wait_for("File has multiple hardlinks")
                click(t, "[ Preserve links ]")
                t.wait_file(t.exit_file, b"0\n")
                self.assertEqual(path.read_bytes(), b"Xlinked\n")
                self.assertEqual(alias.read_bytes(), b"Xlinked\n")
                self.assertEqual(path.stat().st_ino, alias.stat().st_ino)

    def test_exit_reports_automatic_config_save_failure(self):
        root, left, right = self.fixture()
        with self.terminal(root / "ui", [left, right], record_stderr=True) as t:
            t.wait_for("Name")
            config = t.root / "config/mcpp"
            config.mkdir(exist_ok=True)
            config.chmod(0o500)
            t.press("F10")
            t.wait_file(t.exit_file, b"1\n")
            self.assertIn("cannot save setup", t.stderr_file.read_text())

    def test_viewer_wheel_aliases_wrap_decision_hex_save_and_standalone_quit(self):
        root, left, _ = self.fixture()
        path = left / "viewer.txt"
        path.write_text("".join(f"ROW_{index:03}\n" for index in range(100)))
        with self.terminal(root / "text", ["--view", path]) as t:
            t.wait_for("ROW_000")
            mouse(t, 30, 10, 65)
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_003"), "Viewer wheel scrolls")
            t.type("j")
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_004"), "j moves down")
            t.type("u")
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_000"), "u moves half a page up")
            t.press("Space")
            t.wait(lambda: not t.text().splitlines()[1].startswith("ROW_000"), "Space pages down")
            t.press("Home")
            t.wait_for("ROW_000")
            key_click(t, 2, "UnWrap")
            t.wait_for("2Wrap")
            t.press("F7")
            t.wait_for("Enter search string:")
            t.type("row_000")
            t.press("Enter")
            t.wait_for("Enter search string:", present=False)
            t.type("n")
            t.wait_for("Continue from the beginning?")
            click(t, "[ Cancel ]")
            t.wait_for("Continue from the beginning?", present=False)
            t.type("n")
            t.wait_for("Continue from the beginning?")
            t.press("Space")
            t.wait_for("Continue from the beginning?", present=False)
            t.type("q")
            t.wait_file(t.exit_file, b"0\n")
        path = left / "bytes.bin"
        path.write_bytes(b"ABC")
        with self.terminal(root / "hex", ["--view", path]) as t:
            t.wait_for("ABC")
            t.press("F4")
            t.wait_for("41 42 43")
            t.press("F7")
            t.wait_for("Enter search string:")
            t.type("42 43")
            t.press("Enter")
            t.wait_for("Enter search string:", present=False)
            t.wait_for("@1/3")
            t.press("Home")
            key_click(t, 2, "Edit")
            t.wait_for("2View")
            t.type("5a")
            t.wait_for("5a 42 43")
            t.press("Tab")
            t.type("Y")
            t.wait_for("5a 59 43")
            t.press("F10")
            t.wait_for("Save modified bytes?")
            click(t, "[ Cancel ]")
            t.wait_for("Save modified bytes?", present=False)
            key_click(t, 6, "Save")
            t.wait_file(path, b"ZYC")
            t.type("Q")
            t.wait_for("5a 59 51")
            t.press("F10")
            t.wait_for("Save modified bytes?")
            click(t, "[ Discard ]")
            t.wait_file(t.exit_file, b"0\n")
            self.assertEqual(path.read_bytes(), b"ZYC")
        with self.terminal(root / "hex-close-save", ["--view", path]) as t:
            t.wait_for("ZYC")
            t.press("F4", "F2")
            t.wait_for("2View")
            t.type("58")
            t.wait_for("58 59 43")
            t.press("F10")
            t.wait_for("Save modified bytes?")
            t.press("Space")
            t.wait_file(t.exit_file, b"0\n")
            self.assertEqual(path.read_bytes(), b"XYC")

    def test_hex_wheel_scrolls_immediately_and_percentage_tracks_cursor(self):
        root, left, _ = self.fixture()
        path = left / "bytes.bin"
        original = bytes(range(256)) + b"final"
        path.write_bytes(original)
        with self.terminal(root / "ui", ["--view", path], 80, 24) as t:
            t.wait_for("bytes.bin")
            t.press("F4")
            t.wait_for("@0/261")
            t.wait(lambda: t.text().splitlines()[0].rstrip().endswith(" 0%"),
                   "First byte reports zero percent")
            t.press("Down")
            t.wait_for("@16/261")
            t.wait(lambda: t.text().splitlines()[0].rstrip().endswith(" 6%"),
                   "Percentage follows byte 16")
            mouse(t, 30, 8, 65)
            t.wait(lambda: t.text().splitlines()[1].startswith("00000030"),
                   "Hex wheel changes the first visible row immediately")
            t.wait_for("@64/261")
            t.save("hex-wheel-and-progress")
            mouse(t, 30, 8, 64)
            t.wait(lambda: t.text().splitlines()[1].startswith("00000000"),
                   "Hex wheel returns to the beginning")
            self.assertEqual(path.read_bytes(), original)

    def test_hex_click_places_byte_nibble_and_ascii_edit_targets(self):
        root, left, _ = self.fixture()
        path = left / "bytes.bin"
        original = bytes(range(67))
        path.write_bytes(original)
        with self.terminal(root / "ui", ["--view", path], 80, 24) as t:
            t.wait_for("bytes.bin")
            t.press("F4", "F2")
            t.wait_for("2View")
            column = t.text().splitlines()[2].index(" 10 11") + 1
            mouse(t, column, 2)
            mouse(t, column, 2, motion="m")
            t.wait_for("@16/67")
            t.type("a")
            t.wait_for("a0 11 12")
            column = t.text().splitlines()[2].index(" 11 12") + 2
            mouse(t, column, 2)
            mouse(t, column, 2, motion="m")
            t.wait_for("@17/67")
            t.type("f")
            t.wait_for("a0 1f 12")
            line = t.text().splitlines()[3]
            ascii_column = line.index("  ") + 2 + 49
            mouse(t, ascii_column, 3)
            mouse(t, ascii_column, 3, motion="m")
            t.wait_for("@32/67")
            t.type("Z")
            t.wait_for("5a 21 22")
            line = t.text().splitlines()[5]
            blank_column = line.index("  ") + 2 + 15 * 3
            mouse(t, blank_column, 5)
            mouse(t, blank_column, 5, motion="m")
            t.wait_idle()
            self.assertIn("@33/67", t.text().splitlines()[0])
            t.save("pointed-hex-edits")
            self.assertEqual(path.read_bytes(), original)
            expected = bytearray(original)
            expected[16], expected[17], expected[32] = 0xA0, 0x1F, ord("Z")
            t.press("F6")
            t.wait_file(path, bytes(expected))

    def test_hex_click_after_horizontal_crop_uses_the_visible_byte(self):
        root, left, _ = self.fixture()
        path = left / "narrow.bin"
        original = bytes(range(64, 128))
        path.write_bytes(original)
        with self.terminal(root / "ui", ["--view", path], 40, 12) as t:
            t.wait_for("narrow.bin")
            t.press("F4", "F2", "Tab", *(["Right"] * 8))
            t.wait(lambda: "H" in t.focused_text(),
                   "All eight movements reach ASCII byte H before positioning the mouse")
            column = t.text().splitlines()[1].index("@AB") + 2
            mouse(t, column, 1)
            mouse(t, column, 1, motion="m")
            t.type("Z")
            t.wait_for("@AZ")
            t.save("cropped-ascii-click")
            expected = bytearray(original)
            expected[2] = ord("Z")
            t.press("F6")
            t.wait_file(path, bytes(expected))

    def test_viewer_goto_line_percentage_decimal_and_hexadecimal(self):
        root, left, _ = self.fixture()
        path = left / "goto.txt"
        path.write_text("".join(f"ROW_{index:03}\n" for index in range(100)))
        with self.terminal(root / "ui", ["--view", path], 80, 24) as t:
            t.wait_for("ROW_000")

            def go(value, choice):
                t.press("F5")
                t.wait_for("Mode: Line number")
                if choice:
                    t.press("Tab", *(["Right"] * choice), "BTab")
                t.type(str(value))
                t.wait_for("> " + str(value))
                t.save("goto-mode-" + str(choice))
                t.press("Enter")
                t.wait_for("Mode:", present=False)

            go(20, 0)
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_019"),
                   "Go to uses one-based logical line numbers")
            go(50, 1)
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_050"),
                   "Percentage mode reaches the middle of the file")
            go(8, 2)
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_001"),
                   "Decimal offset mode uses bytes")
            go("10", 3)
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_002"),
                   "Hexadecimal mode accepts bare hex digits")
            go("0x18", 3)
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_003"),
                   "Hexadecimal mode also accepts a 0x prefix")
            go(100, 1)
            t.wait_for("ROW_099")
            self.assertNotEqual(t.text().splitlines()[1].strip(), "")
            t.press("F5", "Tab", "Right", "BTab")
            t.type("101")
            t.press("Enter")
            t.wait_for("percentage must be between 0 and 100")
            t.save("invalid-percentage")
            t.press("C-u")
            t.type("0")
            t.press("Enter")
            t.wait_for("Mode:", present=False)
            t.wait_for("ROW_000")
            t.press("F4", "F5")
            t.wait_for("Mode: Hexadecimal offset")
            t.type("10")
            t.press("Enter")
            t.wait_for("Mode:", present=False)
            t.wait_for("@16/800")
            t.save("hexadecimal-go-to")

    def test_compare_methods_and_tree_pointer_controls(self):
        root, left, right = self.fixture()
        for directory in (left, right):
            (directory / "same-directory").mkdir()
            (directory / "newer.txt").write_bytes(b"same")
            (directory / "bytes.txt").write_bytes(b"aaaa")
            os.utime(directory / "bytes.txt", (1000, 1000))
        (right / "bytes.txt").write_bytes(b"bbbb")
        os.utime(right / "bytes.txt", (1000, 1000))
        os.utime(left / "newer.txt", (2000, 2000))
        os.utime(right / "newer.txt", (1000, 1000))
        with self.terminal(root / "compare", [left, right]) as t:
            t.wait_for("newer.txt")
            t.press("C-x")
            t.type("d")
            t.wait_for("Compare directories")
            click(t, "[ Quick ]")
            t.wait_for("Compare directories", present=False)
            def marked(name, first, last):
                for row in t.styled_rows()[3:-6]:
                    column = row["text"].find(name, first, last)
                    if column >= 0 and any(start <= column and end >= column + len(name)
                                           for start, end in row["bold_spans"]):
                        return True
                return False
            self.assertTrue(marked("newer.txt", 0, 50))
            self.assertFalse(marked("newer.txt", 50, 100))
            self.assertFalse(marked("same-directory", 0, 100))
            t.press("C-x")
            t.type("d")
            t.wait_for("Compare directories")
            t.press("Down", "Space")
            t.wait_for("Compare directories", present=False)
            self.assertFalse(marked("newer.txt", 0, 100))
            t.press("C-x")
            t.type("d")
            t.wait_for("Compare directories")
            click(t, "[ Thorough ]")
            t.wait_for("Compare directories", present=False)
            self.assertTrue(marked("bytes.txt", 0, 50))
            self.assertTrue(marked("bytes.txt", 50, 100))
        for index in range(10):
            (left / f"node-{index:02}").mkdir()
        (left / "node-04/opened.txt").write_text("tree opened")
        with self.terminal(root / "tree", ["--printwd", root / "cwd", left, right]) as t:
            t.wait_for("Name")
            t.press("F9", "Enter", "Down", "Down", "Down", "Enter")
            t.wait_for("Directory tree")
            t.press("Tab")
            t.wait(lambda: t.selected_tree_row().endswith("left"), "Tree starts at the chosen pane cwd")
            t.press("Enter")
            t.wait_for("node-04")
            t.wait_for("…", present=False)
            lines = t.text().splitlines()
            row = next(index for index, line in enumerate(lines) if "node-04" in line[:50])
            click(t, "node-04", row=row)
            t.wait(lambda: t.selected_tree_row().endswith("node-04"), "Single click selects the Tree node")
            mouse(t, 20, row, 65)
            t.wait(lambda: not t.selected_tree_row().endswith("node-04"), "Tree wheel navigates")
            lines = t.text().splitlines()
            row = next(index for index, line in enumerate(lines) if "node-04" in line[:50])
            click(t, "node-04", row=row, twice=True)
            t.wait_for("opened.txt")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text().strip(), str(left / "node-04"))


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Audit UI checks require the owned, non-root disposable Docker container")
    BINARY, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    ROOT = Path(sys.argv[3] if len(sys.argv) > 3 else f"audit-ui-state-{os.getpid()}").resolve()
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
