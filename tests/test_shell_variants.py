"""Four small PTY workflows; run only in the owned UID-1001 review container.

c++ -std=c++20 -pthread -Iinclude src/shell.cpp tests/shell_session_driver.cpp -o DRIVER
python3 -B tests/test_shell_variants.py DRIVER /review/cases/shell-variants-NAME
"""

from pathlib import Path
import errno
import os
import select
import shlex
import shutil
import signal
import subprocess
import sys
import time
import unittest


DRIVER, ROOT = Path(sys.argv.pop(1)).resolve(), Path(sys.argv.pop(1)).resolve()


class Session:
    def __init__(self, root, shell):
        root.mkdir(parents=True)
        home, runtime = root / "home", root / "runtime"
        home.mkdir()
        runtime.mkdir()
        startup = home / ".bashrc"
        startup.write_text("PS1='SHELL_READY> '\nexport MCPP_NATIVE_START=loaded MCPP_STARTUP_KIND=bash\n")
        posix_startup = home / "envrc"
        posix_startup.write_text("PS1='SHELL_READY> '\nexport MCPP_NATIVE_START=loaded MCPP_STARTUP_KIND=posix\n")
        env = dict(os.environ, SHELL=str(shell), HOME=str(home), ENV=str(posix_startup),
                   TMPDIR=str(runtime), TERM="xterm", INPUTRC="/dev/null",
                   XDG_DATA_HOME=str(home / "data"), XDG_CONFIG_HOME=str(home / "config"))
        self.master, slave = os.openpty()
        control, self.input = os.pipe()
        self.events, output = os.pipe()
        self.process = subprocess.Popen(
            [str(DRIVER), str(control), str(output)], stdin=slave, stdout=slave,
            stderr=slave, pass_fds=(control, output), env=env, cwd=root)
        for fd in (slave, control, output):
            os.close(fd)
        self.pending, self.text = bytearray(), bytearray()

    def send(self, *fields):
        os.write(self.input, b"".join(os.fsencode(field) + b"\0" for field in fields))

    def type(self, text):
        os.write(self.master, text.encode() if isinstance(text, str) else text)

    def pump(self, timeout=4):
        ready, _, _ = select.select([self.master, self.events], [], [], timeout)
        if not ready:
            raise AssertionError("shell timed out: " + self.text[-1200:].decode(errors="replace"))
        for fd in ready:
            try:
                data = os.read(fd, 8192)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                data = b""
            if not data:
                raise AssertionError("driver closed: " + self.text[-1200:].decode(errors="replace"))
            if fd == self.events:
                self.pending.extend(data)
            else:
                self.text.extend(data)
                self.text = self.text[-8192:]

    def event(self, kinds=("R", "E", "B", "Y", "U", "I")):
        deadline = time.monotonic() + 5
        while True:
            if self.pending.count(0) >= 6:
                fields = []
                for _ in range(6):
                    end = self.pending.index(0)
                    fields.append(os.fsdecode(bytes(self.pending[:end])))
                    del self.pending[:end + 1]
                if fields[0] in kinds:
                    return fields
            else:
                self.pump(max(0, deadline - time.monotonic()))

    def output(self, marker):
        deadline = time.monotonic() + 5
        while marker.encode() not in self.text:
            self.pump(max(0, deadline - time.monotonic()))

    def command(self, cwd, command):
        self.send("S", str(cwd), command)
        result = self.event()
        if result[0] != "R":
            raise AssertionError(result)
        return result

    def poll_result(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.send("P")
            result = self.event()
            if result[0] != "I":
                return result
            self.event(("N",))
        raise AssertionError("hidden command did not complete")

    def close(self):
        if self.process.poll() is None:
            self.send("C", "Q")
            self.type(b"\x03\x0f")
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("shell shutdown did not finish")
        for fd in (self.master, self.input, self.events):
            os.close(fd)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class ShellVariants(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if (os.getuid() != 1001 or not Path("/.dockerenv").is_file() or
                not Path("/review/.operation-regressions-disposable").is_file() or
                ROOT.parent != Path("/review/cases") or not ROOT.name.startswith("shell-variants-")):
            raise RuntimeError("use UID 1001 and a private /review/cases/shell-variants-* fixture")
        ROOT.mkdir(mode=0o700, exist_ok=True)
        signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError("workflow deadline")))

    def setUp(self):
        self.root = ROOT / self._testMethodName
        if self.root.exists():
            shutil.rmtree(self.root)
        signal.alarm(20)

    def tearDown(self):
        signal.alarm(0)

    def state(self, shell, draft=False):
        with Session(self.root, shell) as session:
            child = self.root / "child '$() \nline\n"
            child.mkdir()
            first, second, value = (self.root / name for name in ("pid1", "pid2", "value"))
            quote = shlex.quote
            result = session.command(self.root,
                f"printf '%s' \"$$\" > {quote(str(first))}; export MCPP_KEPT=kept; "
                "kept_fn() { printf '%s:%s' \"$MCPP_NATIVE_START\" \"$MCPP_KEPT\"; }; alias kept_alias=kept_fn")
            self.assertEqual(result[1:4], ["0", "0", "0"])
            result = session.command(self.root,
                f"kept_alias > {quote(str(value))}; printf '%s' \"$$\" > {quote(str(second))}\n"
                f"cd -- {quote(str(child))}\nfalse")
            self.assertEqual(result[1:5], ["1", "0", "0", str(child)])
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(value.read_text(), "loaded:kept")
            if draft:
                target = self.root / "draft"
                session.send("S", str(child), "")
                session.type(f"printf DRAFT_VALUE > {quote(str(target))}")
                session.output("DRAFT_VALUE")
                session.type(b"\x0f")
                self.assertEqual(session.event()[0], "R")
                session.send("S", str(child), "printf blocked")
                self.assertEqual(session.event()[0], "B")
                session.send("S", str(self.root), "")
                session.type("\n")
                session.event(("N",))
                session.type(b"\x0f")
                self.assertEqual(session.event()[4], str(self.root))
                self.assertEqual(target.read_text(), "DRAFT_VALUE")
            result = session.command(self.root, "exit 23")
            self.assertEqual(result[1:4], ["23", "1", "0"])

    def test_bash_state_cwd_status_exit(self):
        self.state("/bin/bash")

    def test_sh_dash_state_cwd_draft_status_exit(self):
        self.state("/bin/sh", draft=True)

    def test_dash_running_builtin_async_completion_and_shutdown(self):
        with Session(self.root, "/bin/dash") as session:
            child, gate, pidfile = (self.root / name for name in ("child", "gate", "pid"))
            child.mkdir()
            os.mkfifo(gate, 0o600)
            session.send("S", str(self.root),
                f"printf '%s' \"$$\" > {shlex.quote(str(pidfile))}; printf 'RUNNING_%s\\n' READY; "
                f"read -r answer < {shlex.quote(str(gate))}; cd -- {shlex.quote(str(child))}; false")
            session.output("RUNNING_READY")
            session.type(b"\x0f")
            self.assertEqual(session.event()[3], "1")
            session.send("S", str(self.root), "printf blocked")
            self.assertEqual(session.event()[0], "B")
            with gate.open("w") as writer:
                writer.write("complete\n")
            self.assertEqual(session.poll_result()[1:5], ["1", "0", "0", str(child)])
            session.send("S", str(child),
                "printf 'BUILTIN_%s\\n' READY; read -r answer; printf 'SHUTDOWN_%s\\n' READY; read -r again")
            session.output("BUILTIN_READY")
            session.type(b"\x0f")
            self.assertEqual(session.event()[3], "1")
            session.send("S", str(child), "")
            session.type("resumed\n")
            session.output("SHUTDOWN_READY")
            session.type(b"\x0f")
            self.assertEqual(session.event()[3], "1")
            pid = int(pidfile.read_text())
            session.send("C")
            self.assertEqual(session.event()[0], "R")
            self.assertFalse(Path(f"/proc/{pid}").exists())
            self.assertEqual(list((self.root / "runtime").iterdir()), [])

    def test_executable_identity_missing_shell_and_directory_error(self):
        self.root.mkdir()
        selected = self.root / "selected-shell"
        selected.symlink_to("/bin/dash")
        with Session(self.root / "alias", selected) as session:
            session.send("A")
            self.assertEqual(session.event()[0], "Y")
            value = self.root / "identity"
            session.command(self.root, f"printf '%s\\n%s' \"$SHELL\" \"$0\" > {shlex.quote(str(value))}")
            self.assertEqual(value.read_text(), f"{selected}\nselected-shell")
            blocked = self.root / "must-not-run"
            session.send("S", str(self.root / "missing-directory"),
                         f"printf unexpected > {shlex.quote(str(blocked))}")
            result = session.event()
            self.assertEqual(result[0], "E")
            self.assertIn("cannot change directory", result[5])
            session.send("S", str(self.root), "")
            session.type("printf 'RESUME_%s\\n' SAFE\n")
            session.output("RESUME_SAFE")
            session.type(b"\x0f")
            self.assertEqual(session.event()[0], "R")
            self.assertFalse(blocked.exists())
        bash_mode = self.root / "bash-identity"
        bash_mode.mkdir()
        bash_sh = bash_mode / "sh"
        bash_sh.symlink_to("/bin/bash")
        with Session(self.root / "bash-sh", bash_sh) as session:
            value = self.root / "posix-startup"
            result = session.command(self.root,
                f"printf '%s\\n%s' \"$MCPP_STARTUP_KIND\" \"$0\" > {shlex.quote(str(value))}; false")
            self.assertEqual(result[1], "1")
            self.assertEqual(value.read_text(), "posix\nsh")
            self.assertEqual(session.command(self.root, "exit 23")[1:3], ["23", "1"])
        with Session(self.root / "missing", self.root / "no-such-shell") as session:
            session.send("A")
            result = session.event()
            self.assertEqual(result[0], "E")
            self.assertIn("missing or not executable", result[5])
        with Session(self.root / "unsupported", "/usr/bin/env") as session:
            session.send("A")
            self.assertEqual(session.event()[0], "U")
            session.send("S", str(self.root), "")
            self.assertIn("unsupported", session.event()[5])


if __name__ == "__main__":
    unittest.main(verbosity=2)
