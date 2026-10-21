"""One actual-App zsh workflow in the owned UID-1001 review container.

python3 -B tests/test_shell_zsh_ui.py APP APP_SHA256 ZSH /review/cases/shell-zsh-ui-NAME
"""

from pathlib import Path
import hashlib
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
    left, right, zdot = (root / name for name in ("left", "right", "zdot"))
    for path in (left, right, zdot):
        path.mkdir(parents=True)
    child = left / "zsh child"
    child.mkdir()
    tools = shell.resolve().parent.parent
    (zdot / ".zshenv").write_text(
        f"module_path=({quote(tools)}/usr/lib/aarch64-linux-gnu/zsh/5.9)\n"
        f"fpath=({quote(tools)}/usr/share/zsh/functions/**/*(N/))\n"
        "export MCPP_ZSH_NATIVE_START=zshenv\n"
    )
    (zdot / ".zshrc").write_text("PROMPT='MCPP_ZSH_UI> '\n")
    settings = root / "ui/config/mcpp/config"
    settings.parent.mkdir(parents=True)
    settings.write_text(
        "escape_timeout=50000\nuse_subshell=true\npause_after_run=0\n"
        "clear_before_exec=0\nshow_output_starts_shell=1\n"
    )
    launcher = root / "launch-app"
    launcher.write_text(
        f"#!/bin/sh\nexport SHELL={quote(shell)} ZDOTDIR={quote(zdot)}\n"
        f"exec {quote(binary)} \"$@\"\n"
    )
    launcher.chmod(0o700)
    tmux = shutil.which("tmux")
    if not tmux:
        raise RuntimeError("tmux is missing; actual-App zsh UI was not run")

    def panels(terminal):
        terminal.wait(
            lambda: all(word in terminal.text().splitlines()[0]
                        for word in ("Left", "File", "Command", "Options", "Right")),
            "file manager is visible",
        )

    def show_shell(terminal):
        terminal.press("C-o")
        terminal.wait_for("MCPP_ZSH_UI> ")
        terminal.wait(
            lambda: not all(word in terminal.text().splitlines()[0]
                            for word in ("Left", "Command", "Options")),
            "native zsh is visible",
        )

    first, second, value = (root / name for name in ("first-pid", "second-pid", "value"))
    cwd_file, draft = root / "shell-cwd", root / "draft"
    with TerminalSession(launcher, tmux, root / "ui",
                         ["--printwd", root / "cwd", left, right], record_exit=True) as terminal:
        panels(terminal)
        terminal.type(
            "export MCPP_UI_KEPT=kept; mcpp_ui_fn() { builtin printf '%s:%s' "
            "\"$MCPP_ZSH_NATIVE_START\" \"$MCPP_UI_KEPT\"; }; alias mcpp_ui_alias=mcpp_ui_fn; "
            f"builtin printf '%s\\n%s' \"$$\" \"$ZSH_VERSION\" > {quote(first)}"
        )
        terminal.press("Enter")
        terminal.wait(lambda: first.is_file() and first.read_bytes().endswith(b"\n5.9"),
                      "actual zsh records its PID and version")
        panels(terminal)
        pid = first.read_bytes().split(b"\n")[0]
        terminal.type(f"mcpp_ui_alias > {quote(value)}; builtin printf '%s' \"$$\" > {quote(second)}")
        terminal.press("Enter")
        terminal.wait_file(value, b"zshenv:kept")
        terminal.wait_file(second, pid)
        panels(terminal)
        terminal.save("zsh-persistent-state")

        show_shell(terminal)
        terminal.type(f"cd -- {quote(child)}; builtin printf '%s' \"$PWD\" > {quote(cwd_file)}")
        terminal.press("Enter")
        terminal.wait_file(cwd_file, os.fsencode(child))
        terminal.wait_idle()
        terminal.press("C-o")
        panels(terminal)
        terminal.wait(lambda: "/zsh child" in terminal.text().splitlines()[1][:50],
                      "zsh cwd reaches the active pane")

        show_shell(terminal)
        terminal.type(f"builtin printf UI_DRAFT > {quote(draft)}")
        terminal.wait_for("UI_DRAFT")
        terminal.press("C-o")
        panels(terminal)
        assert not draft.exists(), "hiding zsh executed unfinished input"
        show_shell(terminal)
        terminal.press("Enter")
        terminal.wait_file(draft, b"UI_DRAFT")
        terminal.wait_idle()
        terminal.press("C-o")
        panels(terminal)
        terminal.save("zsh-cwd-draft")

        show_shell(terminal)
        terminal.type("exit 23")
        terminal.press("Enter")
        terminal.wait_file(terminal.exit_file, b"0\n")
        terminal.save("zsh-native-exit")
    assert (root / "cwd").read_text() == str(child) + "\n"
    assert list((root / "ui/runtime").iterdir()) == []
    print("PASS actual App zsh: persistent PID/state, native startup, Ctrl-O/cwd/draft, native exit")
    print("App SHA256:", expected_hash)


if __name__ == "__main__":
    binary, expected_hash, shell, root = sys.argv[1:]
    root = Path(root).resolve()
    if (os.getuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file() or
            root.parent != Path("/review/cases") or not root.name.startswith("shell-zsh-ui-")):
        raise RuntimeError("use UID 1001 in owned Docker and a private shell-zsh-ui-* fixture")
    signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError("zsh UI deadline")))
    signal.alarm(40)
    workflow(Path(binary).resolve(), expected_hash, Path(shell).resolve(), root)
