"""One focused zsh PTY workflow, only in the owned UID-1001 review container.

python3 -B tests/test_shell_zsh.py DRIVER ZSH /review/cases/shell-zsh-model-NAME
"""

from pathlib import Path
import errno
import os
import select
import shlex
import signal
import subprocess
import sys
import termios
import time


class Session:
    def __init__(self, driver, shell, root):
        self.root = root
        home, zdot, runtime = (root / name for name in ("home", "zdot", "runtime"))
        for path in (home, zdot, runtime):
            path.mkdir(parents=True)
        tools = shell.resolve().parent.parent
        (home / ".zshrc").write_text("export MCPP_WRONG_RC=loaded\n")
        (zdot / ".zshenv").write_text(
            f"module_path=({shlex.quote(str(tools))}/usr/lib/aarch64-linux-gnu/zsh/5.9)\n"
            f"fpath=({shlex.quote(str(tools))}/usr/share/zsh/functions/**/*(N/))\n"
            "export MCPP_NATIVE_ENV=zshenv\n"
        )
        (zdot / ".zshrc").write_text(
            "PROMPT='MCPP_ZSH_READY> '\nexport MCPP_NATIVE_START=zshrc\n"
            f"precmd() {{ builtin printf '%s' \"$?\" > {shlex.quote(str(root / 'precmd-status'))}; }}\n"
            f"mcpp_native_hook() {{ builtin printf '%s' \"$?\" > {shlex.quote(str(root / 'hook-status'))}; return 1; }}\n"
            "precmd_functions=(mcpp_native_hook)\n"
            "autoload -Uz compinit\ncompinit -D -u\n"
        )
        selected = root / "native-zsh"
        selected.symlink_to(shell)
        self.master, slave = os.openpty()
        self.settings = termios.tcgetattr(self.master)
        control, self.input = os.pipe()
        self.events, output = os.pipe()
        self.process = subprocess.Popen(
            [str(driver), str(control), str(output)], stdin=slave, stdout=slave,
            stderr=slave, pass_fds=(control, output), cwd=root,
            env=dict(os.environ, HOME=str(home), ZDOTDIR=str(zdot), SHELL=str(selected),
                     TERM="xterm", TMPDIR=str(runtime), INPUTRC="/dev/null",
                     XDG_CONFIG_HOME=str(home / "config"), XDG_DATA_HOME=str(home / "data")),
        )
        for fd in (slave, control, output):
            os.close(fd)
        self.pending, self.text = bytearray(), bytearray()

    def send(self, *fields):
        os.write(self.input, b"".join(os.fsencode(field) + b"\0" for field in fields))

    def type(self, text):
        os.write(self.master, text.encode() if isinstance(text, str) else text)

    def pump(self, timeout):
        ready, _, _ = select.select([self.master, self.events], [], [], max(0, timeout))
        if not ready:
            raise AssertionError("zsh timed out: " + self.text[-1600:].decode(errors="replace"))
        for fd in ready:
            try:
                data = os.read(fd, 8192)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                data = b""
            if not data:
                raise AssertionError("driver closed: " + self.text[-1600:].decode(errors="replace"))
            (self.pending if fd == self.events else self.text).extend(data)

    def event(self):
        deadline = time.monotonic() + 6
        while True:
            if self.pending.count(0) >= 6:
                fields = []
                for _ in range(6):
                    end = self.pending.index(0)
                    fields.append(os.fsdecode(bytes(self.pending[:end])))
                    del self.pending[:end + 1]
                if fields[0] != "N":
                    return fields
            else:
                self.pump(deadline - time.monotonic())

    def output(self, marker):
        deadline = time.monotonic() + 6
        while marker.encode() not in self.text:
            self.pump(deadline - time.monotonic())

    def file(self, path, value):
        deadline = time.monotonic() + 6
        while not path.is_file() or path.read_bytes() != value:
            self.pump(deadline - time.monotonic())

    def command(self, cwd, command, status="0", directory=None, exited="0"):
        self.send("S", cwd, command)
        result = self.event()
        expected = ["R", status, exited, "0", str(directory or cwd), ""]
        assert result == expected, (result, expected, self.text[-1600:])
        assert termios.tcgetattr(self.master) == self.settings, "terminal settings not restored"
        return result

    def hide(self):
        self.type(b"\x0f")
        return self.event()

    def close(self):
        try:
            if self.process.poll() is None:
                self.send("C", "Q")
                self.type(b"\x03\x0f")
                try:
                    self.process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
                    raise AssertionError("shell driver did not close")
        finally:
            for fd in (self.master, self.input, self.events):
                os.close(fd)


