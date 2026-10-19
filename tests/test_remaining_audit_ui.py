from pathlib import Path
import os
import sys
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click, mouse, save, menu, select_file


class RemainingAuditUI(unittest.TestCase):
    def fixture(self, suffix=""):
        root = ROOT / self._testMethodName / suffix
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        return root, left, right

    def terminal(self, root, arguments, width=100, height=30):
        return TerminalSession(BINARY, TMUX, root / "ui", arguments, width, height,
                               record_exit=True)

    def search(self, terminal, query, editor=False):
        terminal.press("F7")
        terminal.wait_for("Case sensitive" if editor else "Enter search string:")
        terminal.press("C-u")
        terminal.type(query)
        terminal.press("Enter")
        terminal.wait_for("Case sensitive" if editor else "Enter search string:", present=False)

    def main_menu(self, terminal, category, item):
        terminal.press("F9")
        terminal.type(category)
        terminal.wait_for(item)
        click(terminal, item)

    def test_linked_hex_save_cancel_preserve_and_close_detach(self):
        root, left, _ = self.fixture()
        path, alias = left / "data.bin", left / "alias.bin"
        path.write_bytes(b"ABCD")
        os.link(path, alias)
        with self.terminal(root, ["--view", path]) as t:
            t.wait_for("ABCD")
            t.press("F4", "F2")
            t.wait_for("2View")
            t.type("5a")
            t.wait_for("5a 42 43 44")
            t.press("F6")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Cancel ]")
            t.wait_for("File has multiple hardlinks", present=False)
            self.assertEqual(path.read_bytes(), b"ABCD")
            self.assertEqual(alias.read_bytes(), b"ABCD")
            t.press("F6")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Preserve links ]")
            t.wait_file(alias, b"ZBCD")
            self.assertEqual(path.stat().st_ino, alias.stat().st_ino)
            t.type("59")
            t.wait_for("5a 59 43 44")
            t.press("F10")
            t.wait_for("Save modified bytes?")
            t.press("Enter")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Detach ]")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual(path.read_bytes(), b"ZYCD")
        self.assertEqual(alias.read_bytes(), b"ZBCD")
        self.assertNotEqual(path.stat().st_ino, alias.stat().st_ino)

    def test_linked_hex_failure_retains_draft(self):
        root, left, _ = self.fixture()
        path, alias = left / "data.bin", left / "alias.bin"
        path.write_bytes(b"ABCD")
        os.link(path, alias)
        with self.terminal(root, ["--view", path]) as t:
            t.wait_for("ABCD")
            t.press("F4", "F2")
            t.wait_for("2View")
            t.type("5a")
            t.wait_for("5a 42 43 44")
            path.chmod(0o400)
            t.press("F6")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Preserve links ]")
            t.wait_for("not writable")
            self.assertEqual(path.read_bytes(), b"ABCD")
            self.assertEqual(alias.read_bytes(), b"ABCD")
            t.press("Enter")
            path.chmod(0o600)
            t.press("F6")
            t.wait_for("File has multiple hardlinks")
            click(t, "[ Preserve links ]")
            t.wait_for("destination changed")
            self.assertEqual(alias.read_bytes(), b"ABCD")
            t.press("Enter")
            t.wait_for("destination changed", present=False)
            t.wait_for("5a 42 43 44")
            t.press("F10")
            t.wait_for("Save modified bytes?")
            click(t, "[ Discard ]")
            t.wait_file(t.exit_file, b"0\n")

    def test_hex_search_uses_cursor_and_failed_wrap_keeps_view(self):
        root, left, _ = self.fixture()
        path = left / "data.bin"
        data = bytearray(4096)
        for offset in (32, 160, 480):
            data[offset:offset + 2] = b"\xaa\xbb"
        path.write_bytes(data)
        with self.terminal(root, ["--view", path], height=24) as t:
            t.wait_for("data.bin")
            t.press("F4", "Home", *(["Down"] * 12))
            t.wait_for("@192/4096")
            self.search(t, "AA BB")
            t.wait_for("@480/4096")
            t.press("C-Home", *(["Down"] * 14))
            t.wait_for("@224/4096")
            t.type("n")
            t.wait_for("@480/4096")
            t.press("F5")
            t.wait_for("Go to")
            t.press("C-u")
            t.type("0xfff")
            t.press("Enter")
            t.wait_for("@4095/4096")
            before = t.text().splitlines()[1]
            self.search(t, "FF FF")
            t.wait_for("Continue from the beginning?")
            t.press("Enter")
            t.wait_for("Search string not found")
            t.press("Enter")
            t.wait_for("Search string not found", present=False)
            self.assertEqual(t.text().splitlines()[1], before)
            self.assertIn("@4095/4096", t.text().splitlines()[0])
            self.assertTrue(t.text().splitlines()[0].rstrip().endswith("100%"))
            t.save("failed-wrap-retains-eof")

    def test_text_repeat_uses_manually_scrolled_region(self):
        root, left, _ = self.fixture()
        path = left / "rows.txt"
        path.write_text("".join(f"ROW_{i:03}" + (" HIT" if i in (0, 50, 100) else "") + "\n"
                                for i in range(120)))
        with self.terminal(root, ["--view", path], height=24) as t:
            t.wait_for("ROW_000")
            self.search(t, "HIT")
            t.press(*(["Down"] * 60))
            t.wait(lambda: t.text().splitlines()[1].startswith("ROW_060"),
                   "Manual scrolling advances beyond the middle hit")
            t.type("n")
            t.wait_for("ROW_100 HIT")
            self.assertNotIn("ROW_050", t.text().splitlines()[1])

    def test_text_eof_scroll_keeps_content_visible(self):
        for long_file in (False, True):
            root, left, _ = self.fixture(str(long_file))
            path = left / "rows.txt"
            path.write_text("".join(f"ROW_{i:03}\n" for i in range(120)) + "LAST\n"
                            if long_file else "ONE\nTWO\nTHREE\n")
            with self.terminal(root, ["--view", path], height=24) as t:
                t.wait_for("ROW_000" if long_file else "ONE")
                if long_file:
                    t.press("End")
                    t.wait_for("LAST")
                before = t.text().splitlines()[1:-1]
                t.press(*(["Down"] * 40))
                t.wait_idle()
                self.assertEqual(t.text().splitlines()[1:-1], before)

    def test_far_text_match_is_revealed_in_both_wrap_modes(self):
        for wrapped in (True, False):
            root, left, _ = self.fixture(str(wrapped))
            path = left / "long-line.txt"
            path.write_text("BEGIN|" + "x" * 6500 + "NEEDLE|" + "y" * 1500 + "\nEND\n")
            with self.terminal(root, ["--view", path], height=24) as t:
                t.wait_for("BEGIN|")
                if not wrapped:
                    t.press("F2")
                    t.wait_for("2Wrap")
                self.search(t, "NEEDLE")
                t.wait_for("NEEDLE")
                t.save("far-hit-revealed")

    def test_search_preserves_linear_and_column_blocks(self):
        for column in (False, True):
            root, left, _ = self.fixture(str(column))
            path = left / "blocks.txt"
            path.write_text("KEEP xx NEEDLE\nkeep yy other\n" if column else
                            "KEEP and NEEDLE and TAIL\n")
            with self.terminal(root, ["--edit", path]) as t:
                t.wait_editor(offset=0)
                if column:
                    t.raw(b"\x1b[25~")
                else:
                    t.press("F3")
                t.press(*(["Right"] * 4))
                if column:
                    t.press("Down")
                    t.raw(b"\x1b[25~")
                else:
                    t.press("F3")
                t.press("C-Home", "Home")
                self.search(t, "NEEDLE", editor=True)
                t.wait_editor(mark="C" if column else "B", selection=9 if column else 4)
                t.save("search-preserves-user-block")
                t.press("F8")
                expected = b" xx NEEDLE\n yy other\n" if column else b" and NEEDLE and TAIL\n"
                save(t, path, expected)

    def test_editor_mnemonic_and_main_menu_outside_click(self):
        root, left, right = self.fixture()
        path = left / "editor.txt"
        path.write_text("EDITOR FIXTURE\n")
        with self.terminal(root / "editor", ["--edit", path]) as t:
            t.wait_editor(offset=0)
            t.press("F9")
            t.type("S")
            t.press("Enter")
            t.wait_for("Case sensitive")
            t.press("Escape")
            t.wait_for("Case sensitive", present=False)
        with self.terminal(root / "manager", [left, right]) as t:
            t.wait_for("Name")
            t.press("F9")
            t.type("f")
            t.wait_for("Quick cd")
            mouse(t, 95, 9)
            mouse(t, 95, 9, motion="m")
            t.wait_for("Quick cd", present=False)

    def test_names_and_find_results_retain_distinguishing_tails(self):
        root, left, right = self.fixture()
        for suffix in ("A.txt", "B.py", "C.log"):
            (left / ("identical_common_name_prefix_" + suffix)).write_text(suffix)
        for parent in ("alpha", "beta"):
            directory = left / ("shared-" + "x" * 90) / parent
            directory.mkdir(parents=True)
            (directory / "same-name.txt").write_text(parent)
        with self.terminal(root, [left, right]) as t:
            for suffix in ("A.txt", "B.py", "C.log"):
                t.wait_for(suffix)
            t.press("M-?")
            t.wait_for("Recursive name pattern")
            t.press("C-u")
            t.type("same-name.txt")
            t.press("Enter")
            t.wait_for("Search results")
            t.wait_for("alpha/same-name.txt")
            t.wait_for("beta/same-name.txt")
            t.save("identifiable-find-results")

    def test_unicode_group_mask_counts_characters(self):
        root, left, right = self.fixture()
        for name in ("a.txt", "é.txt", "界.txt", "ab.txt"):
            (left / name).write_text(name)
        with self.terminal(root, [left, right]) as t:
            t.wait_for("a.txt")
            t.type("+")
            t.wait_for("Select group")
            t.press("C-u")
            t.type("?.txt")
            t.press("Enter")
            t.wait_for("Select group", present=False)
            t.wait_for("3 selected")
            t.save("unicode-mask")

    def test_cr_save_as_retains_rows_and_next_line_delete(self):
        root, left, _ = self.fixture()
        path, output = left / "source.txt", left / "cr.txt"
        path.write_text("alpha\nbeta\ngamma\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(lines=4)
            t.press("F12")
            t.wait_for("Line endings:")
            t.press("C-u")
            t.type(str(output))
            t.press("Tab", "Right", "Right", "Right", "Enter")
            t.wait_file(output, b"alpha\rbeta\rgamma\r")
            t.wait_editor(lines=4)
            t.press("Home", "Down", "F8")
            save(t, output, b"alpha\rgamma\r")
            t.wait_editor(lines=3)

    def test_listing_formats_and_invalid_custom_input(self):
        root, left, right = self.fixture()
        for index in range(60):
            (left / f"item_{index:03}").write_text(str(index))
        with self.terminal(root, [left, right]) as t:
            t.wait_for("item_000")
            self.main_menu(t, "l", "Listing format...")
            t.wait_for("Full file list")
            t.wait_for("Brief file list:")
            t.wait_for("User mini status")
            click(t, "Brief file list:")
            t.press("Tab", "C-u")
            t.type("3")
            t.press("Enter")
            t.wait_for("Brief file list:", present=False)
            t.press("Right")
            t.wait(lambda: "item_020" in t.text().splitlines()[-5],
                   "Brief Right moves into the next visible column")
            t.press("Right")
            t.wait(lambda: "item_041" in t.text().splitlines()[-5],
                   "The configured third brief column is navigable")
            self.main_menu(t, "l", "Listing format...")
            t.wait_for("Long file list")
            click(t, "Long file list")
            t.press("Enter")
            t.wait_for("Long file list", present=False)
            t.wait(lambda: all(value in t.text().splitlines()[2]
                               for value in ("Permission", "Nl", "Owner", "Group", "Size", "Modify time", "Name")),
                   "Long uses the native single-row full-width field layout")
            self.assertEqual(t.text().splitlines()[1].count("┌"), 1)
            self.assertRegex(t.text().splitlines()[3], r"[d-][rwxstST-]{9}")
            self.main_menu(t, "l", "Listing format...")
            t.wait_for("User defined:")
            click(t, "User defined:")
            t.press("Tab", "C-u")
            t.type("half name:12 | size:8 | owner:8")
            t.press("Tab", "Space", "Tab", "C-u")
            t.type("half name space mode")
            t.press("Enter")
            t.wait_for("User defined:", present=False)
            self.assertIn("Owner", t.text().splitlines()[2][:50])
            self.main_menu(t, "l", "Listing format...")
            t.wait_for("User defined:")
            t.press("Tab", "C-u")
            t.type("unknown")
            t.press("Enter")
            t.wait_for("Unknown tag on display format")
            t.press("Escape")
            t.wait_for("User defined:", present=False)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            config = t.root / "config/mcpp/config"
            self.assertIn("left_listing=custom", config.read_text())
            self.assertIn("left_columns=half name:12 | size:8 | owner:8", config.read_text())
            self.assertIn("left_brief_columns=3", config.read_text())
            self.assertIn("left_user_mini_status=true", config.read_text())
            self.assertIn("left_status_3=half name space mode", config.read_text())

    def test_sort_dialog_is_per_pane_and_preserves_marks(self):
        root, left, right = self.fixture()
        for directory in (left, right):
            for name, data in (("a.txt", "A"), ("b.py", "BBBBBBB"), ("c.log", "CCC")):
                (directory / name).write_text(data)
        with self.terminal(root, [left, right]) as t:
            t.wait_for("a.txt")
            t.press("Down", "Insert")
            t.wait_for("1 selected")
            self.main_menu(t, "l", "Sort order...")
            t.wait_for("Executable first")
            for label in ("Unsorted", "Name", "Version", "Extension", "Size",
                          "Modify time", "Access time", "Change time", "Inode"):
                self.assertTrue(any(label in line and ("( )" in line or "(*)" in line)
                                    for line in t.text().splitlines()))
            self.assertIn("[x] Case sensitive", t.text())
            self.assertNotIn("Directories first", t.text())
            click(t, "( ) Extension")
            t.press("Enter")
            t.wait_for("Executable first", present=False)
            t.wait(lambda: "c.log" in t.text().splitlines()[4][:50],
                   "Extension sort is applied on the left")
            self.assertIn("a.txt", t.text().splitlines()[4][50:])
            t.wait_for("1 selected")
            self.main_menu(t, "r", "Sort order...")
            t.wait_for("Executable first")
            click(t, "( ) Size")
            t.press("Enter")
            t.wait_for("Executable first", present=False)
            t.wait(lambda: "c.log" in t.text().splitlines()[5][50:],
                   "Right size order is independent of left extension order")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            config = (t.root / "config/mcpp/config").read_text()
            self.assertIn("left_sort=extension", config)
            self.assertIn("right_sort=size", config)

    def test_editor_file_actions_bookmarks_and_saved_position(self):
        root, left, _ = self.fixture()
        path = left / "source.txt"
        path.write_text("first\nsecond\n")
        (left / "insert.txt").write_text("INSERT\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(offset=0)
            t.raw(b"\x1b[28~")
            t.wait_for("Insert file")
            t.type("insert.txt")
            t.press("Enter")
            t.wait_for("Insert file", present=False)
            t.wait_editor(lines=4, dirty=True)
            t.press("C-Home", "Home", "F3", *(["Right"] * 6), "F3", "C-f")
            t.wait_for("Save block")
            t.type("block.txt")
            t.press("Enter")
            t.wait_file(left / "block.txt", b"INSERT")
            t.wait_for("Save block", present=False)
            t.press("M-k", "C-End", "M-k", "M-j")
            t.wait_editor(row=1)
            t.press("M-i")
            t.wait_editor(row=4)
            save(t, path, b"INSERT\nfirst\nsecond\n")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            configuration = (t.root / "config/mcpp/config").read_text()
        config = root / "reopened/ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text(configuration)
        with self.terminal(root / "reopened", ["--edit", path]) as t:
            t.wait_editor(row=4, lines=4)
            t.press("M-j")
            t.wait_editor(row=1)
            menu(t, "File", "About...")
            t.wait_for("Independent C++20")
            t.press("Enter")

    def test_editor_user_menu_runs_from_edited_file_parent(self):
        root, left, _ = self.fixture()
        path = left / "editor.txt"
        path.write_text("unchanged\n")
        configuration = root / "ui/config/mcpp"
        configuration.mkdir(parents=True)
        (configuration / "menu").write_text("x|Record edited path|printf '%%s' %f > user-menu-output\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(offset=0)
            t.press("F11")
            t.wait_for("Record edited path")
            t.type("x")
            t.wait_file(left / "user-menu-output", str(path).encode())
            t.wait_editor(dirty=False)
        self.assertEqual(path.read_bytes(), b"unchanged\n")

    def test_directory_chooser_sizes_and_layout(self):
        root, left, right = self.fixture()
        (left / "A/inner").mkdir(parents=True)
        (left / "A/inner/payload").write_bytes(b"12345")
        with self.terminal(root, ["--printwd", root / "cwd", left, right]) as t:
            t.wait_for("Name")
            self.main_menu(t, "c", "Directory tree")
            t.wait(lambda: any("Directory tree" in row and "┌" in row for row in t.text().splitlines()),
                   "Modal Tree opens")
            t.wait_for("└─ A")
            t.press("C-s")
            t.type("A")
            t.wait_for("Search: A")
            t.wait(lambda: any(value.endswith("A") for value in t.focused_text()), "Tree search selects A")
            t.press("Escape")
            t.wait_for("Search: A", present=False)
            t.press("Right")
            t.wait_for("inner")
            t.press("Enter")
            t.wait_for("Directory changed")
            t.press("Down", "C-Space")
            t.wait_for("Directory sizes calculated")
            t.wait(lambda: any(len(columns := row.split("│")) > 2 and "inner" in columns[1] and
                               columns[2].strip() == "5" for row in t.text().splitlines()[3:-6]),
                   "Calculated directory size is shown in the listing metadata")
            self.main_menu(t, "o", "Layout...")
            t.wait_for("Panel split")
            self.assertIn("Vertical", t.text())
            self.assertIn("Horizontal", t.text())
            self.assertNotIn("Side-by-side left width percent", t.text())
            click(t, "Equal split")
            t.press("Tab", *(["Enter"] * 15))
            click(t, "[< OK >]")
            t.wait_for("Panel split", present=False)
            t.wait(lambda: t.text().splitlines()[1][35] == "┌", "Pane split applies the chosen width")
            self.main_menu(t, "o", "Layout...")
            t.wait_for("Panel split")
            click(t, "Horizontal")
            click(t, "Equal split")
            click(t, "[< OK >]")
            t.wait_for("Panel split", present=False)
            t.wait(lambda: "right" in t.text().splitlines()[14],
                   "Stacked layout renders the right pane below the left")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text().strip(), str(left / "A"))

    def test_native_layout_visibility_and_hidden_command_safety(self):
        root, left, right = self.fixture()
        (left / "alpha.txt").write_text("alpha")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("alpha.txt")
            self.main_menu(t, "o", "Layout...")
            t.wait_for("Panel split")
            for label in ("Menubar visible", "Command prompt", "Keybar visible", "Hintbar visible",
                          "XTerm window title", "Show free space"):
                click(t, label)
            click(t, "[< OK >]")
            t.wait_for("Panel split", present=False)
            t.wait(lambda: t.text().splitlines()[0].startswith("┌"),
                   "Hidden menubar gives its row back to the panes")
            self.assertNotIn("1Help", t.text())
            self.assertNotIn("TiB", t.text())
            t.type("touch should-not-exist")
            t.press("Enter")
            t.wait_idle()
            self.assertFalse((left / "should-not-exist").exists())
            t.press("F9")
            t.type("o")
            t.wait_for("Layout...")
            click(t, "Layout...")
            t.wait_for("Panel split")
            for label in ("Menubar visible", "Command prompt", "Keybar visible", "Hintbar visible",
                          "XTerm window title", "Show free space"):
                click(t, label)
            click(t, "[< OK >]")
            t.wait_for("Panel split", present=False)
            t.wait_for("1Help")
            self.assertIn("Left", t.text().splitlines()[0])

    def test_native_panel_options_filters_marking_and_quick_search(self):
        root, left, right = self.fixture()
        (left / "z-dir").mkdir()
        for name in ("a.txt", "backup~", ".hidden", "Ångström.txt"):
            (left / name).write_text(name)
        with self.terminal(root, [left, right]) as t:
            t.wait_for("backup~")
            self.main_menu(t, "o", "Panel options...")
            t.wait_for("Quick search")
            for label in ("Main options", "Navigation", "File highlight", "Mix all files",
                          "Mark moves down", "Reverse files only", "Simple swap"):
                self.assertIn(label, t.text())
            self.assertNotIn("Directories first", t.text())
            self.assertNotIn("Reverse sort", t.text())
            for label in ("Show backup files", "Show hidden files", "Mix all files",
                          "Mark moves down", "Lynx-like motion"):
                click(t, label)
            click(t, "( ) Case insensitive")
            click(t, "[< OK >]")
            t.wait_for("Quick search", present=False)
            self.assertNotIn("backup~", t.text())
            self.assertNotIn(".hidden", t.text())
            self.assertIn("a.txt", t.text().splitlines()[4][:50])
            t.press("Home", "Down", "Insert")
            t.wait_for("1 selected,")
            t.press("Insert")
            t.wait(lambda: "a.txt" in t.text().splitlines()[-5], "Mark moves down can be disabled")
            t.press("C-s")
            t.type("ång")
            t.wait_for("Quick search: ång")
            t.press("Escape")
            t.wait_for("Quick search:", present=False)
            t.wait(lambda: "Ångström.txt" in t.text().splitlines()[-5],
                   "Case-insensitive quick search overrides case-sensitive sort")
            t.press("C-s")
            t.type("z-dir")
            t.wait_for("Quick search: z-dir")
            t.press("Escape")
            t.wait_for("Quick search:", present=False)
            t.press("Right")
            t.wait(lambda: "/z-dir" in t.text().splitlines()[1][:50], "Lynx-like Right enters a directory")
            t.press("Left")
            t.wait_for("a.txt")

    def test_native_panel_options_si_units_and_mini_status(self):
        root, left, right = self.fixture()
        (left / "size.txt").write_bytes(b"x" * 1000)
        config = root / "ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text("left_user_mini_status=true\nleft_status_0=half bsize\n")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("size.txt")
            t.press("Down")
            t.wait(lambda: "1000 B" in t.text().splitlines()[-5], "Binary-size units are initially used")
            self.main_menu(t, "o", "Panel options...")
            t.wait_for("Use SI size units")
            click(t, "Use SI size units")
            click(t, "[< OK >]")
            t.wait_for("Use SI size units", present=False)
            t.wait(lambda: "1.0 kB" in t.text().splitlines()[-5], "The native SI option changes displayed sizes")
            self.main_menu(t, "o", "Panel options...")
            t.wait_for("Show mini-status")
            click(t, "Show mini-status")
            click(t, "[< OK >]")
            t.wait_for("Show mini-status", present=False)
            self.assertNotIn("1.0 kB", t.text())

    def test_native_panel_setup_restores_paths_and_cli_overrides(self):
        root, left, right = self.fixture()
        (left / "child").mkdir()
        with self.terminal(root, [left, right]) as t:
            t.wait_for("child")
            self.main_menu(t, "o", "Panel options...")
            t.wait_for("Auto save panels setup")
            click(t, "Auto save panels setup")
            click(t, "[< OK >]")
            t.wait_for("Auto save panels setup", present=False)
            t.press("Down", "Enter")
            t.wait(lambda: "/child" in t.text().splitlines()[1][:50], "Enter reaches the directory that will be saved")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            saved = (t.root / "config/mcpp/config").read_text()
        for name, arguments, expected in (
            ("restore", [], left / "child"), ("override", [left], left)
        ):
            config = root / name / "ui/config/mcpp/config"
            config.parent.mkdir(parents=True)
            config.write_text(saved)
            output = root / name / "cwd"
            with self.terminal(root / name, ["--printwd", output, *arguments]) as t:
                t.wait_for("Name")
                t.press("F10")
                t.wait_file(t.exit_file, b"0\n")
            self.assertEqual(output.read_text().strip(), str(expected))

    def test_filtered_view_compare_and_external_panelization(self):
        root, left, right = self.fixture()
        (left / "a.txt").write_text("before\n")
        (right / "a.txt").write_text("after\n")
        (left / "sub").mkdir()
        (left / "sub/hit.txt").write_text("hit\n")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("a.txt")
            select_file(t, "a.txt")
            t.press("Tab")
            select_file(t, "a.txt")
            t.press("Tab")
            t.press("C-x", "C-d")
            t.wait_for("9Options")
            self.assertTrue(any("before" in row[:50] and "after" in row[50:] for row in t.text().splitlines()))
            t.press("F10")
            t.wait_for("Name")
            t.raw(b"\x1b!")
            t.wait_for("Filtered view")
            t.press("C-u")
            t.type("tr 'a-z' 'A-Z' < a.txt")
            t.press("Enter")
            t.wait_for("BEFORE")
            t.press("F3")
            t.wait_for("Name")
            t.press("C-x")
            t.type("!")
            t.wait_for("External panelize")
            t.wait_for("Other command")
            self.assertNotIn("Output separator", t.text())
            t.press("Tab", "C-u")
            t.type("printf 'sub/hit.txt\\na.txt\\n'")
            t.press("Enter")
            t.wait_for("External panelization complete")
            t.wait_for("sub/hit.txt")
            t.press("C-x")
            t.type("!")
            t.wait_for("External panelize")
            t.wait_for("Other command")
            t.press("Tab", "C-u")
            t.type("printf missing-file")
            t.press("Enter")
            t.wait_for("Error:")
            self.assertIn("sub/hit.txt", t.text())
            t.press("C-x")
            t.type("!")
            t.wait_for("External panelize")
            t.wait_for("Other command")
            t.press("Tab", "C-u")
            t.type("printf ready; sleep 10")
            t.press("Enter")
            t.wait_for("Running...")
            t.press("Escape")
            t.wait_for("cancelled")
            self.assertIn("sub/hit.txt", t.text())

    def test_edit_symlink_and_advanced_permissions(self):
        root, left, right = self.fixture()
        first, second, link = left / "a.txt", left / "b.txt", left / "link"
        first.write_text("A")
        second.write_text("B")
        link.symlink_to("a.txt")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("link")
            select_file(t, "link")
            self.main_menu(t, "f", "Edit symlink")
            t.wait_for("points to:")
            t.press("C-u")
            t.type("b.txt")
            t.press("Enter")
            t.wait(lambda: os.readlink(link) == "b.txt", "Symlink target is replaced atomically")
            self.assertEqual(first.read_text(), "A")
            self.assertEqual(second.read_text(), "B")
            select_file(t, "a.txt")
            self.main_menu(t, "f", "Advanced chown")
            t.wait_for("Chown advanced command")
            self.assertIn("Permissions (octal):", t.text())
            self.assertIn("Flag", t.text())
            self.assertNotIn("blank keeps", t.text())
            t.press("Tab", "Tab")
            t.type("r")
            click(t, "[< Set >]")
            t.wait(lambda: first.stat().st_mode & 0o777 == 0o640,
                   "Advanced permission change updates only the selected file")
            self.assertEqual(second.stat().st_mode & 0o777, 0o644)

    def test_native_chattr_checkboxes_set_actual_inode_flags(self):
        root, left, right = self.fixture()
        path = left / "attributes.txt"
        path.write_text("attributes unchanged bytes")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("attributes.txt")
            select_file(t, "attributes.txt")
            t.press("C-x")
            t.type("e")
            t.wait_for("Chattr command")
            for label in ("Secure deletion", "Synchronous updates", "Immutable",
                          "No dump", "No update atime", "Inode uses extents", "Project hierarchy"):
                self.assertIn(label, t.text())
            self.assertNotIn("< Keep >", t.text())
            click(t, "No dump")
            click(t, "[< Set >]")
            t.wait_for("Chattr command", present=False)
            t.press("C-x")
            t.type("e")
            t.wait_for("Chattr command")
            self.assertIn("[x] (d) No dump", t.text())
            t.press("Escape")
            t.wait_for("Chattr command", present=False)
        self.assertEqual(path.read_text(), "attributes unchanged bytes")

    def test_filename_encoding_and_confirmation_settings(self):
        root, left, right = self.fixture()
        raw = os.fsencode(left) + b"/\xe9.txt"
        descriptor = os.open(raw, os.O_CREAT | os.O_WRONLY, 0o600)
        os.close(descriptor)
        removable = left / "remove.txt"
        removable.write_text("delete fixture")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("Name")
            t.press("M-e")
            t.wait_for("Choose codepage")
            t.type("1")
            t.wait_for("Choose codepage", present=False)
            t.wait_for("é.txt")
            self.main_menu(t, "o", "Confirmation...")
            t.wait_for("Directory hotlist delete")
            click(t, "[x] Delete")
            t.press("Enter")
            t.wait_for("Directory hotlist delete", present=False)
            select_file(t, "remove.txt")
            t.press("F8")
            t.wait(lambda: not removable.exists(), "Confirmation setting changes actual delete behavior")
            self.assertTrue(os.path.exists(raw))

    def test_native_editor_codepage_is_not_a_file_conversion(self):
        root, left, _ = self.fixture()
        path = left / "latin1.txt"
        path.write_bytes(b"caf\xe9\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(bytes=5, dirty=False)
            t.press("M-e")
            t.wait_for("Choose codepage")
            self.assertIn("< No translation >", t.text())
            self.assertIn("ISO 8859-1", t.text())
            self.assertNotIn("Convert editor buffer", t.text())
            t.type("1")
            t.wait_for("Choose codepage", present=False)
            t.wait_for("café")
            t.wait_editor(bytes=5, dirty=False)
            self.assertEqual(path.read_bytes(), b"caf\xe9\n")
            t.type("é")
            save(t, path, b"\xe9caf\xe9\n")
            t.type("界")
            t.wait_for("Editor error")
            t.press("Enter")
            t.wait_for("Editor error", present=False)
            t.wait_editor(bytes=6, dirty=False)
            self.assertEqual(path.read_bytes(), b"\xe9caf\xe9\n")

    def test_native_appearance_skin_and_shadow_controls(self):
        root, left, right = self.fixture()
        (left / "file.txt").write_text("skin")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("file.txt")
            before = t.text(ansi=True)
            self.main_menu(t, "o", "Appearance...")
            t.wait_for("Skin:")
            self.assertIn("Shadows", t.text())
            self.assertNotIn("File-type accents", t.text())
            click(t, "[ gray")
            t.wait_for("Skins")
            t.press("Home", "Enter")
            t.wait_for("Skins", present=False)
            click(t, "[ Cancel ]")
            t.wait_for("Skin:", present=False)
            self.main_menu(t, "o", "Appearance...")
            t.wait_for("Skin:")
            click(t, "[ gray")
            t.wait_for("Skins")
            t.press("Home", "Enter")
            t.wait_for("Skins", present=False)
            click(t, "Shadows")
            click(t, "[< OK >]")
            t.wait_for("Skin:", present=False)
            self.assertNotEqual(t.text(ansi=True), before)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            config = (t.root / "config/mcpp/config").read_text()
            self.assertIn("theme=default", config)
            self.assertIn("shadows=false", config)

    def test_native_configure_controls_and_safe_operation_defaults(self):
        root, left, right = self.fixture()
        path = left / "source.txt"
        path.write_bytes(b"source data")
        destination = right / "source.txt"
        destination.write_bytes(b"KEEP")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("source.txt")
            select_file(t, "source.txt")
            self.main_menu(t, "o", "Configuration...")
            t.wait_for("Configure options")
            for label in ("File operations", "Esc key mode", "Pause after run", "Other options",
                          "Verbose operation", "Compute totals", "Classic progressbar", "Mkdir autoname",
                          "Preallocate space", "Use internal edit", "Use internal view", "Auto save setup"):
                self.assertIn(label, t.text())
            self.assertNotIn("Directories first", t.text())
            self.assertNotIn("Editor line numbers", t.text())
            for label in ("Verbose operation", "Compute totals", "Preallocate space", "Safe delete", "Safe overwrite"):
                click(t, label)
            click(t, "Timeout:")
            t.press("C-u")
            t.type("50000")
            t.press("Enter")
            t.wait_for("Configure options", present=False)
            t.press("F8")
            t.wait_for("Delete")
            t.press("Enter")
            t.wait_for("Delete cancelled")
            self.assertTrue(path.exists())
            t.press("F5")
            t.wait_for("Source mask:")
            t.press("Enter")
            t.wait_for("Destination exists")
            t.press("Enter")
            t.wait_for("Copy incomplete")
            self.assertEqual(destination.read_bytes(), b"KEEP")
            t.press("F5")
            t.wait_for("Source mask:")
            t.press("Enter")
            t.wait_for("Destination exists")
            click(t, "[ Overwrite ]")
            t.wait_file(destination, b"source data")
            t.wait_for("Copy complete")

    def test_native_configure_auto_save_and_drop_menus(self):
        root, left, right = self.fixture()
        with self.terminal(root, [left, right]) as t:
            t.wait_for("Name")
            self.main_menu(t, "o", "Configuration...")
            t.wait_for("Configure options")
            click(t, "Auto save setup")
            click(t, "Drop down menus")
            click(t, "Timeout:")
            t.press("C-u")
            t.type("50000")
            t.press("Enter")
            t.wait_for("Configure options", present=False)
            t.press("F9")
            t.wait_for("Sort order...")
            t.press("Escape")
            t.wait_for("Sort order...", present=False)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            self.assertFalse((t.root / "config/mcpp/config").exists())

    def test_native_pause_after_run_and_auto_menu_startup(self):
        root, left, right = self.fixture()
        config = root / "ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text("auto_menus=true\npause_after_run=2\nescape_timeout=50000\n")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("Inspect selected path")
            t.press("Escape")
            t.wait_for("Inspect selected path", present=False)
            t.type("printf PAUSED")
            t.press("Enter")
            t.wait_for("Press any key to continue...")
            self.assertIn("PAUSED", t.text())
            t.press("Space")
            t.wait_for("Command exited 0")

    def test_native_sort_paragraph_and_formatter_without_scope_options(self):
        root, left, _ = self.fixture()
        path = left / "sort.txt"
        path.write_text("a\nc\nb\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(bytes=6)
            t.press("M-t")
            t.wait_for("You must first highlight a block of text")
            t.press("Enter")
            t.wait_for("You must first highlight", present=False)
            menu(t, "Edit", "Mark all")
            t.press("M-t")
            t.wait_for("Run sort")
            t.wait_for("Enter sort options")
            self.assertNotIn("Whole buffer", t.text())
            self.assertNotIn("Ignore case:", t.text())
            t.type("-r")
            t.press("Enter")
            t.wait_editor(dirty=True)
            save(t, path, b"c\nb\na\n")
            t.press("C-u")
            save(t, path, b"a\nc\nb\n")
        paragraph = left / "paragraph.txt"
        paragraph.write_text("one two three four five six\n")
        config = root / "paragraph/ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text("editor_wrap_length=12\n")
        with self.terminal(root / "paragraph", ["--edit", paragraph]) as t:
            t.wait_editor(lines=2)
            t.press("M-p")
            t.wait_editor(lines=4, dirty=True)
            self.assertNotIn("Wrap width", t.text())
            save(t, paragraph, b"one two\nthree four\nfive six\n")
        formatted = left / "formatter.txt"
        formatted.write_text("lower\n")
        formatter = root / "formatter/ui/data/mcpp/mcedit/macros.d/macro.0.sh"
        formatter.parent.mkdir(parents=True)
        formatter.write_text("0|Uppercase|tr 'a-z' 'A-Z'\n")
        with self.terminal(root / "formatter", ["--edit", formatted]) as t:
            t.wait_editor(bytes=6)
            menu(t, "Edit", "Mark all")
            menu(t, "Format", "External formatter")
            t.wait_for("LOWER")
            self.assertNotIn("Scope:", t.text())
            save(t, formatted, b"LOWER\n")

    def test_native_block_file_defaults_use_the_managed_clipfile(self):
        root, left, _ = self.fixture()
        path = left / "clip-default.txt"
        path.write_text("abc\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(offset=0)
            t.press("F3", "Right", "Right", "Right", "F3", "C-f")
            t.wait_for("Save block")
            t.press("Enter")
            t.wait_for("Save block", present=False)
            menu(t, "Edit", "Unmark")
            t.raw(b"\x1b[28~")
            t.wait_for("Insert file")
            t.press("Enter")
            t.wait_for("Insert file", present=False)
            t.wait_editor(offset=3, bytes=7, dirty=True)
            save(t, path, b"abcabc\n")

    def test_native_external_panelize_presets_add_run_remove(self):
        root, left, right = self.fixture()
        (left / "selected.txt").write_text("selected")
        (left / "other.txt").write_text("other")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("selected.txt")
            t.press("C-x")
            t.type("!")
            t.wait_for("Other command")
            for label in ("Modified git files", "Find rejects after patching", "Remove", "Add new", "Panelize"):
                self.assertIn(label, t.text())
            self.assertNotIn("Output separator", t.text())
            t.press("Tab", "C-u")
            t.type("printf 'selected.txt\\n'")
            click(t, "[ Add new ]")
            t.wait_for("Add to external panelize")
            t.wait_for("Enter command label:")
            t.type("Only selected fixture")
            t.press("Enter")
            t.wait_for("Panelize command saved")
            t.press("C-x")
            t.type("!")
            t.wait_for("Only selected fixture")
            click(t, "Only selected fixture")
            t.press("Enter")
            t.wait_for("External panelization complete")
            self.assertNotIn("other.txt", t.text())
            t.press("C-x")
            t.type("!")
            t.wait_for("Only selected fixture")
            click(t, "Only selected fixture")
            click(t, "[ Remove ]")
            t.wait_for("Panelize command removed")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
            self.assertNotIn("Only selected fixture", (t.root / "config/mcpp/config").read_text())

    def test_native_macro_hotkeys_repeat_and_literal_query(self):
        root, left, _ = self.fixture()
        path = left / "macro.txt"
        path.write_text("x\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(offset=0)
            menu(t, "Command", "Start/Stop record macro")
            t.wait_editor(flags="--R-")
            t.type("ab")
            menu(t, "Command", "Start/Stop record macro")
            t.wait_for("Press the macro's new hotkey:")
            self.assertNotIn("Name:", t.text())
            t.press("M-z")
            t.wait_for("Press the macro's new hotkey:", present=False)
            t.press("M-z")
            t.wait_for("ababx")
            menu(t, "Command", "Record/Repeat actions")
            t.type("Q")
            menu(t, "Command", "Record/Repeat actions")
            t.wait_for("Repeat last commands")
            t.wait_for("Repeat times:")
            self.assertNotIn("Source:", t.text())
            t.type("2")
            t.press("Enter")
            t.wait_for("ababQQQx")
            t.press("C-q")
            t.wait_for("Insert literal")
            t.wait_for("Press any key:")
            self.assertNotIn("Hexadecimal bytes", t.text())
            t.press("C-a")
            t.wait_for("Insert literal", present=False)
            save(t, path, b"ababQQQ\x01x\n")

    def test_native_save_mode_backup_extension_and_posix_warning(self):
        root, left, _ = self.fixture()
        path = left / "save.txt"
        path.write_bytes(b"old")
        path.chmod(0o600)
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(offset=0)
            menu(t, "Options", "Save mode...")
            t.wait_for("Edit Save Mode")
            for label in ("Quick save", "Safe save", "Do backups with following extension:",
                          "Check POSIX new line"):
                self.assertIn(label, t.text())
            click(t, "Do backups with following extension:")
            t.press("Tab", "C-u")
            t.type(".bak")
            t.press("Tab", "Space", "Enter")
            t.wait_for("Edit Save Mode", present=False)
            t.type("X")
            t.press("F2")
            t.wait_for("Save file")
            t.press("Enter")
            t.wait_for("does not end with a newline")
            click(t, "[ Cancel ]")
            t.wait_for("does not end with a newline", present=False)
            self.assertEqual(path.read_bytes(), b"old")
            self.assertFalse((left / "save.txt.bak").exists())
            t.press("F2")
            t.wait_for("Save file")
            t.press("Enter")
            t.wait_for("does not end with a newline")
            t.press("Enter")
            t.wait_file(path, b"Xold")
            t.wait_file(left / "save.txt.bak", b"old")
            self.assertEqual((left / "save.txt.bak").stat().st_mode & 0o777, 0o600)
            inode = path.stat().st_ino
            menu(t, "Options", "Save mode...")
            t.wait_for("Edit Save Mode")
            click(t, "Quick save")
            click(t, "Check POSIX new line")
            t.press("Enter")
            t.wait_for("Edit Save Mode", present=False)
            t.type("Y")
            save(t, path, b"XYold")
            self.assertEqual(path.stat().st_ino, inode)

    def test_native_window_move_resize_and_escape_rollback(self):
        root, left, _ = self.fixture()
        path = left / "window.txt"
        path.write_text("a界bc\nnext\n")
        with self.terminal(root, ["--edit", path]) as t:
            t.wait_editor(offset=0)
            menu(t, "Window", "Move")
            self.assertNotIn("Left column", t.text())
            t.wait_for("Toggle fullscreen", present=False)
            menu(t, "Window", "Toggle fullscreen")
            t.wait(lambda: all(label in t.text().splitlines()[0] for label in ("File", "Edit", "Window")),
                   "Windowed editor keeps the native global menubar")
            menu(t, "Window", "Resize")
            self.assertNotIn("Width:", t.text())
            t.press(*(["Left"] * 20), *(["Up"] * 10), "Enter")
            menu(t, "Window", "Move")
            t.press(*(["Right"] * 5), "Down", "Down", "Enter")
            t.wait(lambda: "a界bc" in t.text().splitlines()[4], "Arrow-driven move repositions the editor window")
            row = t.text().splitlines()[4]
            start = row.index("a界bc")
            mouse(t, start + 3, 4)
            mouse(t, start + 3, 4, motion="m")
            t.type("X")
            t.press("F2")
            t.wait_for("Save file")
            t.press("Enter")
            t.wait_file(path, "a界Xbc\nnext\n".encode())
            menu(t, "Window", "Move")
            t.press("Right", "Right", "Down", "Escape")
            t.wait(lambda: t.text().splitlines()[4].index("a界Xbc") == start,
                   "Escape rolls back move mode instead of quitting")
            self.assertFalse(t.exit_file.exists())

    def test_native_learn_keys_grid_and_sequence_validation(self):
        root, left, right = self.fixture()
        path = left / "view.txt"
        path.write_text("LEARNED KEY VIEW\n")
        with self.terminal(root, [left, right]) as t:
            t.wait_for("view.txt")
            select_file(t, "view.txt")
            self.main_menu(t, "o", "Learn keys...")
            t.wait_for("Function key 20")
            self.assertIn("Completion/M-tab", t.text())
            self.assertNotIn("Action: <", t.text())
            t.press("F3")
            t.wait(lambda: any("Function key 3" in line and "OK" in line for line in t.text().splitlines()),
                   "Learn keys marks an existing terminal key as working")
            click(t, "Function key 3")
            t.wait_for("Teach me a key")
            t.type("x")
            t.wait_for("Cannot accept this key")
            t.press("Enter")
            t.wait_for("Function key 20")
            click(t, "Function key 3")
            t.wait_for("Teach me a key")
            sequence = b"\x1b[99~"
            t.raw(sequence)
            t.wait_for("Teach me a key", present=False)
            t.raw(sequence)
            t.wait(lambda: any("Function key 3" in line and "OK" in line for line in t.text().splitlines()),
                   "A taught sequence is verified in the native key grid")
            click(t, "[< Save >]")
            t.wait_for("Function key 20", present=False)
            t.raw(sequence)
            t.wait_for("LEARNED KEY VIEW")
            t.press("F3")
            t.wait_for("Name")


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Remaining audit checks require the owned non-root disposable container")
    BINARY, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    ROOT = Path(sys.argv[3] if len(sys.argv) > 3 else f"remaining-audit-{os.getpid()}").resolve()
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
