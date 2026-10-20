from pathlib import Path
import os
import sys
import tarfile
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click


class HotlistUI(unittest.TestCase):
    def fixture(self):
        root = ROOT / self._testMethodName
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        profile = root / "ui"
        config = profile / "config/mcpp"
        config.mkdir(parents=True)
        (config / "config").write_text(
            "auto_save_setup=false\nconfirm_hotlist_delete=true\nsafe_delete=true\n"
        )
        return root, left, right, profile, config

    def test_named_groups_move_and_legacy_persistence(self):
        root, left, right, profile, config = self.fixture()
        target, legacy = left / "destination", left / "legacy"
        target.mkdir()
        legacy.mkdir()
        settings = config / "config"
        settings.write_text(settings.read_text() + f"bookmark={legacy}\n")
        original_config = settings.read_bytes()
        hotlist = config / "hotlist"
        hotlist.write_text(f'GROUP "Existing"\n ENTRY "Named target" URL "{target}"\nENDGROUP\n')
        with TerminalSession(BINARY, TMUX, profile, ["--printwd", root / "cwd", left, right],
                             record_exit=True) as t:
            t.wait_for("Name")
            t.raw(b"\x1c")
            t.wait_for("Directory hotlist")
            for button in ("Add current", "New group", "New entry", "Remove", "Move"):
                self.assertIn(button, t.text())
            t.wait_for("legacy")
            click(t, "New group")
            t.wait_for("New hotlist group")
            t.type("Projects")
            click(t, "Append")
            t.wait_for("New hotlist group", present=False)
            t.press("Enter")
            t.wait(lambda: any("Projects" in line and "─" in line for line in t.text().splitlines()),
                   "The new group is open")
            click(t, "New entry")
            t.wait_for("New hotlist entry")
            t.press("C-u")
            t.type("Project target")
            t.press("Tab", "C-u")
            t.type(str(target))
            click(t, "Append")
            t.wait_for("New hotlist entry", present=False)
            click(t, "Move")
            t.wait_for("Moving Project target")
            click(t, "Up")
            t.wait_for("->Existing")
            click(t, "->Existing", twice=True)
            t.wait_for("Named target")
            click(t, "Append")
            t.wait_for("Moving Project target", present=False)
            t.wait_for("Directory hotlist")
            t.save("named-groups-and-move")
            t.press("Enter")
            t.wait_for("Directory hotlist", present=False)
            t.wait(lambda: "destination" in t.text().splitlines()[1][:50],
                   "A named entry changes the active panel directory")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text().strip(), str(target))
        self.assertEqual(settings.read_bytes(), original_config)
        persisted = hotlist.read_text()
        self.assertIn('GROUP "Projects"', persisted)
        self.assertIn('ENTRY "Project target"', persisted)
        with TerminalSession(BINARY, TMUX, profile, [left, right], record_exit=True) as t:
            t.wait_for("Name")
            t.raw(b"\x1c")
            t.wait_for("Directory hotlist")
            t.press("Enter")
            t.wait_for("Named target")
            t.wait_for("Project target")
            click(t, "Cancel")
            t.wait_for("Directory hotlist", present=False)
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")

    def test_cancel_failed_add_and_archive_location(self):
        root, left, right, profile, config = self.fixture()
        host = left / "same-path"
        host.mkdir()
        (host / "HOST_ONLY").write_text("host data\n")
        archive = left / "locations.tar"
        member = str(host).lstrip("/")
        with tarfile.open(archive, "w") as output:
            directory = tarfile.TarInfo(member)
            directory.type = tarfile.DIRTYPE
            output.addfile(directory)
            entry = tarfile.TarInfo(member + "/ARCHIVE_ONLY")
            output.addfile(entry)
        original_archive = archive.read_bytes()
        uri = str(archive) + "/utar://" + member
        hotlist = config / "hotlist"
        (config / "mcpp.keymap").write_text("[filemanager]\nHotListAdd=alt-h\n")
        with TerminalSession(BINARY, TMUX, profile, [uri, right], record_exit=True) as t:
            t.wait_for("ARCHIVE_ONLY")
            t.press("M-h")
            t.wait_for("Add to hotlist")
            t.press("C-u")
            t.type("Archive bookmark")
            config.chmod(0o500)
            try:
                t.press("Enter")
                t.wait_for("Permission denied")
                self.assertIn("Add to hotlist", t.text())
                self.assertIn("Archive bookmark", t.text())
                self.assertFalse(hotlist.exists())
                t.save("failed-add-keeps-label")
            finally:
                config.chmod(0o700)
            t.press("Enter")
            t.wait_for("Add to hotlist", present=False)
            self.assertIn(f'URL "{uri}"', hotlist.read_text())
            saved = hotlist.read_bytes()
            t.press("C-x")
            t.type("h")
            t.wait_for("Add to hotlist")
            click(t, "Cancel")
            t.wait_for("Add to hotlist", present=False)
            self.assertEqual(hotlist.read_bytes(), saved)
            t.raw(b"\x1c")
            t.wait_for("Directory hotlist")
            click(t, "New group")
            t.wait_for("New hotlist group")
            t.type("Cancelled group")
            click(t, "Cancel")
            t.wait_for("New hotlist group", present=False)
            self.assertEqual(hotlist.read_bytes(), saved)
            t.press("Delete")
            t.wait_for("Directory hotlist delete")
            t.press("Escape")
            t.wait_for("Directory hotlist delete", present=False)
            self.assertEqual(hotlist.read_bytes(), saved)
            click(t, "Cancel")
            t.wait_for("Directory hotlist", present=False)
            t.press("M-c")
            t.wait_for("Change directory")
            t.press("C-u")
            t.type(str(host))
            t.press("Enter")
            t.wait_for("Change directory", present=False)
            t.wait_for("HOST_ONLY")
            t.raw(b"\x1c")
            t.wait_for("Directory hotlist")
            t.press("Enter")
            t.wait_for("Directory hotlist", present=False)
            t.wait_for("ARCHIVE_ONLY")
            t.wait(lambda: "/utar://" in t.text().splitlines()[1][:50],
                   "Reopening the hotlist entry retains the archive provider")
            self.assertNotIn("HOST_ONLY", "\n".join(line[:50] for line in t.text().splitlines()[2:-5]))
            t.save("archive-hotlist-location")
            t.press("F10")
            t.wait_file(t.exit_file, b"0\n")
        self.assertEqual((host / "HOST_ONLY").read_bytes(), b"host data\n")
        self.assertEqual(archive.read_bytes(), original_archive)


if __name__ == "__main__":
    if os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Hotlist UI checks require the owned uid1001 disposable container")
    BINARY, TMUX = sys.argv[1:3]
    base = Path(sys.argv[3]).resolve()
    if base != Path("/review/cases/hierarchical-hotlists"):
        raise SystemExit("Expected /review/cases/hierarchical-hotlists")
    ROOT = base / f"ui-{os.getpid()}"
    ROOT.mkdir()
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