def workflow(driver, shell, root):
    quote = lambda path: shlex.quote(str(path))
    session = Session(driver, shell, root)
    try:
        session.send("A")
        assert session.event()[0] == "Y"
        first, second, value = (root / name for name in ("pid-one", "pid-two", "value"))
        identity = root / "identity"
        child = root / "child '$() \nline\n"
        child.mkdir()
        session.command(root,
            f"builtin printf '%s' \"$$\" > {quote(first)}; "
            f"builtin printf '%s\\n%s\\n%s\\n%s\\n%s' \"$ZSH_VERSION\" \"$0\" \"$MCPP_NATIVE_ENV\" "
            f"\"$MCPP_NATIVE_START\" \"${{MCPP_WRONG_RC-unset}}\" > {quote(identity)}; "
            "export MCPP_KEPT=kept; kept_fn() { builtin printf '%s:%s' \"$MCPP_NATIVE_START\" \"$MCPP_KEPT\"; }; "
            "alias kept_alias=kept_fn")
        assert identity.read_text() == "5.9\nnative-zsh\nzshenv\nzshrc\nunset", identity.read_text()
        session.command(root,
            f"kept_alias > {quote(value)}; builtin printf '%s' \"$$\" > {quote(second)}\n"
            f"cd -- {quote(child)}\nfalse", status="1", directory=child)
        assert first.read_bytes() == second.read_bytes()
        assert value.read_bytes() == b"zshrc:kept"
        assert (root / "precmd-status").read_bytes() == b"1"
        assert (root / "hook-status").read_bytes() == b"1"
        session.command(child,
            "builtin printf 'P\\0009\\000/\\000H\\000/\\0000\\000R\\000/\\0000\\000'; false",
            status="1")

        draft = root / "draft"
        session.text.clear()
        session.send("S", child, "")
        session.output("MCPP_ZSH_READY> ")
        session.type(f"builtin printf DRAFT_VALUE > {quote(draft)}")
        session.output("DRAFT_VALUE")
        assert session.hide()[0] == "R"
        session.send("S", root, "builtin printf blocked")
        result = session.event()
        assert result[0] == "B" and "unfinished input" in result[5], result
        session.text.clear()
        session.send("S", root, "")
        session.type("\n")
        session.file(draft, b"DRAFT_VALUE")
        session.output("MCPP_ZSH_READY> ")
        assert session.hide()[4] == str(root)

        completed = root / "completion-target"
        completed.touch()
        session.text.clear()
        session.send("S", root, "")
        session.output("MCPP_ZSH_READY> ")
        session.type("builtin printf completed > completion-ta\t\n")
        session.file(completed, b"completed")
        session.output("MCPP_ZSH_READY> ")
        assert session.hide()[0] == "R"

        blocked = root / "must-not-run"
        session.send("S", root / "missing-directory", f"builtin printf unsafe > {quote(blocked)}")
        result = session.event()
        assert result[0] == "E" and "cannot change directory" in result[5], result
        assert not blocked.exists()
        assert termios.tcgetattr(session.master) == session.settings
        session.command(root, "builtin true")

        gate = root / "gate"
        os.mkfifo(gate, 0o600)
        session.text.clear()
        session.send("S", root,
            f"builtin printf 'RUNNING_%s\\n' READY; read -r answer < {quote(gate)}; "
            f"cd -- {quote(child)}; false")
        session.output("RUNNING_READY")
        assert session.hide()[3] == "1"
        session.send("S", root, "builtin printf blocked")
        assert session.event()[0] == "B"
        writer = os.open(gate, os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(writer, b"complete\n")
        finally:
            os.close(writer)
        deadline = time.monotonic() + 6
        while True:
            session.send("P")
            result = session.event()
            if result[0] != "I":
                break
            assert time.monotonic() < deadline, "hidden completion timed out"
            time.sleep(0.02)
        assert result[1:5] == ["1", "0", "0", str(child)], result

        session.text.clear()
        session.send("S", child, "builtin printf 'CANCEL_%s\\n' READY; read -r answer")
        session.output("CANCEL_READY")
        assert session.hide()[3] == "1"
        session.text.clear()
        session.send("S", child, "")
        deadline = time.monotonic() + 2
        while termios.tcgetattr(session.master)[3] & termios.ICANON:
            assert time.monotonic() < deadline, "shell did not restore raw input"
            time.sleep(0.005)
        session.type(b"\x03")
        session.output("MCPP_ZSH_READY> ")
        result = session.event()
        assert result[1:4] == ["130", "0", "0"], (result, session.text[-1600:])

        job, waited = root / "job-pid", root / "job-status"
        session.command(root, f"sleep 20 & mcpp_job=$!; builtin printf '%s' \"$mcpp_job\" > {quote(job)}")
        os.kill(int(job.read_text()), 0)
        session.command(root,
            f"kill -TERM \"$mcpp_job\"; wait \"$mcpp_job\"; builtin printf '%s' \"$?\" > {quote(waited)}")
        assert waited.read_bytes() == b"143", waited.read_bytes()
        session.command(root, "exit 23", status="23", exited="1")
        assert list((root / "runtime").iterdir()) == []
        print("PASS zsh: native identity/startup/hooks, persistent state/cwd/status, private protocol,")
        print("draft/completion, directory-error restoration, hidden builtin completion, cancel/jobs, exit")
    finally:
        session.close()


if __name__ == "__main__":
    driver, shell, root = (Path(value).resolve() for value in sys.argv[1:])
    if (os.getuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file() or
            root.parent != Path("/review/cases") or not root.name.startswith("shell-zsh-model-")):
        raise RuntimeError("use UID 1001 in the owned Docker and a private shell-zsh-model-* fixture")
    signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError("zsh workflow deadline")))
    signal.alarm(40)
    workflow(driver, shell, root)
