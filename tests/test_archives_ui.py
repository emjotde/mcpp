from pathlib import Path
import hashlib
import io
import os
import stat
import sys
import tarfile
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click, select_file


PAYLOAD = b"archive contents stay in the archive\nsecond line\n"


def make_tar(path, mode="w"):
    with tarfile.open(path, mode, format=tarfile.PAX_FORMAT) as archive:
        directory = tarfile.TarInfo("folder")
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o750
        directory.uid = os.getuid()
        directory.gid = os.getgid()
        directory.mtime = 1700000030
        archive.addfile(directory)
        document = tarfile.TarInfo("folder/data.txt")
        document.size = len(PAYLOAD)
        document.mode = 0o640
        document.uid = os.getuid()
        document.gid = os.getgid()
        document.mtime = 1700000030
        archive.addfile(document, io.BytesIO(PAYLOAD))
        linked = tarfile.TarInfo("linked.txt")
        linked.type = tarfile.LNKTYPE
        linked.linkname = "folder/data.txt"
        linked.mode = document.mode
        linked.uid = document.uid
        linked.gid = document.gid
        linked.mtime = document.mtime
        archive.addfile(linked)
        symbolic = tarfile.TarInfo("relative-link")
        symbolic.type = tarfile.SYMTYPE
        symbolic.linkname = "folder/data.txt"
        symbolic.mode = 0o777
        symbolic.uid = os.getuid()
        symbolic.gid = os.getgid()
        symbolic.mtime = document.mtime
        archive.addfile(symbolic)


def make_cpio(path):
    output = bytearray()

    def member(name, mode, data, inode):
        encoded = name.encode() + b"\0"
        fields = (inode, mode, os.getuid(), os.getgid(), 1, 1700000030,
                  len(data), 0, 0, 0, 0, len(encoded), 0)
        output.extend(b"070701" + b"".join(f"{value:08x}".encode() for value in fields))
        output.extend(encoded)
        output.extend(b"\0" * (-len(output) % 4))
        output.extend(data)
        output.extend(b"\0" * (-len(output) % 4))

    member("folder", stat.S_IFDIR | 0o750, b"", 1)
    member("folder/data.txt", stat.S_IFREG | 0o640, PAYLOAD, 2)
    member("TRAILER!!!", 0, b"", 0)
    path.write_bytes(output)


