from pathlib import Path
import os
import shlex
import sys
import unittest

from terminal_session import TerminalSession


class ViewerSourcesUI(unittest.TestCase):
    def streamed_stdin(self, binary, arguments):
        root = ROOT / self._testMethodName
        root.mkdir(parents=True)
        source = root / "input.fifo"
        os.mkfifo(source)
        producer = os.open(source, os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
        first = b"FIRST_STREAM\nq\x1b[21~\x03\n"
        second = b"SECOND_STREAM\n"
        try:
            os.write(producer, first)
            command = (
                "exec " + shlex.join([str(binary), *arguments])
                + " < " + shlex.quote(str(source))
            )
            with TerminalSession("/bin/bash", TMUX, root / "ui",
                                 ["-c", command], width=100, height=24,
                                 record_exit=True, record_stderr=True) as ui:
                ui.wait_for("FIRST_STREAM")
                ui.wait_for(f"/{len(first)}+")
                ui.press("F4", "Tab", "Right")
                ui.wait_for("@1/")
                ui.wait(lambda: ui.styled_rows()[1]["focused"] == "I",
                        "ASCII cursor remains separate from streamed data")
                self.assertIn("46 49 52 53 54", ui.text())
                ui.press("F2")
                ui.wait_for("streamed viewer data is read-only")
                ui.press("Enter")
                ui.wait_for("streamed viewer data is read-only", present=False)
                ui.press("F4")
                os.write(producer, second)
                ui.wait_for(f"/{len(first) + len(second)}+")
                ui.press("End")
                ui.wait_for("SECOND_STREAM")
                ui.press("Home")
                ui.wait_for("FIRST_STREAM")
                ui.press("C-l")
                ui.save("streamed-stdin")
                ui.press("F10")
                ui.wait(lambda: ui.exit_file.is_file(),
                        "viewer exits while the producer still has no EOF",
                        timeout=2)
                self.assertEqual(ui.exit_file.read_bytes(), b"0\n")
        finally:
            os.close(producer)
            source.unlink(missing_ok=True)

    def test_standalone_stdin(self):
        self.streamed_stdin(VIEWER, ["/dev/stdin"])

    def test_manager_stdin(self):
        self.streamed_stdin(MANAGER, ["--view", "/dev/stdin"])


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() != 1001 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Viewer source checks require the owned uid1001 disposable container")
    MANAGER = Path(sys.argv[1]).resolve()
    VIEWER = Path(sys.argv[2]).resolve()
    TMUX = sys.argv[3]
    parent = Path(sys.argv[4]).resolve()
    if not str(parent).startswith("/review/cases/streamed-viewer/"):
        raise SystemExit("Use a dedicated streamed-viewer child directory")
    ROOT = parent / f"ui-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[5:]], verbosity=2)
