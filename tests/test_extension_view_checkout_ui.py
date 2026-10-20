#!/usr/bin/env python3
"""One hash-pinned virtual extension View checkout workflow."""

import argparse
from contextlib import contextmanager
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import stat
import tarfile

from terminal_session import TerminalSession
from test_audit_ui import select_file
from test_independent_screens_ui import screen


STALE_SHA256 = "790bf9f8b11e7b8aea40f7708d668b7c88cab12da3412a3d0736fabbc64d0549"
PAYLOAD = b"ARCHIVE-ONLY-CONTENT\nSECOND ARCHIVE LINE\n"
EXPECTED_OUTPUT = b"VIRTUAL-FILTER-BEGIN\n" + PAYLOAD + b"VIRTUAL-FILTER-END\n"
DRAFT = "UNSAVED-DRAFT-GUARDED-ORIGINAL"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    state = path.lstat()
    assert stat.S_ISDIR(state.st_mode) and state.st_uid == 1001, path


@contextmanager
def blocked_checkout_storage(terminal):
    runtime = terminal.root / "runtime"
    before = {path.name for path in runtime.iterdir()}
    permissions = stat.S_IMODE(runtime.stat().st_mode)
    runtime.chmod(0o500)
    try:
        yield
        assert {path.name for path in runtime.iterdir()} == before
    finally:
        runtime.chmod(permissions)


