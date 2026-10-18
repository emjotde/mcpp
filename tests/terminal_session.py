from pathlib import Path
import re
import shlex
import subprocess
import time
import traceback


SGR = re.compile(r"(\x1b\[[0-9;:]*m)")


class ApplicationExitError(AssertionError):
    pass


def reverse_sgr(reverse, sequence):
    parameters = sequence[2:-1].split(";")
    index = 0
    while index < len(parameters):
        parameter = parameters[index]
        index += 1
        code = int(parameter.split(":")[0] or "0")
        if code in (38, 48, 58):
            # Color components and palette indices are not style commands.
            if ":" not in parameter and index < len(parameters):
                mode = parameters[index]
                index += 1
                if ":" not in mode:
                    index += {"2": 3, "5": 1}.get(mode, 0)
            continue
        if code == 0 or code == 27:
            reverse = False
        elif code == 7:
            reverse = True
    return reverse


class TerminalSession:
    def __init__(self, binary, tmux, root, arguments, width=100, height=30,
                 record_exit=False, record_stderr=False):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.socket = "terminal.sock"
        self.exit_file = self.root / "exit.status"
        self.record_exit = record_exit
        self.stderr_file = self.root / "stderr.txt"
        self.tmux = tmux
        self.width = width
        environment = {
            "HOME": str(self.root / "home"),
            "XDG_CONFIG_HOME": str(self.root / "config"),
            "XDG_CACHE_HOME": str(self.root / "cache"),
            "XDG_DATA_HOME": str(self.root / "data"),
            "TERM": "xterm-256color",
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
            "SHELL": "/bin/bash",
            "TMPDIR": str(self.root / "runtime"),
            "DISPLAY": "",
            "WAYLAND_DISPLAY": "",
        }
        for name in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR"):
            Path(environment[name]).mkdir(parents=True, exist_ok=True)
        command = ["env", *(f"{key}={value}" for key, value in environment.items()),
                   str(Path(binary).resolve()), *map(str, arguments)]
        command = shlex.join(command)
        if record_stderr:
            command += " 2> " + shlex.quote(str(self.stderr_file))
        if record_exit:
            self.exit_file.unlink(missing_ok=True)
            command = (
                "set +e; " + command +
                '; status=$?; printf "%s\\n" "$status" > ' +
                shlex.quote(str(self.exit_file)) + '; exit "$status"'
            )
        self.run("new-session", "-d", "-s", "test", "-x", str(width),
                 "-y", str(height), command)
        self.run("set-option", "-g", "status", "off")
        self.run("set-option", "-g", "remain-on-exit", "on")

    def run(self, *arguments):
        return subprocess.run(
            [self.tmux, "-S", self.socket, "-f", "/dev/null", *arguments],
            cwd=self.root, check=True, capture_output=True, text=True, timeout=15,
        ).stdout

    def press(self, *keys):
        self.run("send-keys", "-t", "test:0.0", *keys)

    def type(self, value):
        self.run("send-keys", "-t", "test:0.0", "-l", "--", value)

    def raw(self, value):
        self.run("send-keys", "-t", "test:0.0", "-H",
                 *(f"{byte:02x}" for byte in value))

    def text(self, ansi=False):
        arguments = ["capture-pane", "-t", "test:0.0", "-p", "-N"]
        if ansi:
            arguments.append("-e")
        return self.run(*arguments)

    def wait(self, predicate, description, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if self.record_exit and self.exit_file.is_file():
                status = self.exit_file.read_text().strip()
                if status and status != "0":
                    self.save("failure")
                    raise ApplicationExitError(
                        f"Application exited with status {status} while waiting for {description}\n"
                        f"{self.text()}"
                    )
            time.sleep(0.025)
        self.save("failure")
        raise AssertionError(f"{description}\n{self.text()}")

    def wait_for(self, value, present=True, timeout=12):
        self.wait(lambda: (value in self.text()) == present,
                  f"Expected {value!r}, present={present}", timeout)

    def wait_idle(self, timeout=3, stable=0.12):
        deadline = time.monotonic() + timeout
        previous, since = None, time.monotonic()
        while time.monotonic() < deadline:
            current = self.text()
            if current != previous:
                previous, since = current, time.monotonic()
            elif time.monotonic() - since >= stable:
                return current
            time.sleep(0.025)
        return self.text()

    def step(self, *keys):
        for key in keys:
            self.press(key)
            self.wait_idle()

    def styled_rows(self, ansi=None):
        rows = []
        reverse = False
        for line in (self.text(ansi=True) if ansi is None else ansi).splitlines():
            plain = ""
            spans = []
            for token in SGR.split(line):
                if token.startswith("\x1b["):
                    reverse = reverse_sgr(reverse, token)
                    continue
                start = len(plain)
                plain += token
                if reverse and token:
                    spans.append((start, len(plain)))
            rows.append({
                "text": plain, "spans": spans,
                "focused": "".join(plain[start:end] for start, end in spans).strip(),
            })
        return rows

    def focused_text(self):
        return [row["focused"] for row in self.styled_rows() if row["focused"]]

    def editor_state(self):
        header = self.text().splitlines()[0]
        match = re.search(
            r"\[([A-Z-]{4})\]\s+L:\[(\d+) C:(\d+)/(\d+)\]"
            r"\s+\*\((\d+)/(\d+)/(\d+)b\)", header
        )
        if not match:
            raise AssertionError(f"Editor header not reached: {header}")
        flags, row, column, lines, selection, offset, size = match.groups()
        return {
            "row": int(row), "column": int(column), "lines": int(lines),
            "selection": int(selection), "offset": int(offset), "bytes": int(size),
            "mark": flags[0], "dirty": flags[1] == "M",
        }

    def wait_editor(self, **expected):
        def matches():
            try:
                state = self.editor_state()
                return all(state[key] == value for key, value in expected.items())
            except (AssertionError, IndexError):
                return False
        self.wait(matches, f"Expected editor state {expected}")
        return self.editor_state()

    def confirm_editor_save(self):
        self.wait_for("Save file")
        self.press("Enter")
        self.wait_for("Save file", present=False)
        self.wait_editor(dirty=False)

    def save_editor(self):
        self.press("F2")
        self.confirm_editor_save()

    def wait_file(self, path, expected):
        path = Path(path)
        self.wait(
            lambda: path.is_file() and path.read_bytes() == expected,
            f"Expected exact file bytes {expected!r} in {path}",
        )

    def selected_tree_row(self, side=0):
        start, end = (1, self.width // 2 - 1) if side == 0 else (
            self.width // 2 + 1, self.width - 1
        )
        for row in self.styled_rows()[2:-5]:
            selected = "".join(
                row["text"][max(left, start):min(right, end)]
                for left, right in row["spans"] if left < end and right > start
            )
            if selected.strip():
                return selected.strip()
        return ""

    def save(self, name):
        ansi = self.text(ansi=True)
        (self.root / f"{name}.txt").write_text(SGR.sub("", ansi))
        (self.root / f"{name}.ansi").write_text(ansi)

    def record_output(self):
        self.run(
            "pipe-pane", "-o", "-t", "test:0.0",
            "cat >> " + shlex.quote(str(self.root / "output.ansi")),
        )

    def resize(self, width, height):
        self.width = width
        self.run("resize-window", "-t", "test:0", "-x", str(width),
                 "-y", str(height))

    def close(self):
        try:
            self.run("kill-server")
        except subprocess.CalledProcessError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, error, tb):
        try:
            if error is not None:
                (self.root / "failure.traceback.txt").write_text(
                    "".join(traceback.format_exception(exc_type, error, tb))
                )
                try:
                    self.save("failure")
                except Exception as capture_error:
                    (self.root / "failure-capture.txt").write_text(repr(capture_error))
        finally:
            self.close()
