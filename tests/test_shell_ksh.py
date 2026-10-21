"""One focused ksh93 workflow in the owned UID-1001 review container.

python3 -B tests/test_shell_ksh.py DRIVER KSH /review/cases/shell-ksh-model-NAME
"""

from pathlib import Path
import errno
import json
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
        home, runtime = root / "home", root / "runtime"
        for path in (home, runtime):
            path.mkdir(parents=True)
        startup = root / "user-env.sh"
        startup.write_text(
            "PS1='MCPP_KSH_READY> '\n"
            "set -o emacs\n"
            "export MCPP_NATIVE_START=user-env\n"
            f"printf '%s\\n' \"$$:$ENV\" >> {shlex.quote(str(root / 'startup'))}\n"
        )
        (home / ".kshrc").write_text("export MCPP_WRONG_RC=loaded\n")
        selected = root / "native-ksh"
        selected.symlink_to(shell)
        self.master, slave = os.openpty()
        self.settings = termios.tcgetattr(self.master)
        control, self.input = os.pipe()
        self.events, output = os.pipe()
        self.process = subprocess.Popen(
            [str(driver), str(control), str(output)], stdin=slave, stdout=slave,
            stderr=slave, pass_fds=(control, output), cwd=root,
            env=dict(os.environ, HOME=str(home), ENV=str(startup), SHELL=str(selected),
                     TERM="xterm", TMPDIR=str(runtime), HISTFILE=str(home / "history"),
                     LD_LIBRARY_PATH="/review/control/lib", LC_ALL="C",
                     XDG_CONFIG_HOME=str(home / "config"), XDG_DATA_HOME=str(home / "data")),
        )
        for fd in (slave, control, output):
            os.close(fd)
        self.pending, self.text, self.history = bytearray(), bytearray(), bytearray()

    def send(self, *fields):
        os.write(self.input, b"".join(os.fsencode(field) + b"\0" for field in fields))

    def type(self, text):
        os.write(self.master, text.encode() if isinstance(text, str) else text)

    def pump(self, timeout):
        ready, _, _ = select.select([self.master, self.events], [], [], max(0, timeout))
        if not ready:
            raise AssertionError("ksh timed out: " + self.text[-2000:].decode(errors="replace"))
        for fd in ready:
            try:
                data = os.read(fd, 8192)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                data = b""
            if not data:
                raise AssertionError("driver closed: " + self.text[-2000:].decode(errors="replace"))
            (self.pending if fd == self.events else self.text).extend(data)
            if fd == self.master:
                self.history.extend(data)

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
        assert result == expected, (result, expected, self.text[-2000:])
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
            (self.root / "model.pty").write_bytes(self.history)
            for fd in (self.master, self.input, self.events):
                os.close(fd)


