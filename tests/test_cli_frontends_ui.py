from pathlib import Path
import os
import re
import subprocess
import sys
import unittest

from terminal_session import TerminalSession


class CliFrontendsUI(unittest.TestCase):
    def deferred_copy(self):
        left, right, ui = ROOT / "copy-left", ROOT / "copy-right", ROOT / "copy-ui"
        left.mkdir()
        right.mkdir()
        plaintext = left / "literal:12"
        plaintext.write_bytes(b"CLI_LITERAL\n")
        compressed = left / "tiny.gz"
        with compressed.open("wb") as output:
            subprocess.run(["gzip", "-c", "--", str(plaintext)],
                           stdout=output, check=True, timeout=5)
        compressed_bytes = compressed.read_bytes()
        evidence = Path("/review/evidence/session")
        evidence.mkdir(parents=True, exist_ok=True)
        with TerminalSession(BINARY, TMUX, ui, ["-u", "--", left, right],
                             width=110, height=32, record_exit=True) as terminal:
            terminal.wait_for("literal:12")
            terminal.press("Home", "Down", "Insert", "End", "F5")
            terminal.wait_for("Copy: literal:12")
            terminal.wait_for("Source mask:")
            terminal.press("C-u")
            terminal.type("copied.txt")
            terminal.press("BTab", "BTab", "C-u")
            terminal.type("tiny.gz/ugz://")
            terminal.press("Enter")
            terminal.wait_for("Source mask:", present=False)
            terminal.wait_for("Copy complete")
            terminal.wait_file(left / "copied.txt", b"CLI_LITERAL\n")
            self.assertFalse((right / "copied.txt").exists())
            self.assertFalse((ui / "copied.txt").exists())
            (evidence / f"{ROOT.name}-sfs-source-copy.txt").write_text(terminal.text())

            terminal.press("End", "F6")
            terminal.wait_for("Move / Rename: literal:12")
            terminal.wait_for("Source mask:")
            terminal.press("C-u")
            terminal.type("must-not-create/blocked.txt")
            terminal.press("BTab", "BTab", "C-u")
            terminal.type("tiny.gz/ugz://")
            terminal.press("Enter")
            terminal.wait_for("source filesystem is read-only")
            text = terminal.text()
            self.assertIn("Move / Rename: literal:12", text)
            self.assertIn("tiny.gz/ugz://", text)
            self.assertIn("must-not-create/blocked.txt", text)
            self.assertFalse((left / "must-not-create").exists())
            self.assertFalse((right / "must-not-create").exists())
            self.assertFalse((ui / "must-not-create").exists())
            self.assertEqual(plaintext.read_bytes(), b"CLI_LITERAL\n")
            self.assertEqual(compressed.read_bytes(), compressed_bytes)
            self.assertEqual(list(right.iterdir()), [])
            (evidence / f"{ROOT.name}-sfs-readonly-move.txt").write_text(text)
            terminal.press("Escape")

    def deferred_ascii(self):
        ui = ROOT / "ui"
        ui.mkdir()
        (ui / "literal:12").write_text("CLI_LITERAL\n")
        with TerminalSession(BINARY, TMUX, ui, ["-a", "--edit", "--", "literal:12"],
                             record_exit=True) as terminal:
            terminal.wait_editor()
            terminal.press("F1")
            terminal.wait_for("mcpp help:")
            text = terminal.wait_idle()
            non_ascii_frames = [
                (row, column, character)
                for row, line in enumerate(text.splitlines(), 1)
                for column, character in enumerate(line, 1)
                if "\u2500" <= character <= "\u257f"
            ]
            evidence = Path("/review/evidence/session")
            evidence.mkdir(parents=True, exist_ok=True)
            (evidence / f"{ROOT.name}-deferred-ascii.txt").write_text(text)
            terminal.press("F10", "F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertFalse(non_ascii_frames, "ASCII frame characters were not converted")

    def test_launch_flags_do_not_replace_saved_preferences(self):
        os.environ.pop("MC_DATADIR", None)
        if ASCII_ONLY:
            self.deferred_ascii()
            return
        if COPY_ONLY:
            self.deferred_copy()
            return
        invalid = ROOT / "invalid/mcpp"
        invalid.mkdir(parents=True)
        (invalid / "ini").write_text("[Midnight-Commander]\neditor_tab_spacing=0\n")
        environment = dict(os.environ, XDG_CONFIG_HOME=str(invalid.parent))
        for name in ("mcpp", "mcppedit", "mcppview", "mcppdiff"):
            result = subprocess.run(
                [str(BINARY.parent / name), "--help-all"], env=environment,
                capture_output=True, text=True, timeout=5,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--nomouse", result.stdout)
            self.assertIn("--colors", result.stdout)
        result = subprocess.run(
            [str(BINARY), "--colors=normal=invalid-color"], env=environment,
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown color", result.stderr)
        self.assertNotIn("\x1b", result.stdout)

        ui = ROOT / "ui"
        saved = ROOT / "saved-pane"
        saved.mkdir()
        (ui / "config/mcpp").mkdir(parents=True)
        preferences = ui / "config/mcpp/config"
        preferences.write_text(
            "theme=classic\nuse_subshell=true\nauto_save_panels_setup=true\n"
            f"saved_left_path={saved}\nsaved_right_path={saved}\n"
        )
        (ui / "literal:12").write_text("CLI_LITERAL\n")
        (saved / "literal:12").write_text("WRONG_SAVED_PANE\n")
        arguments = ["-dab", "-Sforest", "-u", "-P", "last-directory",
                     "--edit", "--", "literal:12"]
        with TerminalSession(BINARY, TMUX, ui, arguments, record_exit=True) as terminal:
            terminal.wait_editor(offset=0, dirty=False)
            terminal.wait_for("CLI_LITERAL")
            self.assertNotIn("WRONG_SAVED_PANE", terminal.text())
            terminal.raw(b"\x1b[<0;6;3M\x1b[<0;6;3m")
            terminal.wait_idle()
            self.assertEqual(terminal.editor_state()["offset"], 0)
            terminal.press("F1")
            terminal.wait_for("mcpp help:")
            terminal.wait_idle()
            text, ansi = terminal.text(), terminal.text(ansi=True)
            self.assertIn("+-", text)
            non_ascii_frames = [
                (row, column, character)
                for row, line in enumerate(text.splitlines(), 1)
                for column, character in enumerate(line, 1)
                if "\u2500" <= character <= "\u257f"
            ]
            for parameters in re.findall(r"\x1b\[([0-9;]*)m", ansi):
                for parameter in parameters.split(";"):
                    code = int(parameter or "0")
                    self.assertFalse(30 <= code <= 38 or 40 <= code <= 48 or 90 <= code <= 107)
            evidence = Path("/review/evidence/session")
            evidence.mkdir(parents=True, exist_ok=True)
            (evidence / f"{ROOT.name}-display.txt").write_text(text)
            (evidence / f"{ROOT.name}-display.ansi").write_text(ansi)
            terminal.press("F10")
            terminal.wait_for("mcpp help:", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual((ui / "last-directory").read_text(), str(saved) + "\n")
        persisted = preferences.read_text()
        self.assertRegex(persisted, r"(?m)^theme=classic$")
        self.assertRegex(persisted, r"(?m)^use_subshell=true$")
        self.assertNotIn("forest", persisted)
        self.assertEqual((ui / "literal:12").read_text(), "CLI_LITERAL\n")

        compressed = ROOT / "tiny.gz"
        with compressed.open("wb") as output:
            subprocess.run(["gzip", "-c", "--", str(ui / "literal:12")],
                           stdout=output, check=True, timeout=5)
        viewer_arguments = ["-dab", "-Sforest", "-u", "--", str(ui / "literal:12") + "/gz://"]
        with TerminalSession(BINARY.parent / "mcppview", TMUX, ROOT / "view-ui",
                             viewer_arguments, record_exit=True) as terminal:
            terminal.wait_for("CLI_LITERAL")
            (evidence / f"{ROOT.name}-sfs-viewer.txt").write_text(terminal.text())
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        diff_arguments = ["-dab", "-Sforest", "-u", "--",
                          str(ui / "literal:12"), str(compressed) + "/ugz://"]
        with TerminalSession(BINARY.parent / "mcppdiff", TMUX, ROOT / "diff-ui",
                             diff_arguments, record_exit=True) as terminal:
            terminal.wait_for("CLI_LITERAL")
            terminal.wait_for("Dif 0")
            (evidence / f"{ROOT.name}-sfs-diff.txt").write_text(terminal.text())
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.deferred_copy()
        self.assertFalse(non_ascii_frames, "ASCII frame characters were not converted")


if __name__ == "__main__":
    if os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("CLI checks require the existing UID1001 disposable container")
    BINARY = Path(sys.argv[1]).resolve()
    TMUX = sys.argv[2]
    parent = Path(sys.argv[3]).resolve()
    if not parent.is_relative_to("/review/cases") or not parent.name.startswith("cli-remaining-"):
        raise SystemExit("Use an owned /review/cases/cli-remaining-* directory")
    ROOT = parent / f"frontends-{os.getpid()}"
    ROOT.mkdir(parents=True)
    ASCII_ONLY = "--ascii-only" in sys.argv[4:]
    COPY_ONLY = "--copy-only" in sys.argv[4:]
    arguments = [argument for argument in sys.argv[4:] if argument not in ("--ascii-only", "--copy-only")]
    unittest.main(argv=[sys.argv[0], *arguments], verbosity=2)