def exercise(binary, tmux, root, observations):
    left = root / "left"
    terminal_root = root / "terminal"
    config = terminal_root / "config/mcpp"
    for directory in (left, config):
        private_directory(directory)
    source = left / "odd ' $(touch BAD).case"
    guarded = left / "guarded.case"
    tagged = left / "tagged.case"
    for path in (source, guarded, tagged):
        path.write_bytes(b"HOST SHADOW MUST STAY UNCHANGED\n")
        path.chmod(0o400)
    archive = left / "members.tar"
    with tarfile.open(archive, "w") as output:
        for path, data in (
                (source, PAYLOAD),
                (guarded, b"GUARDED-ORIGINAL\n"),
                (tagged, b"TAGGED-ORIGINAL\n")):
            member = tarfile.TarInfo(str(path).lstrip("/"))
            member.size, member.mode = len(data), 0o444
            output.addfile(member, io.BytesIO(data))
    archive.chmod(0o400)
    originals = {path: digest(path) for path in (archive, source, guarded, tagged)}
    location = str(archive) + "/utar://" + str(left).lstrip("/")
    metadata = root / "argv.bin"
    unexpected = root / "guard-command-ran"
    resource = config / "mc.ext.ini"
    (config / "config").write_text(
        "pause_after_run=0\nuse_subshell=0\nconfirm_execute=0\nask_new_filename=0\n"
        "use_internal_view=1\nuse_internal_edit=1\nuse_file_to_guess_type=0\n"
    )

    def command(value):
        encoded = value.replace("\\", "\\\\").replace("\n", "\\n")
        resource.write_text(
            "[mc.ext.ini]\nVersion=4.0\n[Original checkout workflow]\nShell=.case\nView=" +
            encoded + "\n"
        )

    filtered = (
        r"%view{ascii,unform} test -f %f && printf '%%s\000' %f %p %d "
        '"$MC_EXT_FILENAME" "$MC_EXT_BASENAME" "$MC_EXT_CURRENTDIR" '
        '"$MC_EXT_SELECTED" "$MC_EXT_ONLYTAGGED" "$PWD" > ' + shlex.quote(str(metadata)) +
        r"; printf 'VIRTUAL-FILTER-BEGIN\n'; "
        r"""while IFS= read -r line; do printf '%%s\n' "$line"; done < %f; """ +
        r"printf 'VIRTUAL-FILTER-END\n'"
    )
    command(filtered)
    with TerminalSession(
            binary, tmux, terminal_root, ["-u", "--nokeymap", location, location],
            width=120, height=30, record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for("Name")
        terminal.press("Tab")
        select_file(terminal, source.name)
        terminal.press("Tab")
        select_file(terminal, tagged.name)
        terminal.press("Insert")
        select_file(terminal, source.name)
        with blocked_checkout_storage(terminal):
            terminal.press("F3")
            terminal.wait_for("clear active-pane marks first")
            assert not metadata.exists() and not unexpected.exists()
            terminal.save("active-marks-rejected-before-checkout")
        observations["marked_source_rejected_before_storage"] = True

        terminal.press("Down", "Insert")
        select_file(terminal, source.name)
        command("%view printf OPPOSITE-COMMAND %F > " + shlex.quote(str(unexpected)))
        with blocked_checkout_storage(terminal):
            terminal.press("F3")
            terminal.wait_for("other-pane file macros")
            assert not metadata.exists() and not unexpected.exists()
            terminal.save("opposite-virtual-rejected-before-checkout")
        observations["opposite_virtual_rejected_before_storage"] = True

        command(filtered)
        select_file(terminal, guarded.name)
        terminal.press("F4")
        terminal.wait_editor(dirty=False)
        terminal.type("UNSAVED-DRAFT-")
        terminal.wait_editor(dirty=True)
        terminal.wait_for(DRAFT)
        screen(terminal, "File manager")
        select_file(terminal, guarded.name)
        with blocked_checkout_storage(terminal):
            terminal.press("F3")
            terminal.wait_for("dirty editor buffer")
            assert not metadata.exists() and not unexpected.exists()
            terminal.save("original-dirty-buffer-rejected-before-checkout")
        observations["original_dirty_source_rejected_before_storage"] = True

        select_file(terminal, source.name)
        terminal.press("F3")
        terminal.wait_for("VIRTUAL-FILTER-BEGIN")
        terminal.wait_for("ARCHIVE-ONLY-CONTENT")
        terminal.wait_for("VIRTUAL-FILTER-END")
        terminal.wait(lambda: metadata.is_file(), "capture actual extension argv and environment")
        values = metadata.read_bytes().split(b"\0")
        assert len(values) == 10 and values[-1] == b"", values
        values.pop()
        fields = [os.fsdecode(value) for value in values]
        physical = Path(fields[0])
        assert physical.is_absolute() and physical.name == source.name, fields
        assert physical.parent.parent == terminal.root / "runtime", fields
        assert physical.parent.name.startswith("mcpp-view-"), fields
        assert fields == [
            str(physical), source.name, location, str(physical), source.name,
            location, source.name, "", str(physical.parent),
        ], fields
        assert not physical.exists() and not physical.parent.exists()
        assert not list((terminal.root / "runtime").glob("mcpp-view-*"))
        output_files = [
            path for path in (terminal.root / "runtime").glob("mcpp-local-*")
            if path.is_file() and path.read_bytes() == EXPECTED_OUTPUT
        ]
        assert len(output_files) == 1, output_files
        output_file = output_files[0]
        state = output_file.lstat()
        assert stat.S_ISREG(state.st_mode) and state.st_uid == 1001
        assert stat.S_IMODE(state.st_mode) == 0o600
        terminal.save("filtered-output-original-identity")
        observations["argv"] = dict(zip(
            ("f", "p", "d", "MC_EXT_FILENAME", "MC_EXT_BASENAME", "MC_EXT_CURRENTDIR",
             "MC_EXT_SELECTED", "MC_EXT_ONLYTAGGED", "physical_cwd"), fields))
        observations["output_sha256"] = digest(output_file)
        observations["checkout_removed_when_command_finished"] = True
        observations["ascii_unformatted_output_exact"] = True

        screen(terminal, "File manager")
        assert output_file.read_bytes() == EXPECTED_OUTPUT
        screen(terminal, guarded.name, "Edit")
        terminal.wait_editor(dirty=True)
        terminal.wait_for(DRAFT)
        terminal.save("original-draft-preserved")
        assert output_file.read_bytes() == EXPECTED_OUTPUT
        observations["original_dirty_draft_preserved"] = True
        observations["hidden_output_survives_checkout_and_screen_changes"] = True

        screen(terminal, "Extension View", "View")
        terminal.wait_for("ARCHIVE-ONLY-CONTENT")
        terminal.press("F4", "F2")
        terminal.wait_for("command output is read-only")
        assert output_file.read_bytes() == EXPECTED_OUTPUT
        terminal.save("filtered-output-remains-read-only")
        observations["filtered_output_remains_read_only"] = True
        terminal.press("Enter")
        terminal.wait_for("command output is read-only", present=False)
        terminal.press("F3")
        terminal.wait_for("Name")
        terminal.wait(lambda: not output_file.exists(), "closed output screen releases its own private file")
        assert not list((terminal.root / "runtime").glob("mcpp-view-*"))
        assert not list((terminal.root / "runtime").glob("mcpp-local-*"))
        observations["output_removed_only_when_its_screen_closed"] = True
        assert all(digest(path) == original for path, original in originals.items())
        assert not unexpected.exists() and not list(root.rglob("BAD"))
    assert all(digest(path) == original for path, original in originals.items())
    observations["original_archive_and_host_shadows_unchanged"] = True
    observations["original_sha256"] = {str(path): value for path, value in originals.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("sha256")
    parser.add_argument("--tmux", default="tmux")
    arguments = parser.parse_args()
    assert os.geteuid() == 1001 and Path("/.dockerenv").is_file()
    assert Path("/review/.operation-regressions-disposable").is_file()
    binary = arguments.binary.resolve(strict=True)
    assert str(binary).startswith("/home/mcpp/src/build/local-parity/"), binary
    assert re.fullmatch(r"[0-9a-f]{64}", arguments.sha256), arguments.sha256
    assert arguments.sha256 != STALE_SHA256 and digest(binary) == arguments.sha256
    root = Path("/review/cases/l21-view-checkout/ui") / arguments.sha256[:16] / f"run-{os.getpid()}"
    private_directory(root)
    evidence = Path("/review/evidence/session/l21-view-checkout-ui.json")
    result = {
        "case": "one reserved actual virtual extension View checkout workflow",
        "binary": str(binary),
        "sha256": arguments.sha256,
        "uid": os.geteuid(),
        "root": str(root),
        "observations": {},
    }
    try:
        exercise(binary, arguments.tmux, root, result["observations"])
        assert digest(binary) == arguments.sha256
        result["status"] = "passed"
        print("Virtual extension View UI passed: original identity, guards, exact output and independent cleanup")
    except Exception as error:
        result["status"] = "failed"
        result["error"] = repr(error)
        raise
    finally:
        evidence.write_text(json.dumps(result, indent=2) + "\n")
        print("evidence:", evidence)


if __name__ == "__main__":
    main()