def workflow(driver, shell, root):
    quote = lambda value: shlex.quote(str(value))
    session = Session(driver, shell, root)
    try:
        session.send("A")
        assert session.event()[0] == "Y"
        first, second, identity, value = (
            root / name for name in ("first-pid", "second-pid", "identity", "value")
        )
        child = root / "child '$() \nline\n"
        child.mkdir()
        session.command(root,
            f"printf '%s' \"$$\" > {quote(first)}; "
            f"printf '%s\\n%s\\n%s\\n%s\\n%s' \"${{.sh.version}}\" \"$0\" \"$MCPP_NATIVE_START\" "
            f"\"${{MCPP_WRONG_RC-unset}}\" \"$ENV\" > {quote(identity)}; "
            "export MCPP_KEPT=kept; kept_fn() { printf '%s:%s' \"$MCPP_NATIVE_START\" \"$MCPP_KEPT\"; }; "
            "alias kept_alias=kept_fn")
        identity_lines = identity.read_text().splitlines()
        assert "93u+m/1.0.8" in identity_lines[0], identity_lines
        assert identity_lines[1:] == ["native-ksh", "user-env", "unset", str(root / "user-env.sh")]
        pid = first.read_bytes()
        assert (root / "startup").read_bytes() == pid + b":" + os.fsencode(root / "user-env.sh") + b"\n"
        session.command(root,
            f"kept_alias > {quote(value)}; printf '%s' \"$$\" > {quote(second)}\n"
            f"cd -- {quote(child)}\nfalse", status="1", directory=child)
        assert first.read_bytes() == second.read_bytes()
        assert value.read_bytes() == b"user-env:kept"
        session.command(child,
            f"/bin/sh -c 'printf \"%s\" \"$MCPP_KEPT\"' > {quote(value)}; "
            "printf 'P\\0009\\000/\\000H\\000/\\0000\\000R\\000/\\0000\\000'; false",
            status="1")
        assert value.read_bytes() == b"kept"

        draft = root / "draft"
        session.text.clear()
        session.send("S", child, "")
        session.output("MCPP_KSH_READY> ")
        session.type(f"printf DRAFT_VALUE > {quote(draft)}")
        session.output("DRAFT_VALUE")
        assert session.hide()[0] == "R"
        assert not draft.exists(), "Ctrl-O executed the draft"
        session.send("S", root, "printf blocked")
        result = session.event()
        assert result[0] == "B" and "unfinished input" in result[5], result
        session.text.clear()
        session.send("S", root, "")
        session.type("\n")
        session.file(draft, b"DRAFT_VALUE")
        session.output("MCPP_KSH_READY> ")
        assert session.hide()[4] == str(root)

        completed = root / "completion-target"
        completed.touch()
        session.text.clear()
        session.send("S", root, "")
        session.output("MCPP_KSH_READY> ")
        session.type("printf completed > completion-ta\t\n")
        session.file(completed, b"completed")
        session.output("MCPP_KSH_READY> ")
        assert session.hide()[0] == "R"

        blocked = root / "must-not-run"
        session.send("S", root / "missing-directory", f"printf unsafe > {quote(blocked)}")
        result = session.event()
        assert result[0] == "E" and "cannot change directory" in result[5], result
        assert not blocked.exists()
        assert termios.tcgetattr(session.master) == session.settings
        session.command(root, f"printf '%s' \"$$\" > {quote(second)}")
        assert second.read_bytes() == pid, "directory failure replaced ksh"

        gate = root / "gate"
        os.mkfifo(gate, 0o600)
        session.text.clear()
        session.send("S", root,
            f"printf 'RUNNING_%s\\n' READY; read -r answer < {quote(gate)}; "
            f"cd -- {quote(child)}; false")
        session.output("RUNNING_READY")
        assert session.hide()[3] == "1"
        session.send("S", root, "printf blocked")
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
        session.send("S", child, "printf 'CANCEL_%s\\n' READY; sleep 20")
        session.output("CANCEL_READY")
        assert session.hide()[3] == "1"
        session.text.clear()
        session.send("S", child, "")
        deadline = time.monotonic() + 2
        while termios.tcgetattr(session.master)[3] & termios.ICANON:
            assert time.monotonic() < deadline, "shell did not restore raw input"
            time.sleep(0.005)
        session.type(b"\x03")
        result = session.event()
        assert result[1:4] == ["258", "0", "0"], (result, session.text[-2000:])
        assert termios.tcgetattr(session.master) == session.settings

        job = root / "job-pid"
        session.command(child, f"sleep 20 & mcpp_job=$!; printf '%s' \"$mcpp_job\" > {quote(job)}")
        os.kill(int(job.read_text()), 0)
        session.command(child, "kill -TERM \"$mcpp_job\"; wait \"$mcpp_job\"", status="271")
        session.command(child, f"printf '%s' \"$?\" > {quote(value)}")
        assert value.read_bytes() == b"271", value.read_bytes()
        session.command(child, "if then", status="3")
        session.command(child,
            f"printf '%s' \"$$\" > {quote(second)}; kept_alias > {quote(value)}")
        assert second.read_bytes() == pid, "syntax recovery replaced ksh"
        assert value.read_bytes() == b"user-env:kept"
        session.command(child, "exit 23", status="23", exited="1")
        assert list((root / "runtime").iterdir()) == []
        result = {
            "shell": str(shell), "pid": pid.decode(), "identity": identity_lines,
            "native_statuses": {"cancel": 258, "job": 271, "syntax": 3, "exit": 23},
            "passed": [
                "native ENV once", "persistent PID/export/function/alias/cwd/status",
                "private protocol", "native editor/draft/completion",
                "directory-error restoration", "hidden builtin completion",
                "cancel/jobs", "syntax recovery", "native exit and cleanup",
            ],
        }
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    finally:
        session.close()


if __name__ == "__main__":
    driver, shell, root = (Path(value).resolve() for value in sys.argv[1:])
    if (os.getuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file() or
            root.parent != Path("/review/cases") or not root.name.startswith("shell-ksh-model-")):
        raise RuntimeError("use UID 1001 in owned Docker and a private shell-ksh-model-* fixture")
    signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError("ksh workflow deadline")))
    signal.alarm(45)
    workflow(driver, shell, root)
