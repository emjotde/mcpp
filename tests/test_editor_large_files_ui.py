from pathlib import Path
import argparse
import hashlib
import os
import shutil
import tarfile

from terminal_session import TerminalSession
from test_editor_workflows import menu, prompt


THRESHOLD = 64 * 1024 * 1024
QUESTION = "Open it anyway?"


def profile(root, extra=""):
    config = root / "config" / "mcpp"
    config.mkdir(parents=True, exist_ok=True)
    (config / "config").write_text(
        "editor_line_numbers=false\neditor_syntax=false\nauto_save_setup=false\n"
        "single_escape=true\nuse_internal_edit=true\nask_new_filename=false\n" + extra
    )
    return root


def write_large(path, size):
    line = b"LARGE " + b"x" * 1017 + b"\n"
    block = line * 1024
    with path.open("wb") as output:
        while size >= len(block):
            output.write(block)
            size -= len(block)
        output.write(block[:size])


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def warning(session):
    session.wait_for(QUESTION)
    session.wait_for("Warning")
    session.wait_for("64.0 MiB")


def answer(session, *keys):
    session.press(*keys)
    session.wait_for(QUESTION, present=False, timeout=25)


def active(session, path=None, **state):
    session.wait_for(QUESTION, present=False, timeout=25)
    session.wait_editor(**state)
    if path is not None:
        session.wait(lambda: path.name in session.text().splitlines()[0],
                     f"Editor title contains {path.name}")


def request_open(session, path):
    menu(session, 0, "Open file")
    prompt(session, path)


def thresholds(manager, tmux, root, medium, exact):
    for label, path in (("above-old-cap", medium), ("exact-threshold", exact)):
        with TerminalSession(
            manager, tmux, profile(root / label), ["--nokeymap", "--edit", path],
            record_exit=True, record_stderr=True,
        ) as session:
            active(session, path, row=1, bytes=path.stat().st_size, dirty=False)
            assert QUESTION not in session.text()
            session.save(label)
        print(f"PASS {label}: opened without a confirmation", flush=True)


def unnamed_and_dirty(editor, tmux, root, large):
    with TerminalSession(
        editor, tmux, profile(root / "unnamed-and-dirty"), [large],
        record_exit=True, record_stderr=True,
    ) as session:
        warning(session)
        session.save("startup-confirmation")
        answer(session, "n")
        active(session, row=1, column=1, bytes=0, dirty=False)
        session.type("draft\nsecond")
        session.press("Home", "Right", "F3", "Right", "Right", "F3")
        before = session.wait_editor(dirty=True, selection=2)
        request_open(session, large)
        warning(session)
        answer(session, "Escape")
        active(session, **before)
        session.wait_for("draft")
        request_open(session, large)
        warning(session)
        answer(session, "Enter")
        active(session, large, row=1, column=1, dirty=False, bytes=large.stat().st_size)
        menu(session, 5, "Next")
        active(session, **before)
        session.wait_for("draft")
        session.save("dirty-buffer-retained")
    with TerminalSession(editor, tmux, profile(root / "unnamed-no-arguments"), []) as session:
        active(session, row=1, column=1, bytes=0, dirty=False)
        assert QUESTION not in session.text()
    print("PASS standalone startup decline, unnamed startup, Open accept/Escape and dirty-buffer retention", flush=True)


def multifile(manager, editor, tmux, root, large, first, second):
    with TerminalSession(
        editor, tmux, profile(root / "multi-decline"),
        [f"{first}:2", f"{large}:3", f"{second}:4"],
        record_exit=True, record_stderr=True,
    ) as session:
        warning(session)
        answer(session, "n")
        active(session, first, row=2)
        menu(session, 5, "Next")
        active(session, second, row=4)
        menu(session, 5, "Next")
        active(session, first, row=2)
        session.save("other-cli-arguments-retained")
    with TerminalSession(
        editor, tmux, profile(root / "multi-accept"),
        ["+5", f"{large}:3", f"{first}:2", large],
        record_exit=True, record_stderr=True,
    ) as session:
        warning(session)
        answer(session, "Enter")
        active(session, large, row=5)
        menu(session, 5, "Next")
        active(session, first, row=2)
        menu(session, 5, "Next")
        active(session, large, row=5)
        session.save("accepted-cli-positions-and-deduplication")
    with TerminalSession(
        manager, tmux, profile(root / "manager-cli-multi"),
        ["--nokeymap", "--edit", large, f"{first}:3", f"{second}:2"],
        record_exit=True, record_stderr=True,
    ) as session:
        warning(session)
        answer(session, "Escape")
        active(session, first, row=3)
        menu(session, 5, "Next")
        active(session, second, row=2)
        session.save("manager-cli-escape-retains-arguments")
    another = root / "another-large.txt"
    if another.is_file():
        another.unlink()
    another.hardlink_to(large)
    with TerminalSession(
        editor, tmux, profile(root / "multiple-large"), [large, first, another]
    ) as session:
        warning(session)
        session.press("n")
        session.wait_for(str(another))
        warning(session)
        answer(session, "Enter")
        active(session, first, row=1)
        menu(session, 5, "Next")
        active(session, another, row=1, bytes=large.stat().st_size)
        menu(session, 5, "Next")
        active(session, first, row=1)
        session.save("each-large-cli-argument-is-confirmed")
    another.unlink()
    print("PASS multi-file CLI accept/No/Escape, duplicate arguments and per-file/+line positions", flush=True)


