from pathlib import Path
import os
import shutil
import sys
import tempfile

from terminal_session import TerminalSession


def find_node(terminal, name):
    terminal.raw(b"\x13")
    terminal.type(name)
    terminal.press("Enter")
    terminal.wait(
        lambda: terminal.selected_tree_row().endswith(name),
        f"Tree selects {name}",
    )


def wait_loaded(terminal):
    terminal.wait_for("…", present=False, timeout=45)


def run(binary, tmux):
    root = Path(tempfile.mkdtemp(prefix="mcpp-tree-workflows-"))
    passed = False
    try:
        left, right = root / "left", root / "right"
        for path in ("alpha/one/deep", "alpha/two", "beta/leaf",
                     ".hidden/child", "locked/grand"):
            (left / path).mkdir(parents=True, exist_ok=True)
        for path in ("alpha/one", "locked/grand"):
            (left / path / "copy-marker.txt").write_text("Tree refresh outcome\n")
        right.mkdir()
        unicode_name = "é" * 13 + "-suffix"
        (left / unicode_name).mkdir()
        for index in range(4096):
            (left / "alpha" / f"wide-{index:05}").mkdir()
        with TerminalSession(binary, tmux, root / "ui", [left, right]) as terminal:
            terminal.wait_for("Name")
            terminal.type("touch pending-marker")
            terminal.press("F9", "Enter", "Down", "Down", "Down", "Enter")
            terminal.wait_for("Directory tree")
            terminal.press("Tab")
            terminal.wait_for("Rescan")
            terminal.wait(
                lambda: terminal.selected_tree_row().endswith("left"),
                "Tree opens at the pane cwd",
            )
            process_id = int(terminal.run(
                "display-message", "-p", "-t", "test:0.0", "#{pane_pid}"
            ))
            task_directory = Path(f"/proc/{process_id}/task")
            initial_threads = len(list(task_directory.iterdir()))
            terminal.press("Enter")
            terminal.wait_for("alpha")
            assert not (left / "pending-marker").exists()
            assert not (right / "pending-marker").exists(), (
                "Tree Enter must not execute the pending commander command"
            )
            find_node(terminal, "alpha")
            terminal.type("+")
            terminal.wait_for("Expanding tree")
            terminal.press("Down")
            terminal.wait(
                lambda: terminal.selected_tree_row().endswith("beta"),
                "Selection moves to beta during alpha expansion",
            )
            wait_loaded(terminal)
            terminal.wait_for("Expanding tree", present=False)
            assert terminal.selected_tree_row().endswith("beta"), (
                "Expansion must not redirect the selection to an inserted child"
            )
            terminal.save("selection-preserved")
            terminal.press("F8")
            terminal.wait_for("Delete 1 selected item(s)?")
            lines = terminal.text().splitlines()
            count_line = next(
                index for index, line in enumerate(lines)
                if "Delete 1 selected item(s)?" in line
            )
            assert "beta" in lines[count_line + 1], (
                "The Tree delete dialog must target the visible beta selection"
            )
            terminal.save("visible-delete-target")
            terminal.press("Escape")
            terminal.wait_for("Delete cancelled")
            assert (left / "beta" / "leaf").is_dir()
            terminal.press("Tab", "Tab")
            terminal.wait(
                lambda: terminal.selected_tree_row().endswith("beta"),
                "Tab round-trip preserves Tree selection",
            )
            find_node(terminal, "alpha")
            terminal.press("F2")
            terminal.wait_for("Tree refreshed", timeout=45)
            wait_loaded(terminal)
            assert terminal.selected_tree_row().endswith("alpha"), (
                "Rescan must preserve the selected branch"
            )
            terminal.wait_for("wide-00000", timeout=45)
            terminal.save("rescan-preserved")
            (left / "alpha" / "new-child").mkdir()
            terminal.press("F2")
            terminal.wait_for("Tree refreshed", timeout=45)
            wait_loaded(terminal)
            terminal.wait_for("new-child", timeout=45)
            find_node(terminal, "one")
            terminal.press("Enter")
            terminal.wait_for("deep")
            wait_loaded(terminal)
            find_node(terminal, "alpha")
            terminal.press("F2")
            terminal.wait_for("Tree refreshed", timeout=45)
            wait_loaded(terminal)
            terminal.wait_for("deep")
            terminal.save("expanded-descendant-preserved")
            terminal.type("-")
            terminal.wait_for("Tree branch collapsed")
            terminal.wait_for("wide-00000", present=False)
            terminal.type("+")
            terminal.wait_for("wide-00000", timeout=45)
            wait_loaded(terminal)
            assert terminal.selected_tree_row().endswith("alpha")
            terminal.save("collapse-expand")
            terminal.type("-")
            terminal.wait_for("Tree branch collapsed")
            terminal.type("+")
            terminal.wait_for("Expanding tree")
            terminal.type("-")
            terminal.wait_for("Tree branch collapsed")
            terminal.wait(
                lambda: len(list(task_directory.iterdir())) <= initial_threads,
                "Cancelled Tree expansion worker exits",
            )
            terminal.press("Tab", "Tab")
            terminal.wait(
                lambda: terminal.selected_tree_row().endswith("alpha"),
                "Cancelled expansion keeps its selected parent",
            )
            assert "wide-00000" not in terminal.text(), (
                "Late expansion results must not reopen a collapsed branch"
            )
            terminal.save("cancelled-loading")
            terminal.type("+")
            terminal.wait_for("wide-00000", timeout=45)
            wait_loaded(terminal)
            terminal.press("F2")
            terminal.wait_for("Rescanning tree")
            terminal.type("-")
            terminal.wait_for("Tree branch collapsed")
            terminal.wait(
                lambda: len(list(task_directory.iterdir())) <= initial_threads,
                "Cancelled Tree rescan worker exits",
            )
            terminal.press("Tab", "Tab")
            assert "wide-00000" not in terminal.text(), (
                "Collapse during a rescan must remove the cached descendants"
            )
            terminal.save("cancelled-rescan")
            terminal.type("+")
            terminal.wait_for("wide-00000", timeout=45)
            wait_loaded(terminal)
            terminal.type("-")
            terminal.wait_for("Tree branch collapsed")
            terminal.type("+")
            terminal.wait_for("Expanding tree")
            terminal.run("send-keys", "-t", "test:0.0", "F2", "Down")
            terminal.wait(
                lambda: terminal.selected_tree_row().endswith("beta"),
                "Moving during a queued rescan keeps beta selected",
            )
            terminal.wait_for("Tree refreshed", timeout=45)
            find_node(terminal, "alpha")
            terminal.wait_for("wide-00000", timeout=45)
            terminal.save("queued-rescan-target")
            terminal.type("-")
            terminal.wait_for("Tree branch collapsed")
            terminal.type("+")
            terminal.wait_for("Expanding tree")
            terminal.run("send-keys", "-t", "test:0.0", "F2", "-")
            terminal.wait_for("Tree branch collapsed")
            terminal.wait(
                lambda: len(list(task_directory.iterdir())) <= initial_threads,
                "Cancelling a queued rescan stops the worker",
            )
            terminal.press("Tab", "Tab")
            assert "wide-00000" not in terminal.text(), (
                "A queued rescan must not reopen a deliberately collapsed branch"
            )
            terminal.type("+")
            terminal.wait_for("wide-00000", timeout=45)
            wait_loaded(terminal)
            if os.geteuid() != 0:
                locked = left / "locked"
                locked.chmod(0)
                try:
                    find_node(terminal, "locked")
                    terminal.type("+")
                    terminal.wait_for("! locked")
                    locked.chmod(0o700)
                    terminal.type("+")
                    terminal.wait_for("grand")
                    terminal.wait_for("! locked", present=False)
                    terminal.save("permission-retry")
                    locked.chmod(0)
                    terminal.press("F2")
                    terminal.wait_for("! locked")
                    assert "grand" in terminal.text(), (
                        "Failed rescan retains the cached expanded child"
                    )
                    terminal.type("-")
                    terminal.wait_for("Tree branch collapsed")
                    terminal.wait_for("grand", present=False)
                    terminal.save("failed-rescan-collapsed")
                    locked.chmod(0o700)
                    terminal.type("+")
                    terminal.wait_for("grand")
                    wait_loaded(terminal)
                    find_node(terminal, "grand")
                    terminal.press("Enter")
                    terminal.wait(
                        lambda: "/locked/grand$" in terminal.text().splitlines()[-2],
                        "Tree Enter changes the opposite listing directory",
                    )
                    wait_loaded(terminal)
                    terminal.type("+")
                    terminal.wait_for("Tree branch has no children")
                finally:
                    locked.chmod(0o700)
            terminal.press("Tab", "Escape")
            terminal.wait_for("touch pending-marker", present=False)
            terminal.press("End", "F5", "Enter")
            terminal.wait_for("Copy complete")
            terminal.wait(
                lambda: len(list(task_directory.iterdir())) <= initial_threads,
                "Background Tree refresh finishes after the copy",
            )
            terminal.press("Tab", "Tab")
            assert "Copy complete" in terminal.text(), (
                "Automatic Tree refresh must not overwrite the operation result"
            )
            assert (left / "copy-marker.txt").read_text() == "Tree refresh outcome\n"
            terminal.save("operation-refresh-status")
            terminal.press("Tab")
            find_node(terminal, unicode_name)
            terminal.resize(64, 20)
            terminal.wait(
                lambda: terminal.selected_tree_row().endswith("~é-suffix"),
                "Narrow Tree preserves complete Unicode glyphs",
            )
            assert terminal.selected_tree_row().count("é") == 9, (
                "Tree fitting must count display columns, not UTF-8 bytes"
            )
            terminal.save("unicode-fit")
        with TerminalSession(binary, tmux, root / "right-ui", [left, right]) as terminal:
            terminal.wait_for("Name")
            terminal.press("Tab", "F9", "Enter", "Down", "Down", "Down", "Enter")
            terminal.wait_for("Directory tree")
            terminal.press("Tab")
            terminal.wait(
                lambda: terminal.selected_tree_row(side=1).endswith("right"),
                "Right Tree opens at the right pane cwd",
            )
            terminal.save("right-tree-cwd")
        hidden_ui = root / "hidden-ui"
        (hidden_ui / "config" / "mcpp").mkdir(parents=True)
        (hidden_ui / "config" / "mcpp" / "config").write_text("show_hidden=false\n")
        with TerminalSession(
            binary, tmux, hidden_ui, [left / ".hidden", right]
        ) as terminal:
            terminal.wait_for("Name")
            terminal.press("F9", "Enter", "Down", "Down", "Down", "Enter")
            terminal.wait_for("Directory tree")
            terminal.press("Tab")
            terminal.wait(
                lambda: terminal.selected_tree_row().endswith(".hidden"),
                "Tree retains a hidden cwd even with hidden files disabled",
            )
            terminal.press("Enter")
            terminal.wait_for("child")
            terminal.save("hidden-cwd")
        passed = True
    finally:
        if passed:
            shutil.rmtree(root)
        else:
            print(f"Tree failure artifacts: {root}", file=sys.stderr)


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
