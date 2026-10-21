"""One actual-App ksh93 workflow in the owned UID-1001 review container.

python3 -B tests/test_shell_ksh_ui.py APP APP_SHA256 KSH /review/cases/shell-ksh-ui-NAME
"""

from pathlib import Path
import hashlib
import json
import os
import shlex
import shutil
import signal
import sys

from terminal_session import TerminalSession


def workflow(binary, expected_hash, shell, root):
    if hashlib.sha256(binary.read_bytes()).hexdigest() != expected_hash:
        raise RuntimeError("App binary does not match the parent-approved SHA256")
    quote = lambda value: shlex.quote(str(value))
    left, right = root / "left", root / "right"
    for path in (left, right):
        path.mkdir(parents=True)
    child = left / "ksh child"
    child.mkdir()
    startup, startup_log = root / "user-env.sh", root / "startup"
    startup.write_text(
        "PS1='MCPP_KSH_UI> '\nset -o emacs\n"
        "export MCPP_KSH_NATIVE_START=user-env\n"
        f"printf '%s\\n' \"$$:$ENV\" >> {quote(startup_log)}\n"
    )
    settings = root / "ui/config/mcpp/config"
    settings.parent.mkdir(parents=True)
    settings.write_text(
        "escape_timeout=50000\nuse_subshell=true\npause_after_run=0\n"
        "clear_before_exec=0\nshow_output_starts_shell=1\n"
    )
    launcher = root / "launch-app"
    launcher.write_text(
        f"#!/bin/sh\nexport SHELL={quote(shell)} ENV={quote(startup)}\n"
        f"export LD_LIBRARY_PATH=/review/control/lib\nexec {quote(binary)} \"$@\"\n"
    )
    launcher.chmod(0o700)
    tmux = shutil.which("tmux")
    if not tmux:
        raise RuntimeError("tmux is missing; actual-App ksh UI was not run")

    def panels(terminal):
        terminal.wait(
            lambda: all(word in terminal.text().splitlines()[0]
                        for word in ("Left", "File", "Command", "Options", "Right")),
            "file manager is visible",
        )

    def show_shell(terminal):
        terminal.press("C-o")
        terminal.wait_for("MCPP_KSH_UI> ")
        terminal.wait(
            lambda: not all(word in terminal.text().splitlines()[0]
                            for word in ("Left", "Command", "Options")),
            "native ksh is visible",
        )

    first, second, value = (root / name for name in ("first-pid", "second-pid", "value"))
    cwd_file, draft = root / "shell-cwd", root / "draft"
    with TerminalSession(launcher, tmux, root / "ui",
                         ["--printwd", root / "cwd", left, right], record_exit=True) as terminal:
        panels(terminal)
        terminal.type(
            "export MCPP_UI_KEPT=kept; mcpp_ui_fn() { printf '%s:%s' "
            "\"$MCPP_KSH_NATIVE_START\" \"$MCPP_UI_KEPT\"; }; alias mcpp_ui_alias=mcpp_ui_fn; "
            f"printf '%s\\n%s\\n' \"$$\" \"${{.sh.version}}\" > {quote(first)}"
        )
        terminal.press("Enter")
        terminal.wait(
            lambda: first.is_file() and len(first.read_bytes().splitlines()) == 2 and
                    b"93u+m/" in first.read_bytes().splitlines()[1],
            "actual ksh records its PID and native version",
        )
        panels(terminal)
        pid = first.read_bytes().splitlines()[0]
        terminal.wait_file(startup_log, pid + b":" + os.fsencode(startup) + b"\n")
        terminal.type(f"mcpp_ui_alias > {quote(value)}; printf '%s' \"$$\" > {quote(second)}")
        terminal.press("Enter")
        terminal.wait_file(value, b"user-env:kept")
        terminal.wait_file(second, pid)
        panels(terminal)
        terminal.save("ksh-persistent-state")

        show_shell(terminal)
        terminal.type(f"cd -- {quote(child)}; printf '%s' \"$PWD\" > {quote(cwd_file)}")
        terminal.press("Enter")
        terminal.wait_file(cwd_file, os.fsencode(child))
        terminal.wait_idle()
        terminal.press("C-o")
        panels(terminal)
        terminal.wait(lambda: "/ksh child" in terminal.text().splitlines()[1][:50],
                      "ksh cwd reaches the active pane")

        show_shell(terminal)
        terminal.type(f"printf UI_DRAFT > {quote(draft)}")
        terminal.wait_for("UI_DRAFT")
        terminal.press("C-o")
        panels(terminal)
        assert not draft.exists(), "hiding ksh executed unfinished input"
        show_shell(terminal)
        terminal.wait_for("UI_DRAFT")
        terminal.press("Enter")
        terminal.wait_file(draft, b"UI_DRAFT")
        terminal.wait_idle()
        terminal.press("C-o")
        panels(terminal)
        terminal.save("ksh-cwd-draft")

        show_shell(terminal)
        terminal.type("exit 23")
        terminal.press("Enter")
        terminal.wait_file(terminal.exit_file, b"0\n")
        terminal.save("ksh-native-exit")
    assert (root / "cwd").read_text() == str(child) + "\n"
    assert list((root / "ui/runtime").iterdir()) == []
    result = {
        "app_sha256": expected_hash, "shell": str(shell), "pid": pid.decode(),
        "passed": ["native ENV once", "persistent PID/export/function/alias",
                   "Ctrl-O cwd synchronization", "unfinished draft restoration", "native exit"],
    }
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    binary, expected_hash, shell, root = sys.argv[1:]
    root = Path(root).resolve()
    if (os.getuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file() or
            root.parent != Path("/review/cases") or not root.name.startswith("shell-ksh-ui-")):
        raise RuntimeError("use UID 1001 in owned Docker and a private shell-ksh-ui-* fixture")
    signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError("ksh UI deadline")))
    signal.alarm(45)
    workflow(Path(binary).resolve(), expected_hash, Path(shell).resolve(), root)