def history(editor, tmux, root, large, first):
    extra = (
        f"editor_history={large}\neditor_save_position=true\n"
        f"editor_position=8,3,8:{large}\n"
    )
    with TerminalSession(editor, tmux, profile(root / "history", extra), [first]) as session:
        active(session, first, row=1)
        session.type("dirty ")
        before = session.wait_editor(dirty=True)
        for choice in ("n", "Enter"):
            menu(session, 0, "History")
            session.wait_for("History")
            session.press("Down", "Enter")
            warning(session)
            answer(session, choice)
            if choice == "n":
                active(session, first, **before)
            else:
                active(session, large, row=9, column=4, dirty=False)
        menu(session, 5, "Next")
        active(session, first, **before)
        session.save("history-retains-draft-and-restores-position")
    print("PASS editor history accept/decline and saved positions", flush=True)


def archive_provider_identity(editor, tmux, root, large, first):
    archive = root / "large-member.tar.gz"
    member = first.as_posix().lstrip("/")
    with tarfile.open(archive, "w:gz", format=tarfile.USTAR_FORMAT) as output:
        output.add(large, arcname=member, recursive=False)
    location = str(archive) + "/utar://" + member
    (root / "archive-provider-identity" / "cache" / "mcpp").mkdir(mode=0o700, parents=True, exist_ok=True)
    with TerminalSession(
        editor, tmux, profile(root / "archive-provider-identity"), [first, location]
    ) as session:
        warning(session)
        answer(session, "Enter")
        active(session, first, bytes=first.stat().st_size, dirty=False)
        menu(session, 5, "Next")
        active(session, bytes=large.stat().st_size, dirty=False)
        session.wait_for("LARGE ")
        menu(session, 5, "Next")
        active(session, first, bytes=first.stat().st_size, dirty=False)
        session.save("provider-identity-survives-large-approval")
    print("PASS CLI archive approval retains its provider and does not alias an equal local path", flush=True)


def manager_f4(manager, tmux, root, large):
    panel = root / "f4-panel"
    panel.mkdir(exist_ok=True)
    link = panel / "large.txt"
    if link.is_symlink():
        link.unlink()
    link.symlink_to(large)
    with TerminalSession(
        manager, tmux, profile(root / "manager-f4"),
        ["--nokeymap", "--nosubshell", panel, panel],
        record_exit=True, record_stderr=True,
    ) as session:
        session.wait_for("Name")
        session.press("Home", "Down", "F4")
        warning(session)
        answer(session, "n")
        session.wait_for("Name")
        session.wait(lambda: "L:[" not in session.text(), "Declining F4 retains the manager panels")
        session.press("F4")
        warning(session)
        answer(session, "Enter")
        active(session, link, row=1, bytes=large.stat().st_size)
        session.press("F10")
        session.wait_for("Name")
        session.save("manager-f4-accept-and-return")
    print("PASS manager F4 accept/decline and return to panels", flush=True)


def failed_open(editor, tmux, root, large, first, second):
    link = root / "disappearing-large.txt"
    if link.is_symlink():
        link.unlink()
    link.symlink_to(large)
    with TerminalSession(editor, tmux, profile(root / "failed-open"), [first]) as session:
        active(session, first)
        session.type("retained ")
        before = session.wait_editor(dirty=True)
        request_open(session, link)
        warning(session)
        link.unlink()
        answer(session, "Enter")
        session.wait_for("Editor error")
        session.press("Enter")
        session.wait_for("Editor error", present=False)
        active(session, first, **before)
        session.save("failed-approved-open-retains-draft")
    link.symlink_to(large)
    with TerminalSession(
        editor, tmux, profile(root / "failed-cli-open"), [f"{first}:2", link, f"{second}:3"]
    ) as session:
        warning(session)
        link.unlink()
        answer(session, "Enter")
        session.wait_for("Editor error")
        session.press("Enter")
        session.wait_for("Editor error", present=False)
        active(session, first, row=2, dirty=False)
        menu(session, 5, "Next")
        active(session, second, row=3, dirty=False)
        session.save("failed-cli-open-retains-other-arguments")
    print("PASS file removal during confirmation reports an error without replacing the draft", flush=True)


