from pathlib import Path
import gzip
import json
import os
import sys
import unittest
from unittest.mock import patch

from terminal_session import TerminalSession
from test_audit_ui import click, mouse, menu, save, select_file


class NativeCompletionUI(unittest.TestCase):
    def fixture(self):
        root = ROOT / self._testMethodName
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        return root, left, right

    def terminal(self, root, arguments, config="", width=100, height=30):
        if config:
            target = root / "ui/config/mcpp/config"
            target.parent.mkdir(parents=True)
            target.write_text(config)
        return TerminalSession(BINARY, TMUX, root / "ui", arguments, width, height, record_exit=True)

    def general(self, terminal):
        menu(terminal, "Options", "General...")
        terminal.wait_for("Word wrap line length:")

    def tree(self, terminal):
        terminal.press("F9")
        terminal.type("c")
        terminal.wait(lambda: any("Directory tree" in row and "│" in row and "┌" not in row
                                 for row in terminal.text().splitlines()), "Command menu offers Directory tree")
        row = next(index for index, line in enumerate(terminal.text().splitlines())
                   if "Directory tree" in line and "│" in line and "┌" not in line)
        click(terminal, "Directory tree", row=row)
        terminal.wait(lambda: any("Directory tree" in line and "┌" in line and
                                 line.rindex("┐") - line.index("┌") > terminal.width // 2
                                 for line in terminal.text().splitlines()),
                      "Native modal Tree opens")

    def tree_select(self, terminal, name):
        terminal.press("C-s")
        terminal.type(name)
        terminal.wait_for("Search: " + name)
        def selected():
            frame = next(line for line in terminal.text().splitlines()
                         if "Directory tree" in line and "┌" in line and line.rindex("┐") - line.index("┌") > terminal.width // 2)
            left, right = frame.index("┌") + 1, frame.rindex("┐")
            for row in terminal.styled_rows():
                value = "".join(row["text"][max(begin, left):min(end, right)]
                                for begin, end in row["spans"] if begin < right and end > left)
                if value.strip().endswith(name):
                    return True
            return False
        terminal.wait(selected,
                      "Tree search selects the requested node")
        terminal.press("Escape")
        terminal.wait_for("Search: " + name, present=False)

    def compare(self, terminal, first, second):
        select_file(terminal, first)
        terminal.press("Tab")
        select_file(terminal, second)
        terminal.press("Tab", "C-x", "C-d")
        terminal.wait_for("9Options")
        terminal.wait_for("5Merge")

    def viewer_search(self, terminal, query, kind="Normal", **options):
        terminal.press("F7")
        terminal.wait_for("Enter search string:")
        terminal.press("C-u")
        terminal.type(query)
        for line in terminal.text().splitlines():
            if kind in line and "( )" in line:
                click(terminal, kind)
                break
        for name, value in options.items():
            label = {"sensitive": "Case sensitive", "backwards": "Backwards",
                     "whole": "Whole words", "charsets": "All charsets"}[name]
            row = next(line for line in terminal.text().splitlines() if label in line)
            checked = "[x] " + label in row
            if checked != value:
                click(terminal, label)
        click(terminal, "[< OK >]")
        terminal.wait_for("Enter search string:", present=False)

    def test_general_native_controls_defaults_and_real_wrap(self):
        root, left, _ = self.fixture()
        path = left / "wrapped.txt"
        path.write_text("")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(bytes=0)
            self.general(t)
            for name in ("Wrap mode", "None", "Dynamic paragraphing", "Type writer wrap",
                         "Fake half tabs", "Backspace through tabs", "Fill tabs with spaces",
                         "Return does autoindent", "Confirm before saving", "Save file position",
                         "Visible trailing spaces", "Visible tabs", "Syntax highlighting",
                         "Cursor after inserted block", "Persistent selection", "Cursor beyond end of line",
                         "Group undo"):
                self.assertIn(name, t.text())
            self.assertNotIn("Indent spacing", t.text())
            self.assertNotIn("Show whitespace", t.text())
            click(t, "Type writer wrap")
            click(t, "Tab spacing:")
            t.press("C-u")
            t.type("6")
            click(t, "Word wrap line length:")
            t.press("C-u")
            t.type("12")
            click(t, "Confirm before saving")
            click(t, "Visible tabs")
            click(t, "[< OK >]")
            t.wait_for("Word wrap line length:", present=False)
            t.type("one two three")
            t.wait_editor(lines=2, dirty=True)
            t.press("F2")
            t.wait_file(path, b"one two\nthree")
            t.wait_editor(dirty=False)
            self.assertNotIn("Save file", t.text())
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            stored = (t.root / "config/mcpp/config").read_text()
            for setting in ("editor_tab_width=6", "editor_wrap_length=12", "editor_wrap_mode=typewriter",
                            "editor_confirm_save=false", "editor_visible_tabs=false",
                            "editor_visible_trailing_spaces=true"):
                self.assertIn(setting, stored)

    def test_general_virtual_cursor_pointer_and_undo(self):
        root, left, _ = self.fixture()
        path = left / "virtual.txt"
        path.write_text("a界\nx\n")
        with self.terminal(root, ["--edit", path],
                           "editor_cursor_beyond_eol=true\neditor_group_undo=false\n") as t:
            t.wait_editor(offset=0)
            t.press("End", "Right", "Right", "Right", "Right")
            t.wait_editor(column=8, offset=4)
            t.type("Z")
            t.wait_editor(offset=9, dirty=True)
            t.press("C-u")
            t.wait_editor(column=8, offset=4)
            t.press("Down")
            t.wait_editor(row=2, column=8)
            t.type("Y")
            save(t, path, "a界\nx      Y\n".encode())
            mouse(t, 10, 1)
            mouse(t, 10, 1, motion="m")
            t.wait_editor(row=1, column=11, offset=4)
            t.type("X")
            save(t, path, "a界       X\nx      Y\n".encode())

    def test_general_nonpersistent_selection_and_grouped_undo(self):
        root, left, _ = self.fixture()
        path = left / "selection.txt"
        path.write_text("keep\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(offset=0)
            self.general(t)
            click(t, "Persistent selection")
            click(t, "[< OK >]")
            t.wait_for("Word wrap line length:", present=False)
            menu(t, "Edit", "Mark all")
            t.type("abc")
            t.wait_editor(bytes=3)
            t.press("C-u")
            t.wait_editor(bytes=5)
            save(t, path, b"keep\n")
            t.press("End")
            t.type("XY")
            t.press("C-u")
            t.wait_editor(bytes=5)

    def test_general_dynamic_wrap_and_tabulation(self):
        root, left, _ = self.fixture()
        path = left / "dynamic.txt"
        path.write_text("one two three four five")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(offset=0)
            self.general(t)
            click(t, "Dynamic paragraphing")
            click(t, "Word wrap line length:")
            t.press("C-u")
            t.type("12")
            click(t, "[< OK >]")
            t.wait_for("Word wrap line length:", present=False)
            t.press("End")
            t.type(" six")
            t.wait_editor(lines=3)
            save(t, path, b"one two\nthree four\nfive six")
            self.general(t)
            click(t, "( ) None")
            click(t, "Backspace through tabs")
            click(t, "Fill tabs with spaces")
            click(t, "[< OK >]")
            t.wait_for("Word wrap line length:", present=False)
            t.press("C-Home", "Home", "Tab", "Tab")
            t.wait_editor(column=9)
            t.press("BSpace")
            t.wait_editor(column=1)
            save(t, path, b"one two\nthree four\nfive six")

    def test_modal_tree_hierarchy_selection_and_cancel(self):
        root, left, right = self.fixture()
        (left / "branch-A/inside").mkdir(parents=True)
        (left / "branch-B").mkdir()
        (left / "branch-C").mkdir()
        output = root / "cwd"
        with self.terminal(root, ["--printwd", output, left, right]) as t:
            t.wait_for("branch-A")
            self.tree(t)
            t.wait_for("branch-B")
            self.assertNotIn("[Current]", t.text())
            self.tree_select(t, "branch-A")
            t.press("Right")
            t.wait_for("inside")
            t.press("Right")
            t.wait(lambda: any(value.endswith("inside") for value in t.focused_text()), "Right enters the expanded child")
            t.press("Enter")
            t.wait(lambda: "/inside" in t.text().splitlines()[1][:50], "Enter accepts the exact Tree directory")
            self.tree(t)
            t.wait(lambda: any(value.endswith("inside") for value in t.focused_text()), "Reopened Tree selects the current directory")
            t.press("Home", "Escape")
            t.wait_for("Directory Tree cancelled")
            self.assertIn("/inside", t.text().splitlines()[1][:50])
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual(output.read_text().strip(), str(left / "branch-A/inside"))

    def test_modal_tree_keeps_sidebar_marks_and_persistent_cache(self):
        root, left, right = self.fixture()
        (left / "cached-A").mkdir()
        (left / "cached-B").mkdir()
        (left / "marked.txt").write_text("marked")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("marked.txt")
            select_file(t, "marked.txt")
            t.press("Insert")
            t.wait_for("1 selected,")
            t.press("F9")
            t.type("r")
            t.wait_for("Tree")
            click(t, "Tree")
            t.press("Tab")
            t.wait(lambda: t.selected_tree_row(1).endswith("right"), "The existing sidebar owns its selection")
            before = t.selected_tree_row(1)
            self.tree(t)
            t.wait_for("cached-B")
            self.tree_select(t, "cached-B")
            t.press("Escape")
            t.wait_for("Directory Tree cancelled")
            self.assertEqual(t.selected_tree_row(1), before)
            self.assertIn("1 selected,", t.text())
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            cache = (t.root / "cache/mcpp/Tree").read_bytes()
            self.assertEqual((t.root / "cache/mcpp/Tree").stat().st_mode & 0o777, 0o600)
        target = root / "next/ui/cache/mcpp/Tree"
        target.parent.mkdir(parents=True)
        target.write_bytes(cache)
        target.chmod(0o600)
        with self.terminal(root / "next", [left / "cached-A", right]) as t:
            t.wait_for("Name")
            self.tree(t)
            t.wait_for("cached-B")
            self.tree_select(t, "cached-B")
            t.press("F3")
            t.wait_for("cached-B", present=False)
            self.assertTrue((left / "cached-B").is_dir())
            self.tree_select(t, "cached-A")
            t.press("Left", "F2")
            t.wait_for("cached-B")

    def test_modal_tree_retries_an_unmatched_search_after_loading_new_children(self):
        root, left, right = self.fixture()
        target = left / "zz-loaded-target"
        output = root / "cwd"
        with self.terminal(root, ["--printwd", output, left, right]) as t:
            t.wait_for("Name")
            self.tree(t)
            t.press("C-s")
            t.type(target.name)
            t.wait_for("Search: " + target.name)
            self.assertNotIn(target.name, t.text().replace("Search: " + target.name, ""))
            target.mkdir()
            t.press("F2")
            t.wait(lambda: any(value.endswith(target.name) for value in t.focused_text()),
                   "a newly loaded matching directory becomes the pending search result")
            t.press("Enter")
            t.wait_for("Directory changed")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual(output.read_text(), str(target) + "\n")

    def test_modal_tree_file_operation_context(self):
        root, left, right = self.fixture()
        (left / "copy-branch").mkdir()
        (left / "copy-branch/data.txt").write_bytes(b"tree data")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("copy-branch")
            self.tree(t)
            t.wait_for("copy-branch")
            self.tree_select(t, "copy-branch")
            t.press("F5")
            t.wait_for("Source mask:")
            click(t, "[ Cancel ]")
            t.wait_for("Source mask:", present=False)
            t.wait(lambda: any(value.endswith("copy-branch") for value in t.focused_text()), "Cancel returns to the modal Tree")
            t.press("F5")
            t.wait_for("Source mask:")
            t.press("Enter")
            t.wait_file(right / "copy-branch/data.txt", b"tree data")
            t.wait_for("Copy complete")
            t.wait(lambda: any(value.endswith("copy-branch") for value in t.focused_text()), "Copy completion returns to the Tree")
            t.press("F6")
            t.wait_for("Source mask:")
            t.press("C-u")
            t.type(str(right / "renamed"))
            t.press("Enter")
            t.wait_file(right / "renamed/data.txt", b"tree data")
            self.assertFalse((left / "copy-branch").exists())

    def test_native_diff_alignment_hunks_merge_undo_and_save(self):
        root, left, right = self.fixture()
        first, second = left / "first.txt", right / "second.txt"
        first.write_text("same\nLEFT\ncommon\nLEFT2\nlast\n")
        original = b"same\nRIGHT\ncommon\nRIGHT2\nlast\n"
        second.write_bytes(original)
        with self.terminal(root, [left, right]) as t:
            t.wait_for("first.txt")
            self.compare(t, "first.txt", "second.txt")
            t.wait_for("Dif 2")
            rows = t.text().splitlines()
            self.assertIn("LEFT", rows[3][:50])
            self.assertIn("RIGHT", rows[3][50:])
            t.press("F5")
            t.wait_for("Dif 1")
            self.assertEqual(second.read_bytes(), original)
            t.press("C-z")
            t.wait_for("Dif 2")
            t.press("F5", "F2")
            t.wait_file(second, b"same\nLEFT\ncommon\nRIGHT2\nlast\n")
            t.press("n", "F5")
            t.wait_for("Dif 0")
            t.press("F2")
            t.wait_file(second, first.read_bytes())
            t.press("F10")
            t.wait_for("Name")

    def test_native_diff_linked_save_requires_a_real_decision(self):
        root, left, right = self.fixture()
        first, second, alias = left / "first.txt", right / "second.txt", right / "alias.txt"
        first.write_bytes(b"new\n")
        second.write_bytes(b"old\n")
        os.link(second, alias)
        with self.terminal(root, [left, right]) as t:
            t.wait_for("first.txt")
            self.compare(t, "first.txt", "second.txt")
            t.press("F5", "F2")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Cancel ]")
            t.wait_for("File has multiple hardlinks", present=False)
            self.assertEqual(second.read_bytes(), b"old\n")
            self.assertEqual(alias.read_bytes(), b"old\n")
            t.press("F2")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Preserve links ]")
            t.wait_file(alias, b"new\n")
            self.assertEqual(alias.stat().st_ino, second.stat().st_ino)

    def test_native_diff_stale_save_retains_the_merged_draft(self):
        root, left, right = self.fixture()
        first, second = left / "first.txt", right / "second.txt"
        first.write_bytes(b"merged draft\n")
        second.write_bytes(b"original\n")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("first.txt")
            self.compare(t, "first.txt", "second.txt")
            t.press("F5")
            t.wait_for("Dif 0")
            second.write_bytes(b"EXTERNAL CONTENT\n")
            t.press("F2")
            t.wait_for("Diff save")
            self.assertEqual(second.read_bytes(), b"EXTERNAL CONTENT\n")
            t.press("Enter")
            t.wait_for("9Options")
            self.assertIn("merged draft", t.text().splitlines()[2][50:])
            t.press("F10")
            t.wait_for("Save with exit?")
            click(t, "[ Cancel ]")
            t.wait_for("Save with exit?", present=False)
            t.wait_for("9Options")
            self.assertEqual(second.read_bytes(), b"EXTERNAL CONTENT\n")

    def test_native_diff_options_and_editor_return_context(self):
        root, left, right = self.fixture()
        first, second = left / "first.txt", right / "second.txt"
        first.write_bytes(b"Name\t \r\n")
        second.write_bytes(b"name \n")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("first.txt")
            self.compare(t, "first.txt", "second.txt")
            t.press("F9")
            t.wait_for("Diff Options")
            for label in ("Normal", "Fastest (Assume large files)", "Minimal (Find a smaller set of change)",
                          "Ignore case", "Ignore tab expansion", "Ignore space change",
                          "Ignore all whitespace", "Strip trailing carriage return"):
                self.assertIn(label, t.text())
            for label in ("Ignore case", "Ignore all whitespace", "Strip trailing carriage return"):
                click(t, label)
            click(t, "[< OK >]")
            t.wait_for("Dif 0")
            self.assertEqual(first.read_bytes(), b"Name\t \r\n")
            self.assertEqual(second.read_bytes(), b"name \n")
            t.press("F4")
            t.wait_editor(offset=0)
            t.type("X")
            save(t, first, b"XName\t \r\n")
            t.press("F10")
            t.wait_for("9Options")
            t.wait_for("Dif 1")
            self.assertIn("XName", t.text().splitlines()[2][:50])

    def test_native_diff_codepage_preserves_original_file_bytes(self):
        root, left, right = self.fixture()
        first, second = left / "first.txt", right / "second.txt"
        first.write_bytes(b"caf\xe9\nLEFT\n")
        second.write_bytes(b"caf\xe9\nRIGHT\n")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("first.txt")
            self.compare(t, "first.txt", "second.txt")
            t.press("M-e")
            t.wait_for("Choose codepage")
            t.type("1")
            t.wait_for("Choose codepage", present=False)
            self.assertTrue(any("café" in row[:50] and "café" in row[50:] for row in t.text().splitlines()))
            self.assertEqual(second.read_bytes(), b"caf\xe9\nRIGHT\n")
            t.press("F5", "F2")
            t.wait_file(second, b"caf\xe9\nLEFT\n")

    def test_viewer_raw_parse_and_readonly_decompressed_hex(self):
        root, left, _ = self.fixture()
        path = left / "data.gz"
        encoded = gzip.compress(b"PARSED PAYLOAD\n")
        path.write_bytes(encoded)
        with self.terminal(root, ["--view", path]) as t:
            t.wait_for("PARSED PAYLOAD")
            self.assertIn("8Raw", t.text())
            t.press("F8")
            t.wait_for("8Parse")
            t.press("F4")
            t.wait_for("1f 8b")
            t.press("F8")
            t.wait_for("8Raw")
            t.wait_for("PARSED PAYLOAD")
            t.press("F2")
            t.wait_for("parsed data cannot be hex-edited")
            t.press("Enter")
            t.wait_for("parsed data cannot be hex-edited", present=False)
            self.assertEqual(path.read_bytes(), encoded)

    def test_viewer_nroff_styles_and_search_modes(self):
        root, left, _ = self.fixture()
        path = left / "nroff.txt"
        original = b"B\bBo\bol\bld\bd _\bU_\bN_\bD_\bE_\bR\ncatalog cat\nAlpha42\nalpha17\n"
        path.write_bytes(original)
        with self.terminal(root, ["--view", path]) as t:
            t.wait_for("9Format")
            t.press("F9")
            t.wait_for("9Unform")
            t.wait_for("Bold UNDER")
            self.assertTrue(any(row["text"].startswith("Bold UNDER") and row["bold_spans"] for row in t.styled_rows()))
            self.viewer_search(t, "alpha[0-9]+", "Regular expression", sensitive=False, backwards=False)
            t.wait(lambda: any("Alpha42" in text for text in t.focused_text()), "Regex finds the first formatted hit")
            t.press("n")
            t.wait(lambda: any("alpha17" in text for text in t.focused_text()), "Repeat finds the next regex hit")
            t.press("C-r")
            t.wait(lambda: any("Alpha42" in text for text in t.focused_text()), "Backwards repeat finds the preceding hit")
            t.press("Home")
            self.viewer_search(t, "cat", "Normal", backwards=False, whole=True)
            t.wait(lambda: any(text.strip() == "cat" for text in t.focused_text()), "Whole words does not select catalog")
            self.assertEqual(path.read_bytes(), original)

    def test_viewer_codepages_and_all_charsets_do_not_convert_the_file(self):
        root, left, _ = self.fixture()
        path = left / "legacy.txt"
        original = b"caf\xe9\n"
        path.write_bytes(original)
        with self.terminal(root, ["--view", path]) as t:
            t.wait_for("9Format")
            self.viewer_search(t, "café", "Normal", charsets=True)
            self.assertNotIn("Search string not found", t.text())
            t.press("M-e")
            t.wait_for("Choose codepage")
            t.type("1")
            t.wait_for("Choose codepage", present=False)
            t.wait_for("café")
            self.assertEqual(path.read_bytes(), original)

    def test_viewer_adjacent_files_check_dirty_bytes_before_switching(self):
        root, left, _ = self.fixture()
        first, second = left / "a.bin", left / "b.bin"
        first.write_bytes(b"ABCD")
        second.write_bytes(b"EFGH")
        with self.terminal(root, ["--view", first]) as t:
            t.wait_for("ABCD")
            t.press("F4", "F2")
            t.type("5a")
            t.wait_for("5a 42 43 44")
            t.press("C-f")
            t.wait_for("Save modified bytes?")
            click(t, "[ Cancel ]")
            t.wait_for("Save modified bytes?", present=False)
            self.assertEqual(first.read_bytes(), b"ABCD")
            t.press("C-f")
            t.wait_for("Save modified bytes?")
            t.press("Enter")
            t.wait_file(first, b"ZBCD")
            t.wait_for("EFGH")
            t.press("C-b")
            t.wait_for("ZBCD")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual(second.read_bytes(), b"EFGH")

    def test_viewer_bookmarks_ruler_and_quick_view_goto(self):
        root, left, right = self.fixture()
        path = left / "rows.txt"
        path.write_text("".join(f"ROW_{index:03}\n" for index in range(70)))
        with self.terminal(root, ["--view", path]) as t:
            t.wait_for("ROW_000")
            t.press("F5")
            t.wait_for("Mode:")
            t.press("C-u")
            t.type("25")
            t.press("Enter")
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_024"), "Go to selects the bookmark's line")
            t.press("3", "m", "Home", "r")
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_024"), "Selected bookmark restores its exact line")
            t.press("M-r")
            t.wait(lambda: "----+" in t.text().splitlines()[1], "The native ruler appears above the content")
        with self.terminal(root / "quick", [left, right]) as t:
            t.wait_for("rows.txt")
            select_file(t, "rows.txt")
            t.press("C-x")
            t.type("q")
            t.wait_for("Quick view:")
            t.press("Tab", "F5")
            t.wait_for("Mode:")
            t.press("C-u")
            t.type("25")
            t.press("Enter")
            t.wait_for("ROW_024")
            self.assertEqual(path.read_text().splitlines()[24], "ROW_024")

    def test_editor_native_mail_argv_stdin_and_failed_retry(self):
        root, left, _ = self.fixture()
        path = left / "mail.txt"
        path.write_bytes(b"mail body\n")
        tools = root / "tools"
        tools.mkdir()
        executable = tools / "mail"
        arguments, body = root / "mail-arguments.json", root / "mail-body.bin"
        executable.write_text("#!/usr/bin/python3\nimport json,sys\nfrom pathlib import Path\n"
                              f"Path({str(arguments)!r}).write_text(json.dumps(sys.argv[1:]))\n"
                              f"Path({str(body)!r}).write_bytes(sys.stdin.buffer.read())\n")
        executable.chmod(0o700)
        with patch.dict(os.environ, {"PATH": str(tools) + ":" + os.environ["PATH"]}):
            with self.terminal(root, ["--edit", path]) as t:
                t.wait_editor(offset=0)
                t.type("X")
                menu(t, "Command", "Mail...")
                t.wait_for("Copies to")
                self.assertNotIn("Mailer command:", t.text())
                t.type("alice@example.invalid")
                t.press("Tab")
                t.type("subject; literal")
                t.press("Tab")
                t.type("copy@example.invalid")
                t.press("Enter")
                t.wait_for("Mail command completed.")
                self.assertEqual(body.read_bytes(), b"Xmail body\n")
                self.assertEqual(json.loads(arguments.read_text()),
                                 ["-s", "subject; literal", "-c", "copy@example.invalid", "--", "alice@example.invalid"])
                self.assertEqual(path.read_bytes(), b"mail body\n")
                t.press("Enter")
                t.wait_editor(dirty=True)
                executable.write_text("#!/bin/sh\nexit 1\n")
                t.press("M-m")
                t.wait_for("Copies to")
                t.press("Enter")
                t.wait_for("command exited 1")
                self.assertEqual(path.read_bytes(), b"mail body\n")
                click(t, "[ Cancel ]")
                t.wait_editor(dirty=True)


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Native local checks require the owned non-root disposable container")
    BINARY, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    ROOT = Path(sys.argv[3]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
