from pathlib import Path
import argparse
import os
import tarfile

from terminal_session import TerminalSession
from test_editor_workflows import menu, prompt


def profile(root):
    config = root / "config" / "mcpp"
    config.mkdir(parents=True, exist_ok=True)
    (config / "config").write_text(
        "single_escape=true\nauto_save_setup=false\neditor_syntax=false\n"
        "editor_line_numbers=false\neditor_confirm_save=false\n"
    )
    return root


def session(binary, tmux, root, *files):
    return TerminalSession(binary, tmux, profile(root), list(files),
                           record_exit=True, record_stderr=True)


def lock_path(file):
    return file.with_name(".#" + file.name)


def locked(terminal):
    terminal.wait_for("File locked")
    terminal.wait_for("Grab lock")
    terminal.wait_for("Ignore lock")
    terminal.wait_for("User:")
    terminal.wait_for("Process ID:")
    assert "[ Cancel ]" not in terminal.text()


def open_file(terminal, file):
    menu(terminal, 0, "Open file")
    prompt(terminal, file)


def save_as(terminal, file):
    menu(terminal, 0, "Save as")
    prompt(terminal, file)


def quit_editor(terminal):
    terminal.press("F10")
    for _ in range(4):
        terminal.wait(lambda: terminal.exit_file.exists() or "Save before close?" in terminal.text(),
                      "Editor exits or asks about its remaining draft")
        if terminal.exit_file.exists():
            assert terminal.exit_file.read_text().strip() == "0"
            return
        terminal.press("n")
        terminal.wait_idle()
    raise AssertionError("Editor did not finish closing its buffers")


def opens(binary, tmux, root):
    file, other = root / "shared.txt", root / "other.txt"
    file.write_text("shared original\n")
    other.write_text("other original\n")
    artifact = lock_path(file)
    with session(binary, tmux, root / "holder", file) as first:
        first.wait_editor(dirty=False)
        assert not artifact.is_symlink()
        first.type("first ")
        first.wait_editor(dirty=True)
        first.wait(lambda: artifact.is_symlink(), "First editor owns its native lock")
        original_owner = artifact.readlink()
        with session(binary, tmux, root / "contender", file, other) as second:
            second.wait_editor(dirty=False)
            second.wait_for("shared original")
            second.type("X")
            locked(second)
            second.save("first-modification-live-lock")
            second.press("Escape")
            second.wait_for("File locked", present=False)
            second.wait_editor(dirty=False)
            second.wait_for("Xshared original", present=False)
            menu(second, 5, "Next")
            second.wait_for("other original")
            second.type("draft ")
            second.wait_for("draft other original")
            before = second.wait_editor(dirty=True)
            open_file(second, file)
            second.wait_editor(dirty=False)
            second.type("Y")
            locked(second)
            second.press("Escape")
            second.wait_for("File locked", present=False)
            second.wait_editor(dirty=False)
            menu(second, 5, "Next")
            second.wait_editor(**before)
            second.wait_for("draft other original")
            assert artifact.readlink() == original_owner
            open_file(second, file)
            second.wait_editor(dirty=False)
            second.type("S")
            locked(second)
            second.press("i")
            second.wait_for("File locked", present=False)
            second.wait_editor(dirty=True)
            second.wait_for("Sshared original")
            assert artifact.readlink() == original_owner
            first.press("F2")
            first.wait_editor(dirty=False)
            assert not artifact.is_symlink()
            expected = b"first shared original\n"
            first.wait_file(file, expected)
            second.press("F2")
            second.wait_for("Editor error")
            second.wait_for("changed")
            assert file.read_bytes() == expected
            second.press("Escape")
            second.wait_for("Editor error", present=False)
            second.wait_editor(dirty=True)
            second.save("revision-check-survives-ignore")
            quit_editor(second)
            assert not artifact.is_symlink()
        quit_editor(first)
        assert not artifact.is_symlink()
    assert not lock_path(other).is_symlink()
    print("PASS live conflict, queued CLI cancellation, draft retention, Ignore, revision checks and close cleanup")


def save_as_provider(binary, tmux, root):
    host = root / "host.txt"
    host.write_text("host contents\n")
    member_source = root / "member.txt"
    member_source.write_text("archive contents\n")
    archive = root / "source.tar"
    member = str(host).lstrip("/")
    with tarfile.open(archive, "w") as output:
        output.add(member_source, arcname=member)
    busy, destination = root / "busy.txt", root / "saved.txt"
    busy.write_text("busy original\n")
    with session(binary, tmux, root / "save-holder", busy) as holder:
        holder.wait_editor(dirty=False)
        holder.type("holder ")
        holder.wait_editor(dirty=True)
        owner = lock_path(busy).readlink()
        with session(binary, tmux, root / "save-source", str(archive) + "/utar://" + member) as editor:
            editor.wait_editor(dirty=False)
            editor.wait_for("archive contents")
            assert not lock_path(host).is_symlink()
            editor.type("draft ")
            editor.wait_for("draft archive contents")
            before = editor.wait_editor(dirty=True)
            save_as(editor, busy)
            editor.wait_for("Replace existing file?")
            editor.press("r")
            locked(editor)
            editor.press("Escape")
            editor.wait_for("File locked", present=False)
            editor.wait_editor(**before)
            assert lock_path(busy).readlink() == owner
            assert busy.read_bytes() == b"busy original\n"
            save_as(editor, busy)
            editor.wait_for("Replace existing file?")
            editor.press("r")
            locked(editor)
            busy.write_text("changed while deciding\n")
            editor.press("g")
            editor.wait_for("Editor error")
            editor.wait_for("changed")
            assert busy.read_bytes() == b"changed while deciding\n"
            assert not lock_path(busy).is_symlink()
            editor.press("Escape")
            editor.wait_for("Editor error", present=False)
            editor.wait_editor(**before)
            save_as(editor, destination)
            editor.wait_editor(dirty=False)
            editor.wait_file(destination, b"draft archive contents\n")
            assert not lock_path(destination).is_symlink()
            assert not lock_path(host).is_symlink()
            editor.press("Home")
            editor.type("local ")
            editor.wait_for("local draft archive contents")
            editor.wait_editor(dirty=True)
            assert lock_path(destination).is_symlink()
            editor.press("F2")
            editor.wait_editor(dirty=False)
            editor.wait_file(destination, b"local draft archive contents\n")
            assert not lock_path(destination).is_symlink()
            editor.save("archive-to-local-save-as")
            quit_editor(editor)
            assert not lock_path(destination).is_symlink()
        quit_editor(holder)
    assert host.read_bytes() == b"host contents\n"
    with tarfile.open(archive) as source:
        assert source.extractfile(member).read() == b"archive contents\n"
    print("PASS archive identity, Save As cancel/conflict/changed revision and local destination lock lifecycle")


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument("binary")
    arguments.add_argument("tmux")
    arguments.add_argument("root", type=Path)
    arguments.add_argument("case", choices=("opens", "save-as"))
    args = arguments.parse_args()
    root = args.root.absolute()
    assert os.geteuid() == 1001 and Path("/review/.operation-regressions-disposable").is_file()
    assert str(root).startswith("/review/cases/editor-locks/")
    root.mkdir(parents=True, exist_ok=True)
    assert not root.is_symlink() and root.stat().st_uid == os.geteuid()
    if args.case == "opens":
        opens(args.binary, args.tmux, root)
    else:
        save_as_provider(args.binary, args.tmux, root)


if __name__ == "__main__":
    main()
