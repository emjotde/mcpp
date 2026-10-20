#!/usr/bin/env python3
"""One GNU incremental archive UI case against the parent's matching binary."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

from terminal_session import TerminalSession
from test_audit_ui import select_file


PAYLOAD = b"GNU incremental payload\nsecond line\n"
OLD_BINARY_SHA256 = "c4bad5275c76fbfd808ea7bc4fd93196fd75bc28aab4e36c391011704c112137"


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            result.update(block)
    return result.hexdigest()


def prepare(root):
    source = root / "input"
    tree = source / "tree"
    empty = tree / "empty"
    empty.mkdir(parents=True)
    document = tree / "payload.txt"
    document.write_bytes(PAYLOAD)
    tree.chmod(0o750)
    empty.chmod(0o710)
    document.chmod(0o640)
    for path in (tree, empty, document):
        os.utime(path, (1700000000, 1700000000))
    archive = root / "left" / "original.tar"
    snapshot = root / "original.snar"
    assert not archive.is_relative_to(source) and not snapshot.is_relative_to(source)
    command = [
        "/usr/bin/tar", "--format=gnu", "--listed-incremental=" + str(snapshot),
        "-cf", str(archive), "-C", str(source), "tree",
    ]
    subprocess.run(command, check=True, capture_output=True, timeout=15)
    state = archive.lstat()
    assert stat.S_ISREG(state.st_mode) and state.st_uid == 1001
    assert state.st_size <= 10240, "keep the original incremental archive at most 10 KiB"
    encoded = archive.read_bytes()
    records = []
    position = 0
    while any(encoded[position:position + 512]):
        header = encoded[position:position + 512]
        assert len(header) == 512, "truncated generated header"
        size = int(header[124:136].rstrip(b"\0 "), 8)
        body = encoded[position + 512:position + 512 + size]
        name = os.fsdecode(header[:100].split(b"\0", 1)[0])
        kind = chr(header[156])
        records.append({"name": name, "type": kind, "stored_bytes": size})
        if kind == "D":
            assert body.endswith(b"\0"), "GNU tar must generate a real directory dump"
        if name == "tree/payload.txt":
            assert kind == "0" and body == PAYLOAD, "the original payload must be a regular member"
        position += 512 + (size + 511) // 512 * 512
    assert len(encoded) - position >= 1024 and not any(encoded[position:])
    assert {(item["name"], item["type"]) for item in records} == {
        ("tree/", "D"), ("tree/empty/", "D"), ("tree/payload.txt", "0"),
    }, records
    original = subprocess.run(
        ["/usr/bin/tar", "-xOf", str(archive), "tree/payload.txt"],
        check=True, capture_output=True, timeout=15,
    ).stdout
    assert original == document.read_bytes() == PAYLOAD
    return archive, {
        "command": command, "records": records, "archive_bytes": state.st_size,
        "payload": str(document), "payload_sha256": digest(document),
        "snapshot": str(snapshot), "snapshot_sha256": digest(snapshot),
    }


def heading(terminal):
    lines = terminal.text().splitlines()
    return lines[1][:terminal.width // 2] if len(lines) > 1 else ""


def exercise(binary, tmux, root, archive):
    left, right, ui = root / "left", root / "right", root / "ui"
    config = ui / "config" / "mcpp"
    config.mkdir(parents=True)
    (config / "config").write_text(
        "use_internal_view=1\nuse_subshell=0\nconfirm_exit=0\npause_after_run=0\n"
    )
    final_directory = root / "final-directory"
    destination = right / "payload.txt"
    uri = str(archive) + "/utar://"
    with TerminalSession(
        binary, tmux, ui, ["--printwd", final_directory, left, right],
        record_exit=True, record_stderr=True,
    ) as terminal:
        terminal.wait_for(archive.name)
        terminal.raw(b"\x1bc")
        terminal.wait_for("Change directory")
        terminal.press("C-u")
        terminal.type(uri)
        terminal.press("Enter")
        terminal.wait_for("Change directory", present=False)
        terminal.wait(lambda: "original.tar/utar://" in heading(terminal), "Quick cd opens the original tar")
        terminal.wait_for("Reading archive", present=False)
        terminal.save("quick-cd-root")
        terminal.press("Home", "Down", "Enter")
        terminal.wait(lambda: "/utar://tree" in heading(terminal), "Enter opens the GNU D directory")
        terminal.wait_for("payload.txt")
        terminal.save("gnu-d-directory")

        terminal.press("Home", "Down", "Enter")
        terminal.wait(lambda: "/utar://tree/empty" in heading(terminal), "empty GNU D record remains a directory")
        assert all("payload.txt" not in line[:terminal.width // 2]
                   for line in terminal.text().splitlines()[3:-5])
        terminal.save("empty-gnu-d-directory")
        terminal.press("Home", "Enter")
        terminal.wait(lambda: "/utar://tree" in heading(terminal) and "/empty" not in heading(terminal),
                      "parent returns to the containing D directory")

        select_file(terminal, "payload.txt")
        terminal.press("F3")
        for line in PAYLOAD.decode("ascii").splitlines():
            terminal.wait_for(line)
        terminal.save("original-regular-view")
        terminal.press("F10")
        terminal.wait_for(PAYLOAD.decode("ascii").splitlines()[0], present=False)
        terminal.wait_for("payload.txt")
        terminal.press("F5")
        terminal.wait_for("Source mask:")
        terminal.press("Enter")
        terminal.wait_for("Source mask:", present=False)
        terminal.wait_file(destination, PAYLOAD)
        terminal.wait_for("Copy complete")
        terminal.wait_idle()
        terminal.save("original-regular-copy")

        terminal.press("Home", "Enter")
        terminal.wait(lambda: "original.tar/utar://" in heading(terminal) and
                      "/utar://tree" not in heading(terminal), "parent returns to the tar root")
        terminal.press("Home", "Enter")

        def real_archive_selected():
            lines = terminal.text().splitlines()
            return (len(lines) >= 6 and "/utar://" not in heading(terminal) and
                    left.name in heading(terminal) and
                    archive.name in lines[-5][:terminal.width // 2])

        terminal.wait(real_archive_selected, "tar root exits to and selects the actual archive file")
        terminal.save("real-archive-root-parent")
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")
    assert destination.read_bytes() == PAYLOAD, "copy must have the exact original regular-file bytes"
    assert final_directory.read_text() == str(left) + "\n", "root exit must restore the real local parent"
    return {
        "uri": uri, "copied_path": str(destination), "copied_bytes": len(PAYLOAD),
        "copied_sha256": digest(destination), "viewed_original_payload": True,
        "entered_empty_dump_directory": True, "root_parent_selected": str(archive),
        "final_directory": str(left),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="parent-provided matching new mcpp executable")
    parser.add_argument("--tmux", nargs="?", const="/usr/bin/tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        parser.error("requires uid1001 in the owned disposable container")
    binary = arguments.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("the supplied mcpp binary must be a regular executable")
    binary_before = digest(binary)
    if binary_before == OLD_BINARY_SHA256:
        parser.error("the old frozen frontend lacks this backend; wait for the parent's matching shared build")
    os.umask(0o077)
    os.chdir("/review/cases")
    base = Path("archive-incremental-ui-2fd3")
    base.mkdir(mode=0o700, exist_ok=True)
    if base.is_symlink() or base.stat().st_uid != 1001 or stat.S_IMODE(base.stat().st_mode) & 0o077:
        parser.error("the UI fixture base must be private, owned by uid1001, and not a symlink")
    root = base / ("run-" + str(os.getpid()))
    root.mkdir()
    root = root.resolve()
    os.chdir(root)
    Path("left").mkdir()
    Path("right").mkdir()
    archive, fixture = prepare(root)
    before = digest(archive)
    result = {
        "case": "gnu-incremental-actual-mcpp-ui", "uid": os.geteuid(), "root": str(root),
        "binary": str(binary), "binary_sha256_before": binary_before,
        "archive": str(archive), "archive_sha256_before": before, "fixture": fixture,
        "native_rerun": False, "model_rerun": False, "passed": False,
    }
    evidence = Path("/review/evidence/session/l28-incremental-ui-2fd3.json")
    try:
        result.update(exercise(binary, arguments.tmux, root, archive))
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["archive_sha256_after"] = digest(archive)
        result["source_unchanged"] = result["archive_sha256_after"] == before
        result["payload_unchanged"] = digest(Path(fixture["payload"])) == fixture["payload_sha256"]
        result["snapshot_unchanged"] = digest(Path(fixture["snapshot"])) == fixture["snapshot_sha256"]
        result["binary_sha256_after"] = digest(binary)
        result["binary_unchanged"] = result["binary_sha256_after"] == binary_before
        result["passed"] = result["passed"] and all(result[key] for key in (
            "source_unchanged", "payload_unchanged", "snapshot_unchanged", "binary_unchanged",
        ))
        Path("result.json").write_text(json.dumps(result, indent=2) + "\n")
        evidence.write_text(json.dumps(result, indent=2) + "\n")
    assert result["passed"], "archive, input, snapshot or supplied binary changed during the UI case"
    print("PASS incremental mcpp UI: Quick cd, GNU D directories, exact original view/copy, real tar parent")
    print(evidence)


if __name__ == "__main__":
    main()
