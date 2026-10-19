from contextlib import contextmanager
from pathlib import Path
import json
import os
import sys
import unittest
from unittest.mock import patch

from terminal_session import TerminalSession
from test_audit_ui import click, menu, save


FAKE_ASPELL = r'''#!/usr/bin/python3
from pathlib import Path
import json
import os
import re
import stat
import sys

arguments = sys.argv[1:]
data = sys.stdin.buffer.read() if "-a" in arguments else b""
state = os.fstat(0)
with open(os.environ["SPELL_LOG"], "a") as log:
    log.write(json.dumps({"arguments": arguments, "input": data.hex(),
                          "regular": stat.S_ISREG(state.st_mode),
                          "mode": stat.S_IMODE(state.st_mode)}) + "\n")
fail = Path(os.environ["SPELL_FAIL"])
if fail.exists() or "--lang=missing" in arguments:
    print("controlled dictionary failure", file=sys.stderr)
    raise SystemExit(7)
if arguments == ["dump", "dicts"]:
    print("en\nen_US\nmissing")
    raise SystemExit(0)
if "--byte-offsets" not in arguments or "--encoding=utf-8" not in arguments:
    raise SystemExit(8)
personal = Path.home() / ".aspell-test-personal"
accepted = set(personal.read_text().splitlines()) if personal.exists() else set()
print("@(#) controlled Aspell protocol")
for line in data.splitlines():
    if line.startswith(b"*"):
        accepted.add(line[1:].decode())
    elif line == b"#":
        personal.write_text("\n".join(sorted(accepted)) + "\n")
    elif line.startswith(b"^"):
        if Path(os.environ["SPELL_CORRUPT"]).exists():
            print("& teh 1 999999: the")
        else:
            found = False
            for match in re.finditer(rb"(?<![A-Za-z])(?:teh|wrng|mcppword)(?![A-Za-z])", line[1:]):
                word = match.group().decode()
                if word in accepted:
                    continue
                found = True
                offset = match.start() + 1
                suggestions = {"teh": ["the", "tech"], "wrng": ["wrong"]}.get(word, [])
                if suggestions:
                    print(f"& {word} {len(suggestions)} {offset}: " + ", ".join(suggestions))
                else:
                    print(f"# {word} {offset}")
            if not found:
                print("*")
        print()
    else:
        print("unprotected Aspell protocol input", file=sys.stderr)
        raise SystemExit(9)
'''