class ArchivesUI(unittest.TestCase):
    def fixture(self):
        root = ROOT / self._testMethodName
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        return root, left, right

    def open_archive(self, terminal, name):
        terminal.wait_for(name)
        terminal.press("Home", "Down", "Enter")
        terminal.wait_for("/utar://" if name.endswith((".tar", ".tar.gz", ".tar.bz2", ".tar.xz")) else "/ucpio://")
        terminal.wait_for("Reading archive", present=False)

    def test_tar_browse_view_copy_hardlinks_and_return(self):
        root, left, right = self.fixture()
        path = left / "sample.tar"
        make_tar(path)
        original = path.read_bytes()
        with TerminalSession(MANAGER, TMUX, root / "ui", [left, right], record_exit=True) as terminal:
            self.open_archive(terminal, path.name)
            terminal.press("C-a", "F5")
            terminal.wait_for("Copy:")
            terminal.press("Enter")
            terminal.wait_file(right / "folder/data.txt", PAYLOAD)
            terminal.wait_file(right / "linked.txt", PAYLOAD)
            terminal.wait_for("progress", present=False)
            copied = (right / "folder/data.txt").stat()
            linked = (right / "linked.txt").stat()
            self.assertEqual((copied.st_dev, copied.st_ino), (linked.st_dev, linked.st_ino))
            self.assertEqual(copied.st_nlink, 2)
            self.assertEqual(stat.S_IMODE(copied.st_mode), 0o640)
            self.assertEqual(int(copied.st_mtime), 1700000030)
            self.assertEqual(os.readlink(right / "relative-link"), "folder/data.txt")
            select_file(terminal, "linked.txt")
            terminal.press("F3")
            terminal.wait_for("archive contents stay in the archive")
            terminal.press("F10")
            terminal.wait_for("archive contents stay in the archive", present=False)
            terminal.raw(b"\x1b[5;5~")
            terminal.wait_for("/utar://", present=False)
            terminal.wait_for(path.name)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(path.read_bytes(), original)

    def test_compressed_tar_and_cpio_enter_and_parent(self):
        root, left, right = self.fixture()
        for name, mode in (("sample.tar.gz", "w:gz"), ("sample.tar.bz2", "w:bz2"), ("sample.tar.xz", "w:xz")):
            with self.subTest(name=name):
                directory = left / name.replace(".", "-")
                directory.mkdir()
                path = directory / name
                make_tar(path, mode)
                with TerminalSession(MANAGER, TMUX, root / name, [directory, right], record_exit=True) as terminal:
                    self.open_archive(terminal, name)
                    terminal.press("C-s")
                    terminal.type("folder")
                    terminal.press("Enter")
                    terminal.wait_for("/utar://folder")
                    select_file(terminal, "data.txt")
                    terminal.press("F3")
                    terminal.wait_for("archive contents stay in the archive")
                    terminal.press("F10")
                    terminal.wait_for("archive contents stay in the archive", present=False)
                    terminal.raw(b"\x1b[5;5~")
                    terminal.wait_for("/utar://folder", present=False)
                    terminal.raw(b"\x1b[5;5~")
                    terminal.wait_for("/utar://", present=False)
                    terminal.press("F10")
                    terminal.wait_file(terminal.exit_file, b"0\n")
        directory = left / "cpio"
        directory.mkdir()
        make_cpio(directory / "sample.cpio")
        with TerminalSession(MANAGER, TMUX, root / "cpio-ui", [directory, right], record_exit=True) as terminal:
            self.open_archive(terminal, "sample.cpio")
            terminal.press("C-s")
            terminal.type("folder")
            terminal.press("Enter")
            terminal.wait_for("/ucpio://folder")
            select_file(terminal, "data.txt")
            terminal.press("F5")
            terminal.wait_for("Copy:")
            terminal.press("Enter")
            terminal.wait_file(right / "data.txt", PAYLOAD)
            terminal.wait_for("progress", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_native_uri_startup_printwd_and_viewer(self):
        root, left, right = self.fixture()
        path = left / "sample.tar"
        make_tar(path)
        directory = str(path) + "/utar://folder"
        member = directory + "/data.txt"
        output = root / "final-directory"
        with TerminalSession(MANAGER, TMUX, root / "ui", ["-P", output, directory, right], record_exit=True) as terminal:
            terminal.wait_for("/utar://folder")
            terminal.wait_for("data.txt")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(output.read_text(), directory + "\n")
        with TerminalSession(VIEWER, TMUX, root / "viewer", [member], record_exit=True) as terminal:
            terminal.wait_for("archive contents stay in the archive")
            terminal.press("F4", "F2")
            terminal.wait_for("read-only")
            terminal.press("Enter")
            terminal.wait_for("read-only", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_shell_uses_container_parent_without_leaving_archive(self):
        root, left, right = self.fixture()
        path = left / "sample.tar"
        make_tar(path)
        output = root / "shell-directory"
        with TerminalSession(MANAGER, TMUX, root / "ui", [left, right], record_exit=True) as terminal:
            self.open_archive(terminal, path.name)
            terminal.type(f"pwd > '{output}'")
            terminal.press("Enter")
            terminal.wait_file(output, (str(left) + "\n").encode())
            terminal.wait_for("/utar://")
            terminal.wait_for("Command exited")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_readonly_editor_save_as_to_local_keeps_archive(self):
        root, left, right = self.fixture()
        path = left / "sample.tar"
        make_tar(path)
        before = hashlib.sha256(path.read_bytes()).digest()
        saved = right / "saved-draft.txt"
        with TerminalSession(MANAGER, TMUX, root / "ui", [left, right], record_exit=True) as terminal:
            self.open_archive(terminal, path.name)
            select_file(terminal, "linked.txt")
            terminal.press("F4")
            terminal.wait_editor(dirty=False)
            terminal.type("X")
            terminal.press("F2")
            terminal.wait_for("read-only")
            terminal.press("Enter")
            terminal.wait_for("read-only", present=False)
            terminal.wait_editor(dirty=True)
            terminal.press("F12")
            terminal.wait_for("Save as")
            terminal.press("C-u")
            terminal.type(str(saved))
            terminal.press("Enter")
            terminal.wait_file(saved, b"X" + PAYLOAD)
            terminal.press("F10")
            terminal.wait_for("/utar://")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), before)

    def test_relative_copy_destination_cannot_become_a_host_path(self):
        root, left, right = self.fixture()
        trap = root / "host-trap"
        trap.mkdir()
        target = trap / "target.txt"
        target.write_bytes(b"host sentinel\n")
        path = left / "sample.tar"
        directory = str(trap).lstrip("/")
        with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
            info = tarfile.TarInfo(directory + "/data.txt")
            info.size = len(PAYLOAD)
            info.mode = 0o640
            archive.addfile(info, io.BytesIO(PAYLOAD))
        config = root / "ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text("confirm_overwrite=false\n")
        location = str(path) + "/utar://" + directory
        with TerminalSession(MANAGER, TMUX, root / "ui", [location, right], record_exit=True) as terminal:
            terminal.wait_for("data.txt")
            select_file(terminal, "data.txt")
            terminal.press("F5")
            terminal.wait_for("Copy:")
            terminal.press("C-u")
            terminal.type("target.txt")
            terminal.press("Enter")
            terminal.wait_for("read-only")
            self.assertEqual(target.read_bytes(), b"host sentinel\n")
            terminal.wait_for("Copy:", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual(target.read_bytes(), b"host sentinel\n")

    def test_synchronize_panels_preserves_archive_provider(self):
        root, left, right = self.fixture()
        path = left / "sample.tar"
        make_tar(path)
        resource = root / "ui/config/mcpp/mcpp.keymap"
        resource.parent.mkdir(parents=True)
        resource.write_text("[filemanager]\nPanelOtherSync=alt-y\n")
        with TerminalSession(MANAGER, TMUX, root / "ui", [left, right], record_exit=True) as terminal:
            self.open_archive(terminal, path.name)
            terminal.press("M-y")
            terminal.wait(lambda: terminal.text().splitlines()[1].count("/utar://") == 2,
                          "Both pane headings identify the archive")
            terminal.press("Tab")
            terminal.raw(b"\x1b[5;5~")
            terminal.wait(lambda: terminal.text().splitlines()[1].count("/utar://") == 1,
                          "Only the other pane leaves the archive")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_cache_list_and_local_vfs_options(self):
        root, left, right = self.fixture()
        path = left / "sample.tar"
        make_tar(path)
        config = root / "ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text("auto_save_setup=true\n")
        with TerminalSession(MANAGER, TMUX, root / "ui", [left, right], record_exit=True) as terminal:
            self.open_archive(terminal, path.name)
            terminal.raw(b"\x1b[5;5~")
            terminal.wait_for("/utar://", present=False)
            terminal.press("C-x")
            terminal.type("a")
            terminal.wait_for("Active VFS list")
            terminal.wait_for("sample.tar/utar://")
            terminal.press("Enter")
            terminal.wait_for("Active VFS list", present=False)
            terminal.wait_for("/utar://")
            terminal.press("F9")
            click(terminal, "Options", row=0)
            terminal.wait_for("Virtual FS...")
            click(terminal, "Virtual FS...")
            terminal.wait_for("Timeout for freeing VFSs")
            terminal.press("C-u")
            terminal.type("17")
            terminal.press("Enter")
            terminal.wait_for("Timeout for freeing VFSs", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertIn("vfs_timeout=17\n", config.read_text())


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Archive workflows require the owned non-root disposable container")
    MANAGER, VIEWER = (str(Path(value).resolve()) for value in sys.argv[1:3])
    TMUX = sys.argv[3]
    ROOT = Path(sys.argv[4]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[5:]], verbosity=2)
