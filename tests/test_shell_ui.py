from contextlib import contextmanager
from pathlib import Path
import hashlib
import os
import shlex
import signal
import sys
import unittest

from terminal_session import TerminalSession
from test_audit_ui import click, select_file


class ShellUI(unittest.TestCase):
    def fixture(self, config=""):
        root = ROOT / self._testMethodName
        left, right = root / "left", root / "right"
        left.mkdir(parents=True)
        right.mkdir()
        settings = root / "ui/config/mcpp/config"
        settings.parent.mkdir(parents=True)
        settings.write_text("escape_timeout=50000\n" + config)
        startup = root / "ui/home/.bashrc"
        startup.parent.mkdir(parents=True)
        startup.write_text("PS1='MCPP_TEST_SHELL> '\n")
        return root, left, right

    @contextmanager
    def terminal(self, root, left, right, options=(), binary=None):
        with TerminalSession(binary or BINARY, TMUX, root / "ui",
                             [*options, "--printwd", root / "cwd", left, right], record_exit=True) as terminal:
            self.panels(terminal)
            yield terminal

    def panels(self, terminal):
        terminal.wait(lambda: all(word in terminal.text().splitlines()[0]
                                  for word in ("Left", "File", "Command", "Options", "Right")),
                      "file manager is visible")

    def command(self, terminal, command, output, expected):
        terminal.type(command)
        terminal.press("Enter")
        terminal.wait_file(output, expected)
        self.panels(terminal)

    def show_shell(self, terminal):
        terminal.press("C-o")
        terminal.wait_for("MCPP_TEST_SHELL> ")
        terminal.wait(lambda: not all(word in terminal.text().splitlines()[0]
                                     for word in ("Left", "Command", "Options")),
                      "shell is visible instead of the panels")

    def test_commander_commands_preserve_pid_variables_functions_and_aliases(self):
        root, left, right = self.fixture()
        first, second, value = root / "pid-one", root / "pid-two", root / "value"
        with self.terminal(root, left, right) as terminal:
            terminal.type(f"export MCPP_KEPT=value; keep_fn() {{ printf '%s' \"$MCPP_KEPT\"; }}; "
                          f"alias keep_alias='keep_fn'; printf '%s' \"$$\" > {shlex.quote(str(first))}")
            terminal.press("Enter")
            terminal.wait(lambda: first.is_file() and first.read_bytes().isdigit(), "first Bash PID is recorded")
            self.panels(terminal)
            pid = first.read_bytes()
            self.command(terminal, f"keep_alias > {shlex.quote(str(value))}; "
                         f"printf '%s' \"$$\" > {shlex.quote(str(second))}", second, pid)
            terminal.wait_file(value, b"value")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_ctrl_o_syncs_cwd_and_preserves_shell_variables(self):
        root, left, right = self.fixture()
        child = left / "child space\nline"
        child.mkdir()
        output = root / "from-commander"
        with self.terminal(root, left, right) as terminal:
            self.show_shell(terminal)
            terminal.type("export MCPP_KEPT=preserved; cd -- " + shlex.quote(str(child)))
            terminal.press("Enter")
            terminal.wait_for("MCPP_TEST_SHELL> ")
            terminal.press("C-o")
            self.panels(terminal)
            terminal.wait(lambda: "child space\\nline" in terminal.text(), "shell cwd reaches the pane without a broken row")
            self.command(terminal, f"printf '%s' \"$MCPP_KEPT\" > {shlex.quote(str(output))}", output, b"preserved")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text(), str(child) + "\n")

    def test_unfinished_readline_input_survives_toggling(self):
        root, left, right = self.fixture()
        output = root / "draft"
        command = "printf kept > " + shlex.quote(str(output))
        with self.terminal(root, left, right) as terminal:
            self.show_shell(terminal)
            terminal.type(command)
            terminal.press("C-o")
            self.panels(terminal)
            self.assertFalse(output.exists())
            self.show_shell(terminal)
            terminal.press("Enter")
            terminal.wait_file(output, b"kept")
            terminal.press("C-o")
            self.panels(terminal)

    def test_unfinished_shell_input_blocks_and_retains_a_commander_command(self):
        root, left, right = self.fixture()
        blocked, original = left / "blocked", root / "original"
        draft = "printf draft > " + shlex.quote(str(original))
        command = "printf second > blocked"
        with self.terminal(root, left, right) as terminal:
            self.show_shell(terminal)
            terminal.type(draft)
            terminal.press("C-o")
            self.panels(terminal)
            terminal.type(command)
            terminal.press("Enter")
            terminal.wait_for("unfinished input")
            self.assertIn(command, terminal.text())
            self.assertFalse(blocked.exists())
            self.show_shell(terminal)
            terminal.press("Enter")
            terminal.wait_file(original, b"draft")
            terminal.press("C-o")
            self.panels(terminal)
            terminal.press("Enter")
            terminal.wait_file(blocked, b"second")
            self.panels(terminal)

    def test_ctrl_o_can_hide_a_running_bash_builtin_and_resume_it(self):
        root, left, right = self.fixture()
        output = root / "answer"
        with self.terminal(root, left, right) as terminal:
            self.show_shell(terminal)
            terminal.type(f"read -r answer; printf '%s' \"$answer\" > {shlex.quote(str(output))}")
            terminal.press("Enter")
            terminal.wait_idle()
            terminal.press("C-o")
            self.panels(terminal)
            self.assertFalse(output.exists())
            terminal.press("C-o")
            terminal.wait(lambda: "Options" not in terminal.text().splitlines()[0], "the running builtin is visible")
            terminal.type("resumed")
            terminal.press("Enter")
            terminal.wait_file(output, b"resumed")
            terminal.press("C-o")
            self.panels(terminal)

    def test_hidden_command_completion_updates_cwd_without_another_key(self):
        root, left, right = self.fixture()
        child = left / "child"
        child.mkdir()
        output = root / "completed"
        with self.terminal(root, left, right) as terminal:
            terminal.type("sleep 0.5; cd child; printf done > " + shlex.quote(str(output)))
            terminal.press("Enter")
            terminal.wait(lambda: "Left" not in terminal.text().splitlines()[0], "commander command enters Bash")
            terminal.press("C-o")
            self.panels(terminal)
            terminal.wait_file(output, b"done")
            terminal.wait(lambda: "/child" in terminal.text().splitlines()[1][:50],
                          "background command completion updates the pane")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text(), str(child) + "\n")

    def test_pause_always_does_not_block_returning_from_a_running_command(self):
        root, left, right = self.fixture("pause_after_run=2\n")
        output = root / "completed"
        with self.terminal(root, left, right) as terminal:
            terminal.type("sleep 0.5; printf done > " + shlex.quote(str(output)))
            terminal.press("Enter")
            terminal.wait(lambda: "Left" not in terminal.text().splitlines()[0], "command enters Bash")
            terminal.press("C-o")
            self.panels(terminal)
            terminal.wait_file(output, b"done")
            self.assertNotIn("Press any key to continue", terminal.text())
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_interactive_shell_exit_exits_the_manager_like_mc(self):
        root, left, right = self.fixture()
        with self.terminal(root, left, right) as terminal:
            self.show_shell(terminal)
            terminal.type("exit 23")
            terminal.press("Enter")
            terminal.wait_file(terminal.exit_file, b"0\n")

    def test_disabled_subshell_retains_one_shot_command_behavior(self):
        root, left, right = self.fixture("use_subshell=false\n")
        first, second = root / "first", root / "second"
        with self.terminal(root, left, right) as terminal:
            self.command(terminal, f"export MCPP_ONESHOT=gone; printf ready > {shlex.quote(str(first))}",
                         first, b"ready")
            self.command(terminal, f"printf '%s' \"${{MCPP_ONESHOT-unset}}\" > {shlex.quote(str(second))}",
                         second, b"unset")

    def test_a_running_shell_blocks_new_commands_without_losing_them(self):
        root, left, right = self.fixture()
        answer, blocked = root / "answer", left / "blocked"
        with self.terminal(root, left, right) as terminal:
            self.show_shell(terminal)
            terminal.type(f"read -r answer; printf '%s' \"$answer\" > {shlex.quote(str(answer))}")
            terminal.press("Enter")
            terminal.wait_idle()
            terminal.press("C-o")
            self.panels(terminal)
            terminal.type("printf second > blocked")
            terminal.press("Enter")
            terminal.wait_for("shell is already running")
            self.assertIn("printf second > blocked", terminal.text())
            self.assertFalse(blocked.exists())
            terminal.press("C-o")
            terminal.wait(lambda: "Options" not in terminal.text().splitlines()[0], "running read resumes")
            terminal.type("first")
            terminal.press("Enter")
            terminal.wait_file(answer, b"first")
            terminal.press("C-o")
            self.panels(terminal)
            terminal.wait_idle()
            terminal.press("Enter")
            terminal.wait_file(blocked, b"second")
            self.panels(terminal)

    def test_initial_logical_pane_path_reaches_bash(self):
        root, left, right = self.fixture()
        alias = root / "logical-link"
        alias.symlink_to(left, target_is_directory=True)
        output = root / "pwd"
        with self.terminal(root, alias, right) as terminal:
            self.command(terminal, f"printf '%s' \"$PWD\" > {shlex.quote(str(output))}",
                         output, str(alias).encode())
            self.assertIn("logical-link", terminal.text().splitlines()[1][:50])

    def test_app_specific_bashrc_and_prompt_hooks_keep_command_status(self):
        root, left, right = self.fixture()
        startup = root / "ui/data/mcpp/bashrc"
        startup.parent.mkdir(parents=True)
        status = root / "hook-status"
        startup.write_text(
            "PS1='MCPP_TEST_SHELL> '\nexport MCPP_APP_RC=loaded\n"
            "mcpp_test_hook() { printf '%s' \"$?\" > " + shlex.quote(str(status)) + "; }\n"
            "PROMPT_COMMAND=(mcpp_test_hook)\n"
        )
        output = root / "rc-value"
        with self.terminal(root, left, right) as terminal:
            self.command(terminal, f"printf '%s' \"$MCPP_APP_RC\" > {shlex.quote(str(output))}",
                         output, b"loaded")
            terminal.type("false")
            terminal.press("Enter")
            terminal.wait_file(status, b"1")
            self.panels(terminal)
            terminal.wait_for("Command exited 1")

    def test_hidden_completion_does_not_undo_manual_pane_navigation(self):
        root, left, right = self.fixture()
        (left / "child").mkdir()
        manual = left / "manual"
        manual.mkdir()
        output = root / "completed"
        with self.terminal(root, left, right) as terminal:
            terminal.type("sleep 0.5; cd child; printf done > " + shlex.quote(str(output)))
            terminal.press("Enter")
            terminal.wait(lambda: "Left" not in terminal.text().splitlines()[0], "command enters Bash")
            terminal.press("C-o")
            self.panels(terminal)
            terminal.press("C-s")
            terminal.wait_for("Quick search:")
            terminal.type("manual")
            terminal.wait_for("Quick search: manual")
            terminal.press("Enter")
            terminal.wait(lambda: "/manual" in terminal.text().splitlines()[1][:50], "user navigates while Bash runs")
            terminal.wait_file(output, b"done")
            terminal.wait_idle()
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
        self.assertEqual((root / "cwd").read_text(), str(manual) + "\n")

    def test_commands_run_alongside_a_paused_file_operation(self):
        root, left, right = self.fixture()
        source, destination = left / "source.bin", right / "source.bin"
        block = b"parallel-shell-copy\n" * 4096
        with source.open("wb") as output:
            for _ in range(256):
                output.write(block)
        expected_hash = hashlib.sha256(source.read_bytes()).digest()
        marker = root / "shell-marker"
        with self.terminal(root, left, right) as terminal:
            select_file(terminal, "source.bin")
            terminal.press("F5")
            terminal.wait_for("Run in background")
            click(terminal, "Run in background")
            terminal.press("Enter", "C-x", "j", "F2")
            terminal.wait_for("Background jobs")
            terminal.wait_for("Paused")
            self.assertFalse(destination.exists())
            terminal.press("Escape")
            terminal.wait_for("Background jobs", present=False)
            self.command(terminal, f"printf parallel > {shlex.quote(str(marker))}", marker, b"parallel")
            self.assertFalse(destination.exists(), "the file operation remains paused while Bash executes")
            terminal.press("C-x", "j")
            terminal.wait_for("Paused")
            terminal.press("F2", "Escape")
            terminal.wait_for("Copy complete")
            self.assertTrue(destination.is_file())
            self.assertEqual(hashlib.sha256(destination.read_bytes()).digest(), expected_hash)

    def test_missing_bash_reports_startup_failure_and_restores_the_ui(self):
        root, left, right = self.fixture()
        wrapper = root / "missing-shell-wrapper"
        wrapper.write_text("#!/bin/sh\nexport SHELL=/mcpp-missing/bash\nexec " +
                           shlex.quote(BINARY) + " \"$@\"\n")
        wrapper.chmod(0o700)
        with self.terminal(root, left, right, binary=wrapper) as terminal:
            terminal.type("printf not-run")
            terminal.press("Enter")
            terminal.wait_for("Bash exited before providing a prompt")
            self.panels(terminal)
            terminal.press("F1")
            terminal.wait_for("mcpp help")

    def test_shell_exit_does_not_wait_for_a_child_holding_the_pty(self):
        root, left, right = self.fixture()
        process = root / "background.pid"
        try:
            with self.terminal(root, left, right) as terminal:
                self.show_shell(terminal)
                terminal.type("sleep 30 & printf '%s' \"$!\" > " + shlex.quote(str(process)) + "; exit")
                terminal.press("Enter")
                terminal.wait_file(terminal.exit_file, b"0\n")
        finally:
            if process.exists():
                pid = int(process.read_text())
                command = Path(f"/proc/{pid}/cmdline")
                try:
                    if command.read_bytes() == b"sleep\x0030\x00":
                        os.kill(pid, signal.SIGTERM)
                except FileNotFoundError:
                    pass
                except ProcessLookupError:
                    pass

    def test_subshell_cli_override_does_not_change_the_saved_preference(self):
        root, left, right = self.fixture("use_subshell=true\n")
        first, second = root / "first", root / "second"
        with self.terminal(root, left, right, options=["--nosubshell"]) as terminal:
            self.command(terminal, f"export MCPP_ONESHOT=gone; printf ready > {shlex.quote(str(first))}",
                         first, b"ready")
            self.command(terminal, f"printf '%s' \"${{MCPP_ONESHOT-unset}}\" > {shlex.quote(str(second))}",
                         second, b"unset")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            self.assertIn("use_subshell=true\n", (terminal.root / "config/mcpp/config").read_text())

    def test_subshell_enable_flag_overrides_a_disabled_preference(self):
        root, left, right = self.fixture("use_subshell=false\n")
        first, second = root / "first", root / "second"
        with self.terminal(root, left, right, options=["--subshell"]) as terminal:
            self.command(terminal, f"export MCPP_ENABLED=kept; printf ready > {shlex.quote(str(first))}",
                         first, b"ready")
            self.command(terminal, f"printf '%s' \"$MCPP_ENABLED\" > {shlex.quote(str(second))}",
                         second, b"kept")
            terminal.press("F10")
            terminal.wait_file(terminal.exit_file, b"0\n")
            self.assertIn("use_subshell=false\n", (terminal.root / "config/mcpp/config").read_text())


if __name__ == "__main__":
    if not Path("/.dockerenv").is_file() or os.geteuid() == 0 or not Path(
        "/review/.operation-regressions-disposable"
    ).is_file():
        raise SystemExit("Shell checks require the owned non-root disposable container")
    BINARY, TMUX = str(Path(sys.argv[1]).resolve()), sys.argv[2]
    ROOT = Path(sys.argv[3]).resolve() / f"run-{os.getpid()}"
    ROOT.mkdir(parents=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[4:]], verbosity=2)