class SpellingUI(unittest.TestCase):
    def fixture(self, text=b"teh wrng\n"):
        root = ROOT / self._testMethodName
        root.mkdir(parents=True)
        path = root / "draft.txt"
        path.write_bytes(text)
        binary = root / "bin/aspell"
        binary.parent.mkdir()
        binary.write_text(FAKE_ASPELL)
        binary.chmod(0o700)
        config = root / "ui/config/mcpp/config"
        config.parent.mkdir(parents=True)
        config.write_text("escape_timeout=50000\neditor_spelling_language=en\n")
        return root, path

    @contextmanager
    def editor(self, root, path, width=100, height=30, fake=True):
        environment = {
            "SPELL_LOG": str(root / "calls.jsonl"),
            "SPELL_FAIL": str(root / "fail"),
            "SPELL_CORRUPT": str(root / "corrupt"),
        }
        if fake:
            environment["PATH"] = str(root / "bin") + os.pathsep + os.environ["PATH"]
        with patch.dict(os.environ, environment):
            with TerminalSession(BINARY, TMUX, root / "ui", ["--edit", path], width, height,
                                 record_exit=True) as terminal:
                if width >= 80:
                    terminal.wait_editor(offset=0)
                else:
                    terminal.wait(lambda: "[----]" in terminal.text().splitlines()[0], "compact editor header")
                yield terminal

    def whole_file(self, terminal):
        menu(terminal, "Command", "Spell check")
        terminal.wait_for("Check word")
        terminal.wait_for("Misspelled:")

    def test_whole_file_utf8_offsets_replacements_marks_undo_and_add_word(self):
        original = "café teh wrng\nKEEP teh\n".encode()
        expected = "café the wrong\nKEEP teh\n".encode()
        root, path = self.fixture(original)
        with self.editor(root, path) as terminal:
            terminal.press("Down", "Home", "F3", "Right", "Right", "Right", "Right", "F3", "C-Home", "Home")
            terminal.wait_editor(selection=4)
            self.whole_file(terminal)
            terminal.wait_for("Misspelled: teh")
            for label in ("Language:", "Suggest", "[ Add word ]", "[ Replace ]", "[ Skip ]", "[ Cancel ]"):
                self.assertIn(label, terminal.text())
            terminal.save("check-word")
            click(terminal, "[ Replace ]")
            terminal.wait_for("Misspelled: wrng")
            click(terminal, "[ Replace ]")
            terminal.wait_for("Misspelled: teh")
            self.assertEqual(path.read_bytes(), original)
            click(terminal, "[ Add word ]")
            terminal.wait(lambda: "Check word" not in terminal.text() and "Running..." not in terminal.text(),
                          "personal dictionary update and remaining-word check complete")
            terminal.wait_editor(dirty=True, selection=4)
            terminal.wait(lambda: "Running..." not in terminal.text(), "dictionary update completes")
            personal = terminal.root / "home/.aspell-test-personal"
            terminal.wait_file(personal, b"teh\n")
            save(terminal, path, expected)
            terminal.press("F8")
            save(terminal, path, "café the wrong\n teh\n".encode())
            terminal.press("C-u")
            save(terminal, path, expected)
        calls = [json.loads(line) for line in (root / "calls.jsonl").read_text().splitlines()]
        checks = [call for call in calls if "-a" in call["arguments"]]
        self.assertTrue(all(call["regular"] and call["mode"] == 0o600 for call in checks))
        self.assertIn("--byte-offsets", checks[0]["arguments"])
        self.assertEqual(bytes.fromhex(checks[0]["input"]), b"^caf\xc3\xa9 teh wrng\n^KEEP teh\n^\n")

    def test_check_word_only_changes_the_word_at_the_cursor(self):
        root, path = self.fixture()
        with self.editor(root, path) as terminal:
            terminal.press("C-p")
            terminal.wait_for("Misspelled: teh")
            click(terminal, "[ Replace ]")
            terminal.wait_for("Check word", present=False)
            save(terminal, path, b"the wrng\n")

    def test_skip_and_cancel_do_not_modify_the_draft_or_dictionary(self):
        root, path = self.fixture()
        with self.editor(root, path) as terminal:
            self.whole_file(terminal)
            click(terminal, "[ Skip ]")
            terminal.wait_for("Misspelled: wrng")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Check word", present=False)
            terminal.wait_editor(dirty=False)
            self.assertFalse((terminal.root / "home/.aspell-test-personal").exists())
        self.assertEqual(path.read_bytes(), b"teh wrng\n")

    def test_empty_suggestion_list_has_a_visible_replace_error(self):
        root, path = self.fixture(b"mcppword\n")
        with self.editor(root, path) as terminal:
            self.whole_file(terminal)
            terminal.wait_for("(no suggestions)")
            click(terminal, "[ Replace ]")
            terminal.wait_for("no replacement suggestion is selected")
            self.assertEqual(path.read_bytes(), b"mcppword\n")
            click(terminal, "[ Skip ]")
            terminal.wait_for("Check word", present=False)
            terminal.wait_editor(dirty=False)

    def test_failed_check_preserves_the_unsaved_draft(self):
        root, path = self.fixture()
        (root / "fail").touch()
        with self.editor(root, path) as terminal:
            terminal.type("X")
            before = terminal.wait_editor(dirty=True)
            menu(terminal, "Command", "Spell check")
            terminal.wait_for("controlled dictionary failure")
            terminal.press("Enter")
            terminal.wait_for("controlled dictionary failure", present=False)
            terminal.wait_editor(**before)
        self.assertEqual(path.read_bytes(), b"teh wrng\n")

    def test_bad_offsets_are_rejected_without_changing_the_draft(self):
        root, path = self.fixture()
        (root / "corrupt").touch()
        with self.editor(root, path) as terminal:
            menu(terminal, "Command", "Spell check")
            terminal.wait_for("Aspell word offsets do not match")
            terminal.press("Enter")
            terminal.wait_for("Aspell word offsets do not match", present=False)
            terminal.wait_editor(dirty=False)
        self.assertEqual(path.read_bytes(), b"teh wrng\n")

    def test_failed_add_word_retains_the_dialog_and_can_be_retried(self):
        root, path = self.fixture(b"teh\n")
        with self.editor(root, path) as terminal:
            self.whole_file(terminal)
            (root / "fail").touch()
            click(terminal, "[ Add word ]")
            terminal.wait(lambda: "controlled dictionary failure" in
                          " ".join(terminal.text().replace("│", "").split()), "full dictionary error is visible")
            self.assertIn("Misspelled: teh", terminal.text())
            self.assertFalse((terminal.root / "home/.aspell-test-personal").exists())
            (root / "fail").unlink()
            click(terminal, "[ Add word ]")
            terminal.wait(lambda: "Check word" not in terminal.text() and "Running..." not in terminal.text(),
                          "retried personal dictionary update completes")
            terminal.wait_file(terminal.root / "home/.aspell-test-personal", b"teh\n")
            terminal.wait_editor(dirty=False)
        self.assertEqual(path.read_bytes(), b"teh\n")

    def test_language_selection_is_validated_and_persisted(self):
        root, path = self.fixture()
        with self.editor(root, path) as terminal:
            menu(terminal, "Command", "Change spelling language...")
            terminal.wait_for("Select language")
            terminal.save("language")
            terminal.press("Home", "Down", "Enter")
            terminal.wait(lambda: "Select language" not in terminal.text() and "Running..." not in terminal.text(),
                          "selected dictionary is validated")
            self.whole_file(terminal)
            terminal.wait_for("Language: en_US")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Check word", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            self.assertIn("editor_spelling_language=en_US", (terminal.root / "config/mcpp/config").read_text())

    def test_unloadable_dictionary_does_not_replace_the_selected_language(self):
        root, path = self.fixture()
        with self.editor(root, path) as terminal:
            menu(terminal, "Command", "Change spelling language...")
            terminal.wait_for("Select language")
            terminal.press("End", "Enter")
            terminal.wait_for("controlled dictionary failure")
            terminal.press("Enter")
            terminal.wait_for("controlled dictionary failure", present=False)
            self.whole_file(terminal)
            terminal.wait_for("Language: en")
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Check word", present=False)
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            self.assertIn("editor_spelling_language=en\n", (terminal.root / "config/mcpp/config").read_text())

    def test_document_lines_cannot_become_dictionary_commands(self):
        original = b"*teh\n#\n@wrng\n\x00teh\n"
        root, path = self.fixture(original)
        with self.editor(root, path) as terminal:
            self.whole_file(terminal)
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Check word", present=False)
            self.assertFalse((terminal.root / "home/.aspell-test-personal").exists())
        call = json.loads((root / "calls.jsonl").read_text().splitlines()[0])
        self.assertEqual(bytes.fromhex(call["input"]), b"^*teh\n^#\n^@wrng\n^ teh\n^\n")
        self.assertEqual(path.read_bytes(), original)

    def test_short_terminal_keeps_suggestions_and_all_buttons_visible(self):
        root, path = self.fixture()
        with self.editor(root, path, width=40, height=12) as terminal:
            terminal.press("C-p")
            terminal.wait_for("Check word")
            terminal.wait_for("Misspelled: teh")
            for label in ("Add word", "Replace", "Skip", "Cancel", "the"):
                self.assertIn(label, terminal.text())
            click(terminal, "[ Cancel ]")
            terminal.wait_for("Check word", present=False)

    def test_missing_spelling_tool_is_an_explicit_error(self):
        root, path = self.fixture()
        with self.editor(root, path, fake=False) as terminal:
            menu(terminal, "Command", "Spell check")
            terminal.wait_for("start local command")
            terminal.press("Enter")
            terminal.wait_for("start local command", present=False)
            terminal.wait_editor(dirty=False)
        self.assertEqual(path.read_bytes(), b"teh wrng\n")


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Spelling checks require the owned non-root disposable container")
    BINARY, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    ROOT = Path(sys.argv[3]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
