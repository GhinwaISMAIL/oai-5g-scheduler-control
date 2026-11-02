#!/usr/bin/env python3
"""Add the scheduler-control xApp to the FlexRIC build and let the MAC control
message carry scheduler commands.

The pinned FlexRIC revision accepts a MAC control message only with header word 1
and message word 42 (an assertion on the RIC side, another in the gNB agent).  The
two words are used here to carry a scheduler command (see gnb/src/nr_dl_ctrl_e2.h),
so the RIC-side assertions are removed; the gNB side is changed in the gNB image
build (gnb/patches/*.fxpatch).  The xApp that sends the commands is copied next to
the stock MAC control example and added to its CMake file.

usage: patch-flexric-sched-ctrl.py FLEXRIC_DIR
"""

from __future__ import annotations

from pathlib import Path
import shutil
import sys

REPOSITORY = Path(__file__).resolve().parents[1]
XAPP_SOURCE = REPOSITORY / "xapp" / "xapp_sched_ctrl.c"
E2_HEADER = REPOSITORY / "gnb" / "src" / "nr_dl_ctrl_e2.h"

RIC_FILE = Path("src/sm/mac_sm/mac_sm_ric.c")
ORIGINAL = "  assert(req->hdr.dummy == 1);\n  assert(req->msg.action == 42);\n"
RELAXED = "  /* header and message words carry a scheduler command (nr_dl_ctrl_e2.h) */\n"

CTRL_DIR = Path("examples/xApp/c/ctrl")
CMAKE_MARK = "add_executable(xapp_sched_ctrl"
CMAKE_TARGET = """
add_executable(xapp_sched_ctrl
  xapp_sched_ctrl.c
  ../../../../src/util/alg_ds/alg/defer.c
  )

target_link_libraries(xapp_sched_ctrl
  PUBLIC
  e42_xapp
  -pthread
  -lsctp
  -ldl
  )
"""


def relax_ric_asserts(path: Path) -> bool:
    text = path.read_text()
    if RELAXED in text:
        if ORIGINAL in text:
            raise RuntimeError("source contains both the original and the relaxed check")
        print(f"Already patched: {path}")
        return False
    count = text.count(ORIGINAL)
    if count != 1:
        raise RuntimeError(f"expected exactly one control check in {path}, found {count}")
    path.write_text(text.replace(ORIGINAL, RELAXED, 1))
    print(f"Patched {path}: MAC control words are no longer restricted to 1 and 42")
    return True


def add_xapp(ctrl_dir: Path) -> bool:
    cmake = ctrl_dir / "CMakeLists.txt"
    if not cmake.is_file():
        raise RuntimeError(f"not found: {cmake}")
    for source in (XAPP_SOURCE, E2_HEADER):
        if not source.is_file():
            raise RuntimeError(f"not found: {source}")
        shutil.copyfile(source, ctrl_dir / source.name)
    text = cmake.read_text()
    if CMAKE_MARK in text:
        print(f"Already patched: {cmake}")
        return False
    if not text.endswith("\n"):
        text += "\n"
    cmake.write_text(text + CMAKE_TARGET)
    print(f"Patched {cmake}: added xapp_sched_ctrl")
    return True


def patch_tree(flexric: Path) -> bool:
    changed = relax_ric_asserts(flexric / RIC_FILE)
    changed = add_xapp(flexric / CTRL_DIR) or changed
    return changed


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} FLEXRIC_DIR", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    if not root.is_dir():
        print(f"error: not a directory: {root}", file=sys.stderr)
        return 2
    try:
        patch_tree(root)
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
