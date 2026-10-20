from pathlib import Path
from unittest.mock import patch
import io
import os
import shlex
import shutil
import sys
import tarfile
import unittest

from terminal_session import TerminalSession
from test_audit_ui import select_file


class ExtensionsUI(unittest.TestCase):
    def fixture(self, name, *, internal_view=True, internal_edit=True):
        root = ROOT / name
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        config = root / "ui/config/mcpp"
        config.mkdir(parents=True)
        (config / "config").write_text(
            "pause_after_run=0\nuse_subshell=0\nconfirm_execute=0\nask_new_filename=0\n"
            f"use_internal_view={int(internal_view)}\nuse_internal_edit={int(internal_edit)}\n"
        )
        return root, left, right, config

    def test_streamed_view_output(self):
        root, left, right, config = self.fixture("streamed")
        path = left / "source.case"
        original = b"UNCHANGED SOURCE\n"
        path.write_bytes(original)
        (config / "mc.ext.ini").write_text(
            "[mc.ext.ini]\nVersion=4.0\n[Streamed]\nShell=.case\n"
            "View=%view{ascii,unform} head -c 16777233 /dev/zero | "
            "tr '\\\\000' 'x' | fold -w 4096; printf '\\nLARGE-VIEW-TAIL\\n'\n")
        (left / ".mc.menu").write_text(
            "v Streamed menu view\n    %view{ascii,unform} printf MENU-VIEW-TAIL\n")
        with TerminalSession(BINARY, TMUX, root / "ui", [left, right]) as t:
            t.wait_for("Name")
            select_file(t, path.name)
            t.press("F3")
            t.wait_for("3Quit")
            t.press("C-End")
            t.wait_for("LARGE-VIEW-TAIL")
            t.press("F4", "F2")
            t.wait_for("command output is read-only")
            t.press("Enter")
            t.wait_for("command output is read-only", present=False)
            t.press("F3")
            t.wait_for("Name")
            t.press("F2", "v")
            t.wait_for("MENU-VIEW-TAIL")
            t.press("F3")
            t.wait_for("Name")
            t.raw(b"\x1b!")
            t.wait_for("Filter command and arguments:")
            t.press("C-u")
            t.type("printf FILTERED-VIEW-TAIL")
            t.press("Enter")
            t.wait_for("FILTERED-VIEW-TAIL")
            t.save("streamed-view-entrypoints")
        self.assertEqual(path.read_bytes(), original)

    def test_streamed_view_lifetime_and_failure(self):
        root, left, right, config = self.fixture("streamed-lifetime")
        path = left / "source.case"
        original = b"UNCHANGED SOURCE\n"
        path.write_bytes(original)
        resource = config / "mc.ext.ini"

        def command(value):
            resource.write_text(
                "[mc.ext.ini]\nVersion=4.0\n[Streamed]\nShell=.case\nView=%view " + value + "\n")

        command("printf FIRST-VIEW-OUTPUT")
        with TerminalSession(BINARY, TMUX, root / "ui", [left, right]) as t:
            t.wait_for("Name")
            select_file(t, path.name)
            t.press("F3")
            t.wait_for("FIRST-VIEW-OUTPUT")
            t.raw(b"\x1b`")
            t.wait_for("Screen list")
            t.press("Home", "Enter")
            t.wait_for("Screen list", present=False)
            t.wait_for("Name")
            command("printf SECOND-VIEW-OUTPUT")
            t.press("F3")
            t.wait_for("SECOND-VIEW-OUTPUT")
            t.raw(b"\x1b{")
            t.wait_for("FIRST-VIEW-OUTPUT")
            t.press("F3")
            t.wait_for("Name")
            t.raw(b"\x1b}")
            t.wait_for("SECOND-VIEW-OUTPUT")
            t.press("F3")
            t.wait_for("Name")
            command("printf BAD-PARTIAL; printf FILTER-FAILED >&2; exit 7")
            t.press("F3")
            t.wait_for("FILTER-FAILED")
            self.assertNotIn("BAD-PARTIAL", t.text())
            t.wait_for("3View")
            t.wait(lambda: not list((t.root / "runtime").glob("mcpp-local-*")),
                   "closed and failed views release their private output")
            t.save("streamed-view-lifetimes")
        self.assertEqual(path.read_bytes(), original)

    def test_local_actions(self):
        root, left, right, config = self.fixture("actions")
        name = "odd ' $(touch BAD).case"
        path = left / name
        path.write_text("ORIGINAL\n")
        (left / "marked space.txt").write_text("marked\n")
        resource = config / "mc.ext.ini"
        content = (
            "[mc.ext.ini]\nVersion=4.0\n"
            "[By name]\nShell=.case\nInclude=local\n"
            "[Include/local]\n"
            "Open=printf '%%s\\\\0' %f %p %d %t %{Extension note} "
            '"$MC_EXT_FILENAME" "$MC_EXT_BASENAME" > open-output\n'
            "View=%view{ascii,unform} printf EXTENSION-VIEW\n"
            "Edit=printf EDIT >> %f\n"
        )
        resource.write_text(content)
        legacy = config / "extensions"
        legacy.write_text("*.case=printf '%s' %f > legacy-output\n")
        with TerminalSession(BINARY, TMUX, root / "ui", [left, right], record_exit=True) as t:
            t.wait_for("Name")
            select_file(t, "marked")
            t.press("Insert")
            t.press("C-s")
            t.wait_for("Quick search:")
            t.type("odd")
            t.wait_for("Quick search: odd")
            t.press("Enter")
            t.wait_for("Quick search:", present=False)
            t.press("Enter")
            t.wait_for("Extension note")
            answer = "' $(touch BAD); *"
            t.type(answer)
            t.press("Enter")
            values = [name, name, str(left), "marked space.txt", answer, name, name]
            t.wait_file(left / "open-output", b"".join(value.encode() + b"\0" for value in values))
            self.assertFalse((left / "BAD").exists())
            self.assertFalse((left / "legacy-output").exists())
            t.press("F3")
            t.wait_for("EXTENSION-VIEW")
            t.press("F3")
            t.wait_for("Name")
            t.press("F4")
            t.wait_file(path, b"ORIGINAL\nEDIT")
            t.raw(b"\x1b[25~")
            t.wait_for("ORIGINAL")
            t.press("F3")
            t.wait_for("Name")
            resource.unlink()
            t.press("Enter")
            t.wait_file(left / "legacy-output", str(path).encode())
            t.save("extension-local-actions")

        external, ext_left, ext_right, ext_config = self.fixture(
            "external-view", internal_view=False, internal_edit=False
        )
        external_file = ext_left / "external.case"
        external_file.write_text("EXTERNAL DATA\n")
        (ext_config / "mc.ext.ini").write_text(content.replace("%{Extension note}", "ignored"))
        helper = external / "viewer-helper"
        helper.write_text("#!/bin/sh\nprintf '%s' \"$1\" > " + shlex.quote(str(ext_left / "viewer-output")) + "\n")
        helper.chmod(0o700)
        with patch.dict(os.environ, {"VIEWER": str(helper), "PAGER": "mcpp-intentionally-missing-pager"}):
            with TerminalSession(BINARY, TMUX, external / "ui", [ext_left, ext_right], record_exit=True) as t:
                t.wait_for("Name")
                select_file(t, "external.case")
                t.press("F3")
                t.wait_file(ext_left / "viewer-output", str(external_file).encode())
                t.press("F4")
                t.wait_file(external_file, b"EXTERNAL DATA\nEDIT")
                t.save("extension-external-view")

    def test_boundaries_and_errors(self):
        root, left, right, config = self.fixture("boundaries")
        path = left / "draft.plain"
        path.write_text("DRAFT DATA\n")
        shadow = left / "shadow.safe"
        shadow.write_text("HOST-SHADOW\n")
        resource = config / "mc.ext.ini"
        resource.write_text("[mc.ext.ini]\nVersion=4.0\n[Missing]\nShell=.safe\nOpen=mcpp-missing-extension-helper\n")
        fallback = left / "fallback-was-run"
        (config / "extensions").write_text("*.safe=printf BAD > " + shlex.quote(str(fallback)) + "\n")
        archive = left / "members.tar"
        with tarfile.open(archive, "w") as output:
            member = tarfile.TarInfo(str(shadow).lstrip("/"))
            data = b"ARCHIVE-CONTENT\n"
            member.size = len(data)
            output.addfile(member, io.BytesIO(data))
        with TerminalSession(BINARY, TMUX, root / "ui", [left, right], record_exit=True) as t:
            t.wait_for("Name")
            select_file(t, "shadow.safe")
            t.press("Enter")
            t.wait_for("required helper command is missing")
            resource.write_text("[mc.ext.ini]\nVersion=4.0\n[Invalid]\nRegex=[\nOpen=wrong\n")
            t.press("Enter")
            t.wait_for("invalid expression")
            self.assertFalse(fallback.exists())
            if shutil.which("file") is None:
                resource.write_text("[mc.ext.ini]\nVersion=4.0\n[Type]\nShell=.safe\nType=ASCII\nOpen=wrong\n")
                t.press("Enter")
                t.wait_for("optional 'file' tool")
            resource.write_text("[mc.ext.ini]\nVersion=4.0\n")
            select_file(t, "draft.plain")
            t.press("F4")
            t.wait_editor(dirty=False)
            t.type("UNSAVED")
            t.wait_editor(dirty=True)
            t.raw(b"\x1b`")
            t.wait_for("Screen list")
            t.press("Home", "Enter")
            t.wait_for("Name")
            resource.write_text("[mc.ext.ini]\nVersion=4.0\n[Edit]\nShell=.plain\nEdit=printf BAD >> %f\n")
            t.press("F4")
            t.wait_for("dirty editor buffer")
            self.assertEqual(path.read_bytes(), b"DRAFT DATA\n")
            resource.write_text(
                "[mc.ext.ini]\nVersion=4.0\n[Archive]\nShell=.safe\n"
                "Open=printf BAD > " + shlex.quote(str(fallback)) + "\nView=%view\n"
            )
            location = str(archive) + "/utar://" + str(left).lstrip("/")
            t.type("cd " + shlex.quote(location))
            t.press("Enter")
            t.wait_for("shadow.safe")
            select_file(t, "shadow.safe")
            t.press("Enter")
            t.wait_for("copy archive members")
            self.assertFalse(fallback.exists())
            t.press("F3")
            t.wait_for("ARCHIVE-CONTENT")
            self.assertNotIn("HOST-SHADOW", t.text())
            self.assertEqual(shadow.read_bytes(), b"HOST-SHADOW\n")
            t.save("extension-provider-boundaries")


if __name__ == "__main__":
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("Extension checks require uid1001 in the approved disposable Docker container")
    BINARY, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    base = Path(sys.argv[3]).resolve()
    if not base.is_relative_to("/review/cases/mc-extensions"):
        raise SystemExit("Extension fixtures must stay in /review/cases/mc-extensions")
    ROOT = base / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
