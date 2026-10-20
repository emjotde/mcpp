#!/usr/bin/env python3
"""Two bounded backend cases using original regular ext2 images, never devices."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


def require_container():
    if (os.geteuid() != 1001 or not Path("/.dockerenv").is_file() or
            not Path("/review/.operation-regressions-disposable").is_file()):
        raise SystemExit("ext2 tests require uid1001 in the owned disposable container")


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def inode_offset(image, number):
    with image.open("rb") as stream:
        stream.seek(1024)
        superblock = stream.read(1024)
        block_size = 1024 << struct.unpack_from("<I", superblock, 24)[0]
        first_data = struct.unpack_from("<I", superblock, 20)[0]
        per_group = struct.unpack_from("<I", superblock, 40)[0]
        inode_size = struct.unpack_from("<H", superblock, 88)[0]
        group, index = divmod(number - 1, per_group)
        stream.seek((first_data + 1) * block_size + group * 32 + 8)
        table = struct.unpack("<I", stream.read(4))[0]
        return table * block_size + index * inode_size, per_group


def prepare():
    image = Path("original.ext2")
    direct = Path("direct.txt")
    longer = Path("long.bin")
    direct.write_bytes(b"MCPP-EXT2-UNDELETE-ORIGINAL\n")
    longer.write_bytes((b"ORIGINAL-EXT2-INDIRECT-DATA\n" * 700)[:15 * 1024 + 17])
    with image.open("xb") as stream:
        stream.truncate(8 * 1024 * 1024)
    # Two small groups and 32 inodes place the long deleted inode in group 1.
    made = subprocess.run([
        "/usr/sbin/mke2fs", "-q", "-t", "ext2", "-b", "1024", "-I", "128",
        "-g", "4096", "-N", "32", "-O", "none", "-F", str(image),
    ], check=True, capture_output=True, text=True, timeout=15)
    Path("mke2fs.txt").write_text(made.stdout + made.stderr)
    commands = [f"write {direct} /direct.txt"]
    commands += [f"write {direct} /live-{index}" for index in range(4)]
    commands += [
        f"write {longer} /long.bin",
        "set_inode_field /long.bin mode 0100640",
        "set_inode_field /long.bin uid 1001",
        "set_inode_field /long.bin gid 1001",
        "rm /direct.txt",
        "rm /long.bin",
    ]
    Path("debugfs.commands").write_text("\n".join(commands) + "\n")
    made = subprocess.run([
        "/usr/sbin/debugfs", "-w", "-f", "debugfs.commands", str(image),
    ], check=True, capture_output=True, text=True, timeout=15)
    output = made.stdout + made.stderr
    Path("debugfs.txt").write_text(output)
    allocated = [int(value) for value in re.findall(r"Allocated inode: (\d+)", output)]
    if len(allocated) != 6:
        raise AssertionError(f"fixture allocation failed: {output}")
    direct_inode, live_inode, long_inode = allocated[0], allocated[1], allocated[-1]
    direct_offset, per_group = inode_offset(image, direct_inode)
    long_offset, _ = inode_offset(image, long_inode)
    live_offset, _ = inode_offset(image, live_inode)
    if direct_inode > per_group or long_inode <= per_group:
        raise AssertionError("fixture must exercise both inode groups")
    # Fixed original metadata makes time checks independent of the test clock.
    with image.open("r+b") as stream:
        stream.seek(long_offset + 8)
        stream.write(struct.pack("<III", 1600000000, 1600000200, 1600000100))
    guard = Path("guard.ext2")
    shutil.copyfile(image, guard)
    with guard.open("r+b") as stream:
        stream.seek(live_offset + 40)
        allocated_block = struct.unpack("<I", stream.read(4))[0]
        stream.seek(direct_offset + 40)
        stream.write(struct.pack("<I", allocated_block))
        stream.seek(long_offset + 40)
        stream.write(struct.pack("<I", 0))
    result = {
        "image": str(image), "guard_image": str(guard),
        "direct_payload": str(direct), "long_payload": str(longer),
        "direct_inode": direct_inode, "long_inode": long_inode,
        "inode_group_size": per_group,
        "original_sha256": digest(image), "guard_sha256": digest(guard),
    }
    Path("fixture.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    require_container()
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--root", type=Path, default=Path("/review/cases/ext2-undelete-backend-3d420"))
    arguments = parser.parse_args()
    if arguments.binary:
        arguments.binary = arguments.binary.resolve(strict=True)
    root = arguments.root.absolute()
    if root.parent != Path("/review/cases") or root.is_symlink():
        raise SystemExit("ext2 case root must be directly below /review/cases")
    root.mkdir(exist_ok=True)
    if root.stat().st_uid != os.geteuid():
        raise SystemExit("ext2 case root must belong to uid1001")
    os.chdir(root)
    manifest = Path("fixture.json")
    fixture = json.loads(manifest.read_text()) if manifest.is_file() else prepare()
    if digest(Path(fixture["image"])) != fixture["original_sha256"] or \
            digest(Path(fixture["guard_image"])) != fixture["guard_sha256"]:
        raise AssertionError("original fixture bytes changed outside preparation")
    if arguments.prepare_only:
        print(json.dumps({"root": str(root), **fixture}, indent=2))
        return
    if arguments.binary is None:
        parser.error("--binary is required unless --prepare-only is used")
    results = []
    for case, key in (("recovery", "image"), ("guard", "guard_image")):
        image = Path(fixture[key])
        before = digest(image)
        run = subprocess.run([
            str(arguments.binary), case, str(image), str(fixture["direct_inode"]),
            str(fixture["long_inode"]), fixture["direct_payload"], fixture["long_payload"],
        ], capture_output=True, text=True, timeout=20)
        after = digest(image)
        result = {"case": case, "returncode": run.returncode, "stdout": run.stdout,
                  "stderr": run.stderr, "sha256_before": before,
                  "sha256_after": after, "source_unchanged": before == after}
        results.append(result)
        Path(case + "-result.json").write_text(json.dumps(result, indent=2) + "\n")
        if run.returncode != 0 or before != after:
            raise AssertionError(json.dumps(result, indent=2))
    report = {"uid": os.geteuid(), "root": str(root), "fixture": fixture, "cases": results}
    Path("/review/evidence/session/l32-backend-result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
