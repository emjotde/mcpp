from pathlib import Path
import io
import os
import sys
import tarfile
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click, select_file


class NativeMenusUI(unittest.TestCase):
    def test_panels(self):
        root = ROOT / "panels"
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        name = "selected ' $(touch SHOULD_NOT_EXIST)\nfile.txt"
        (left / name).write_text("selected data\n")
        (left / "marked space.txt").write_text("marked data\n")
        (left / "child space").mkdir()
        (right / "other ' file.dat").write_text("other data\n")
        with tarfile.open(left / "local.tar", "w") as archive:
            data = b"ARCHIVE MEMBER DATA\n"
            member = tarfile.TarInfo("member.txt")
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
        local_menu = left / ".mc.menu"
        local_menu.write_text(
            "shell_patterns=1\n"
            "+ f *.missing\nx Hidden entry\n    :\n"
            "= f *.txt & t r\nr Record local context\n"
            "    printf '%%s\\0' %f %p %F %d %D %x %b %s %t %m "
            "%var{MCPP_NATIVE_MENU_NEVER_SET_A639:environment fallback} %{Menu argument} > native-output\n"
            "l|Legacy command|printf '%%s' %f > legacy-output\n"
            "v Filtered internal view\n    %view{ascii} printf 'MENU-FILTER-OUTPUT\\n'\n"
            "c Change directory\n    %cd %d/'child space'\n"
            "f Retain failed prompt\n    test %{Menu decision} = accepted\n"
        )
        config = root / "ui/config/mcpp"
        config.mkdir(parents=True)
        (config / "menu").write_text(
            "u Parent directory\n    %cd ..\n"
            "+ f member.txt & t r\ng Archive member command\n"
            "    printf '%%s' %f > archive-command-was-run\n"
            "v Internal member view\n    %view\n"
        )
        (config / "config").write_text("pause_after_run=0\nuse_subshell=0\n")
        with TerminalSession(BINARY, TMUX, root / "ui", [left, right], record_exit=True) as t:
            t.wait_for("Name")
            t.press("Tab")
            select_file(t, "other")
            t.press("Tab")
            select_file(t, "marked")
            t.press("Insert")
            select_file(t, "selected")
            t.press("F2")
            t.wait_for("Record local context")
            self.assertNotIn("Hidden entry", t.text())
            t.press("Enter")
            t.wait_for("Menu argument")
            answer = "answer ' $(touch SHOULD_NOT_EXIST); *"
            t.type(answer)
            t.press("Enter")
            expected = [
                name, name, "other ' file.dat", str(left), str(right), "txt", name[:-4],
                "marked space.txt", "marked space.txt", str(local_menu), "environment fallback", answer,
            ]
            t.wait_file(left / "native-output", b"".join(value.encode() + b"\0" for value in expected))
            self.assertFalse((left / "SHOULD_NOT_EXIST").exists())
            t.press("F2", "l")
            t.wait_file(left / "legacy-output", str(left / name).encode())
            t.press("F2", "f")
            t.wait_for("Menu decision")
            t.type("rejected")
            t.press("Enter")
            t.wait_for("Retain failed prompt exited 1")
            self.assertIn("rejected", t.text())
            t.press("C-u")
            t.type("accepted")
            t.press("Enter")
            t.wait_for("Menu decision", present=False)
            t.press("F2", "v")
            t.wait_for("MENU-FILTER-OUTPUT")
            t.press("F3")
            t.wait_for("Name")
            t.press("F2", "c")
            t.wait(lambda: "child space" in t.text().splitlines()[1], "%cd changes the pane")
            t.press("F2", "u")
            t.wait(lambda: "child space" not in t.text().splitlines()[1], "%cd returns to the parent")
            select_file(t, "local.tar")
            t.press("Enter")
            t.wait_for("member.txt")
            select_file(t, "member")
            t.press("F2")
            t.wait_for("Archive member command")
            t.press("g")
            t.wait_for("copy archive members")
            self.assertIn("Archive member command", t.text())
            self.assertFalse((left / "archive-command-was-run").exists())
            t.press("v")
            t.wait_for("ARCHIVE MEMBER DATA")
            t.press("F3")
            t.wait_for("member.txt")
            t.save("native-panels-passed")

    def test_editor(self):
        root = ROOT / "editor"
        directory = root / "files"
        directory.mkdir(parents=True)
        path = directory / "edit.cpp"
        path.write_bytes(b"alpha\nbeta\n")
        config = root / "ui/config/mcpp"
        (config / "mcedit").mkdir(parents=True)
        menu = config / "mcedit/menu"
        menu.write_text(
            "+ y cpp | y C/C++* & f *.cpp\n= t r\nb Uppercase marked block\n"
            "    tr '[:lower:]' '[:upper:]' < %b > %k.new\n"
            "    cat %k.new > %b\n"
            "    rm -- %k.new\n"
            "r Record editor context\n"
            "    printf '%%s\\0' %f %n %x %c %i %y %m > editor-context\n"
            "v Editor internal view\n    %view printf 'EDITOR-MENU-VIEW\\n'\n"
        )
        (config / "menu").write_text("w|Wrong common menu|:\n")
        (config / "config").write_text("pause_after_run=0\nuse_subshell=0\n")
        with TerminalSession(BINARY, TMUX, root / "ui", ["--edit", path], record_exit=True) as t:
            t.wait_editor(offset=0, dirty=False)
            t.press("Right", "Right", "F11")
            t.wait_for("Uppercase marked block")
            self.assertNotIn("Wrong common menu", t.text())
            t.press("r")
            output = directory / "editor-context"
            t.wait(lambda: output.is_file() and len(output.read_bytes().split(b"\0")) == 8,
                   "record editor macro context")
            syntax = output.read_bytes().split(b"\0")[5].decode()
            self.assertIn(syntax, ("cpp", "C/C++ Program"))
            expected = [str(path), str(path.with_suffix("")), "cpp", "2", "  ", syntax, str(menu)]
            t.wait_file(output, b"".join(value.encode() + b"\0" for value in expected))
            t.wait_editor(dirty=False)
            t.press("C-Home", "Home", "F3", "C-End")
            t.wait_editor(selection=11)
            t.press("F11")
            t.wait_for("Uppercase marked block")
            t.press("Enter")
            t.wait_editor(dirty=True, bytes=11)
            t.wait_for("ALPHA")
            t.wait_for("BETA")
            self.assertEqual(path.read_bytes(), b"alpha\nbeta\n")
            t.press("F11", "v")
            t.wait_for("EDITOR-MENU-VIEW")
            t.press("F3")
            t.wait_editor(dirty=True, bytes=11)
            t.wait_for("ALPHA")
            t.save("native-editor-passed")
            t.press("F10")
            t.wait_for("Save before close?")
            click(t, "[ No ]")
            t.wait_file(t.exit_file, b"0\n")
            self.assertEqual(path.read_bytes(), b"alpha\nbeta\n")


if __name__ == "__main__":
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("Native menu checks require uid1001 in the approved disposable Docker container")
    BINARY, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    ROOT = Path(sys.argv[3]).resolve()
    if not ROOT.is_relative_to("/review/cases/native-menus"):
        raise SystemExit("Native menu fixtures must stay in /review/cases/native-menus")
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
