#!/usr/bin/env python3
"""One image-only undelete UI case; no native comparison or device access."""

import argparse
import json
import os
from pathlib import Path
import stat

from terminal_session import TerminalSession
from test_audit_ui import select_file
from test_ext2_undelete import digest, prepare, require_container


def quick_cd(terminal, uri, member):
    terminal.raw(b"\x1bc")
    terminal.wait_for("Change directory")
    terminal.press("C-u")
    terminal.type(uri)
    terminal.press("Enter")
    terminal.wait_for("Change directory", present=False)
    terminal.wait_for("/undel://")
    terminal.wait_for(member)


def parent_selects_image(terminal, image, control_page_up=False):
    if control_page_up:
        terminal.raw(b"\x1b[5;5~")
    else:
        terminal.press("Home", "Enter")

    def selected_real_image():
        lines = terminal.text().splitlines()
        if len(lines) < 6:
            return False
        header = lines[1][:terminal.width // 2]
        selected = lines[-5][:terminal.width // 2]
        return ("/undel://" not in header and image.parent.name in header and
                image.name in selected)

    terminal.wait(selected_real_image, "root exit selects the real source image in its real parent")


def exercise(binary, tmux, root, image, fixture):
    left, right, ui = root / "left", root / "right", root / "ui"
    config = ui / "config" / "mcpp"
    config.mkdir(parents=True)
    (config / "config").write_text(
        "use_internal_view=1\nuse_subshell=0\nconfirm_exit=0\npause_after_run=0\n"
    )
    direct_bytes = (left / fixture["direct_payload"]).read_bytes()
    long_bytes = (left / fixture["long_payload"]).read_bytes()
    direct_member = f"{fixture['direct_inode']}:1"
    long_member = f"{fixture['long_inode']}:16"
    uri = str(image) + "/undel://"
    destination = right / long_member
    with TerminalSession(binary, tmux, ui, [left, right],
                         record_exit=True, record_stderr=True) as terminal:
        terminal.wait_for(image.name)
        quick_cd(terminal, uri, long_member)
        terminal.wait_for(direct_member)
        terminal.save("quick-cd-root")
        select_file(terminal, long_member)
        terminal.press("F3")
        long_marker = long_bytes.splitlines()[0].decode("ascii")
        terminal.wait_for(long_marker)
        terminal.save("indirect-member-view")
        terminal.press("F10")
        terminal.wait_for(long_marker, present=False)
        terminal.wait_for(long_member)
        terminal.press("F5")
        terminal.wait_for("Source mask:")
        terminal.press("Enter")
        terminal.wait_for("Source mask:", present=False)
        terminal.wait_file(destination, long_bytes)
        terminal.wait_for("Copy complete")
        terminal.wait_idle()
        terminal.save("recovered-copy")
        parent_selects_image(terminal, image)
        terminal.save("real-image-root-parent")

        quick_cd(terminal, uri, direct_member)
        select_file(terminal, direct_member)
        terminal.press("F3")
        direct_marker = direct_bytes.decode("ascii").strip()
        terminal.wait_for(direct_marker)
        terminal.save("reentered-direct-member-view")
        terminal.press("F10")
        terminal.wait_for(direct_marker, present=False)
        parent_selects_image(terminal, image, control_page_up=True)
        terminal.save("reentered-real-image-parent")
        terminal.press("F10")
        terminal.wait_file(terminal.exit_file, b"0\n")

    assert destination.read_bytes() == long_bytes, "copy did not recover the exact original bytes"
    return {
        "uri": uri,
        "viewed_members": [long_member, direct_member],
        "copied_member": long_member,
        "copied_path": str(destination),
        "copied_bytes": len(long_bytes),
        "copied_sha256": digest(destination),
        "expected_sha256": digest(left / fixture["long_payload"]),
        "root_parent_selected": str(image),
        "reentry_verified": True,
        "cache_identity_claimed": False,
    }


def main():
    require_container()
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path, help="parent-provided matching frozen mcpp executable")
    parser.add_argument("--tmux", default="/usr/bin/tmux")
    arguments = parser.parse_args()
    binary = arguments.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise SystemExit("the supplied mcpp binary must be a regular executable")

    os.chdir("/review/cases")
    base = Path("ext2-undelete-ui-3d420")
    base.mkdir(exist_ok=True)
    if base.is_symlink() or base.stat().st_uid != os.geteuid():
        raise SystemExit("the UI fixture base must be owned by uid1001 and not a symlink")
    root = base / ("run-" + str(os.getpid()))
    root.mkdir()
    root = root.resolve()
    os.chdir(root)
    Path("left").mkdir()
    Path("right").mkdir()
    os.chdir("left")
    try:
        fixture = prepare()
    finally:
        os.chdir(root)
    image = root / "left" / fixture["image"]
    image_stat = image.lstat()
    assert stat.S_ISREG(image_stat.st_mode) and image_stat.st_uid == os.geteuid(), \
        "the source must be an original owned regular image, never a device"
    assert image_stat.st_size == 8 * 1024 * 1024, "keep the one UI source image at 8 MiB"
    before = digest(image)
    assert before == fixture["original_sha256"], "fixture changed after preparation"
    binary_before = digest(binary)
    result = {
        "case": "image-only-ext2-undelete-ui",
        "uid": os.geteuid(),
        "root": str(root),
        "binary": str(binary),
        "binary_sha256_before": binary_before,
        "image": str(image),
        "image_bytes": image_stat.st_size,
        "image_sha256_before": before,
        "native_comparison": False,
        "passed": False,
    }
    evidence = Path("/review/evidence/session/l32-ui-result.json")
    try:
        result.update(exercise(binary, arguments.tmux, root, image, fixture))
        result["passed"] = True
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        result["image_sha256_after"] = digest(image)
        result["source_unchanged"] = result["image_sha256_after"] == before
        result["binary_sha256_after"] = digest(binary)
        result["binary_unchanged"] = result["binary_sha256_after"] == binary_before
        result["passed"] = result["passed"] and result["source_unchanged"] and result["binary_unchanged"]
        Path("result.json").write_text(json.dumps(result, indent=2) + "\n")
        evidence.write_text(json.dumps(result, indent=2) + "\n")
    assert result["source_unchanged"], "undelete navigation, viewing or copying modified the image"
    assert result["binary_unchanged"], "the supplied frozen binary changed during the UI case"
    print("mcpp image-only ext2 undelete: Quick cd, view/copy, re-entry and real-image parents passed")
    print(evidence)


if __name__ == "__main__":
    main()
