from pathlib import Path
import hashlib
import json
import os
import sys
import time

from terminal_session import TerminalSession


def helper(path, source):
    path.write_text("#!/bin/sh\n" + source)
    path.chmod(0o700)


def main():
    binary, tmux, directory = sys.argv[1:4]
    root = Path(directory).resolve()
    assert os.getuid() == 1001
    assert Path("/review/.operation-regressions-disposable").is_file()
    assert root.is_relative_to(Path("/review/cases/native-clipboard-commands"))
    binary = Path(binary).resolve()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if len(sys.argv) == 5:
        assert digest == sys.argv[4], "UI requires the parent's matching binary"
    else:
        assert len(sys.argv) == 4
    root.mkdir(parents=True, exist_ok=True)
    path = root / "draft"
    copied = "COPY-ż".encode()
    original = copied + b"\nTAIL\n"
    payload = "PASTE-é $(touch clipboard-ui-injected)\n".encode()
    path.write_bytes(original)
    (root / "payload").write_bytes(payload)
    (root / "store-mode").write_text("normal")
    (root / "paste-mode").write_text("normal")
    helper(
        root / "collect",
        '[ "$#" -eq 1 ] || exit 31\n'
        '[ ! -t 0 ] || exit 32\n'
        '[ "$(wc -c)" -eq 0 ] || exit 33\n'
        '[ "$(stat -c %a -- "$1")" = 600 ] || exit 34\n'
        'case "$(cat store-mode)" in\n'
        '  fail) printf "UI store denied" >&2; exit 7;;\n'
        '  wait) sleep 10;;\n'
        'esac\n'
        'printf "%s" "$1" > raw-path\n'
        'pwd > cwd\n'
        'cat -- "$1" > received\n',
    )
    helper(
        root / "produce",
        '[ "$#" -eq 0 ] || exit 35\n'
        '[ "$(wc -c)" -eq 0 ] || exit 36\n'
        'cat paste-mode > paste-called\n'
        'case "$(cat paste-mode)" in\n'
        '  fail) printf "not pasted"; printf "UI paste denied" >&2; exit 9;;\n'
        '  empty) exit 0;;\n'
        'esac\n'
        'cat payload\n',
    )
    terminal = root / "terminal"
    config = terminal / "config/mcpp"
    config.mkdir(parents=True, exist_ok=True)
    (config / "ini").write_text(
        "[Midnight-Commander]\n"
        "use_subshell=0\n"
        "editor_group_undo=0\n"
        "editor_cursor_after_inserted_block=1\n"
        "editor_drop_selection_on_copy=0\n"
        "[Misc]\n"
        "clipboard_store=./collect\n"
        "clipboard_paste=./produce\n"
    )
    cache = terminal / "cache/mcpp/mcedit.clip"
    results = {"binary": str(binary), "binary_sha256": digest, "launches": 1}
    with TerminalSession(binary, tmux, terminal, ["--edit", path],
                         record_exit=True, record_stderr=True) as session:
        session.wait_editor(row=1, column=1, bytes=len(original), dirty=False)
        session.press("F3", "End", "F3")
        session.wait_editor(selection=len(copied), dirty=False)
        session.raw(b"\x1b[2;5~")
        session.wait_file(root / "received", copied)
        session.wait_editor(selection=len(copied), dirty=False)
        assert (root / "cwd").read_text() == str(root) + "\n"
        assert not Path((root / "raw-path").read_text()).exists()
        assert cache.read_bytes() == b"mcpp-clipboard-v1\nlinear\n" + copied
        results["store"] = "one private raw filename, closed stdin, captured editor directory"

        session.press("Down", "End")
        session.raw(b"\x1b[2;2~")
        session.wait_editor(bytes=len(original) + len(payload), dirty=True,
                            selection=len(copied))
        session.wait_for("PASTE-é $(touch clipboard-ui-injected)")
        assert path.read_bytes() == original
        persisted = cache.read_bytes()
        assert persisted == b"mcpp-clipboard-v1\nlinear\n" + payload
        before = session.editor_state()
        results["paste"] = "literal UTF8 stdout; dirty draft is not saved"

        def unchanged():
            session.wait_editor(**before)
            assert path.read_bytes() == original
            assert cache.read_bytes() == persisted

        (root / "store-mode").write_text("fail")
        session.raw(b"\x1b[3;2~")
        session.wait_for("Editor error")
        session.wait_for("status 7")
        session.wait_for("UI store denied")
        session.save("store-error")
        session.press("Enter")
        session.wait_for("Editor error", present=False)
        unchanged()
        results["failed_cut"] = "reported without changing selection, draft or cache"

        (root / "paste-mode").write_text("fail")
        session.raw(b"\x1b[2;2~")
        session.wait_for("Editor error")
        session.wait_for("status 9")
        session.wait_for("UI paste denied")
        session.save("paste-error")
        session.press("Enter")
        session.wait_for("Editor error", present=False)
        unchanged()
        results["failed_paste"] = "nonzero stdout is not inserted and cache is retained"

        (root / "paste-mode").write_text("empty")
        session.raw(b"\x1b[2;2~")
        session.wait_file(root / "paste-called", b"empty")
        session.wait_idle()
        unchanged()
        results["empty_stdout"] = "no insertion or internal fallback; cache retained"

        (root / "store-mode").write_text("wait")
        started = time.monotonic()
        session.raw(b"\x1b[3;2~")
        session.wait_for("Editor error", timeout=5)
        session.wait_for("timed out after 2 seconds")
        results["timeout_seconds"] = time.monotonic() - started
        assert results["timeout_seconds"] < 4
        session.press("Enter")
        session.wait_for("Editor error", present=False)
        unchanged()

        (root / "store-mode").write_text("normal")
        session.raw(b"\x1b[3;2~")
        session.wait_editor(bytes=len(original) + len(payload) - len(copied),
                            selection=0, dirty=True)
        session.press("C-u")
        session.wait_editor(bytes=len(original) + len(payload), selection=len(copied))
        session.press("C-u")
        session.wait_editor(bytes=len(original), selection=len(copied))
        session.save_editor()
        session.wait_file(path, original)
        session.save("clipboard-final")
        results["undo"] = "one undo restores successful cut; next undo reverses only prior paste"
        session.press("F10")
        session.wait_file(session.exit_file, b"0\n")
    assert not (root / "clipboard-ui-injected").exists()
    diagnostics = (terminal / "stderr.txt").read_text()
    assert not any("clipboard_" in line and "not supported" in line
                   for line in diagnostics.splitlines())
    saved = (config / "ini").read_text()
    assert "clipboard_store=./collect" in saved.replace(" ", "")
    assert "clipboard_paste=./produce" in saved.replace(" ", "")
    results["status"] = "passed"
    (root / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    print("PASS one clipboard UI workflow: native commands, literal data, errors, empty, timeout, undo")


if __name__ == "__main__":
    main()