def find_to_editor(manager, tmux, root, large):
    panel = root / "f4-panel"
    with TerminalSession(
        manager, tmux, profile(root / "find-to-editor"),
        ["--nokeymap", "--nosubshell", panel, panel],
    ) as session:
        session.wait_for("Name")
        session.raw(b"\x1b?")
        session.wait_for("Recursive name pattern")
        session.type("large.txt")
        session.press("Enter")
        session.wait_for("Search results")
        session.wait_for("1 search result(s)")
        session.press("F4")
        warning(session)
        answer(session, "n")
        session.wait_for("Search results")
        session.press("F4")
        warning(session)
        answer(session, "Enter")
        active(session, row=1, dirty=False, bytes=large.stat().st_size)
        session.press("F10")
        session.wait_for("Search results")
        session.wait_for(str(panel / "large.txt"))
        session.save("find-accept-decline-and-return")
    print("PASS find-results F4 accept/decline and return to the same results", flush=True)


def backup_save(editor, tmux, root, large):
    target = root / "large-save.txt"
    shutil.copyfile(large, target)
    original = digest(target)
    with TerminalSession(
        editor, tmux, profile(root / "large-backup", "editor_save_mode=backup\n"), [target]
    ) as session:
        warning(session)
        answer(session, "Enter")
        active(session, target, row=1, dirty=False)
        session.type("SAVED ")
        session.wait_editor(dirty=True)
        session.press("F2")
        session.wait_for("Save file")
        session.press("Enter")
        session.wait_for("Save file", present=False, timeout=25)
        active(session, target, dirty=False, bytes=large.stat().st_size + 6)
        backup = Path(str(target) + "~")
        assert digest(backup) == original, "Large backup must preserve every original byte"
        with target.open("rb") as saved, backup.open("rb") as old:
            assert saved.read(6) == b"SAVED "
            while chunk := old.read(1024 * 1024):
                assert saved.read(len(chunk)) == chunk, "Large save changed bytes after its prefix"
            assert saved.read(1) == b""
        session.save("large-backup-save")
    print("PASS real-terminal save and exact streaming backup above 64 MiB", flush=True)


def diff_to_editor(diff, tmux, root, large):
    left, right = root / "diff-left.txt", root / "diff-right.txt"
    for path in (left, right):
        if path.is_symlink() or path.is_file():
            path.unlink()
        path.symlink_to(root / "exact.txt")
    with TerminalSession(diff, tmux, profile(root / "diff-to-editor"), [left, right]) as session:
        session.wait_for("Merge")
        left.unlink()
        left.symlink_to(large)
        session.press("F4")
        warning(session)
        answer(session, "n")
        session.wait_for("Merge")
        assert "L:[" not in session.text(), "Declining diff F4 must retain the comparison"
        session.press("F4")
        warning(session)
        answer(session, "Enter")
        active(session, row=1, bytes=large.stat().st_size, dirty=False)
        session.press("F10")
        warning(session)
        answer(session, "Enter")
        session.wait_for("Merge")
        session.wait_for("Dif 1")
        assert "Editor error" not in session.text(), "The comparison must safely reopen the approved editor file"
        session.save("diff-open-confirmation")
    left.unlink()
    right.unlink()
    print("PASS diff-to-editor accept/decline and confirmed comparison refresh", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("manager")
    parser.add_argument("editor")
    parser.add_argument("diff")
    parser.add_argument("tmux")
    parser.add_argument("root")
    args = parser.parse_args()
    if os.geteuid() != 1001 or not Path("/.dockerenv").exists() or not Path(
        "/review/.operation-regressions-disposable"
    ).exists():
        raise RuntimeError("Large-file UI checks require uid 1001 in the owned disposable container")
    root = Path(args.root).resolve()
    if not root.is_relative_to(Path("/review/cases/editor-large-files")):
        raise RuntimeError("Large-file UI fixtures must stay in the owned case subtree")
    root.mkdir(parents=True, exist_ok=True)
    medium, exact, large = (root / name for name in ("medium.txt", "exact.txt", "large.txt"))
    for path, size in ((medium, 16 * 1024 * 1024 + 1024), (exact, THRESHOLD),
                       (large, THRESHOLD + 1024)):
        write_large(path, size)
    first, second = root / "first.txt", root / "second.txt"
    first.write_text("".join(f"FIRST {line}\n" for line in range(1, 9)))
    second.write_text("".join(f"SECOND {line}\n" for line in range(1, 9)))
    original = digest(large)
    thresholds(args.manager, args.tmux, root, medium, exact)
    unnamed_and_dirty(args.editor, args.tmux, root, large)
    multifile(args.manager, args.editor, args.tmux, root, large, first, second)
    history(args.editor, args.tmux, root, large, first)
    archive_provider_identity(args.editor, args.tmux, root, large, first)
    manager_f4(args.manager, args.tmux, root, large)
    failed_open(args.editor, args.tmux, root, large, first, second)
    find_to_editor(args.manager, args.tmux, root, large)
    backup_save(args.editor, args.tmux, root, large)
    diff_to_editor(args.diff, args.tmux, root, large)
    assert digest(large) == original, "Opening/declining files must not change their bytes"
    print("PASS all editor large-file UI regressions", flush=True)


if __name__ == "__main__":
    main()
