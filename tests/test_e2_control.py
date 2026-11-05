#!/usr/bin/env python3
"""Tests of the E2 transport of scheduler commands.

The xApp is compiled against a stand-in for the FlexRIC xApp API and run; the two
control words it sends are compared with an encoding written out here
independently.  The FlexRIC patch script and the patch files are checked for their
structure.  Applying the patches to the real source trees is checked at image build
time (gnb/build-gnb-image.sh) and by the C test in gnb/test.
"""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPOSITORY = Path(__file__).resolve().parents[1]
XAPP = REPOSITORY / "xapp" / "xapp_sched_ctrl.c"
HEADER = REPOSITORY / "gnb" / "src" / "nr_dl_ctrl_e2.h"
PATCH_SCRIPT = REPOSITORY / "bin" / "patch-flexric-sched-ctrl.py"
OAI_PATCH = REPOSITORY / "gnb" / "patches" / "0002-e2-mac-control.patch"
FLEXRIC_PATCH = REPOSITORY / "gnb" / "patches" / "0003-flexric-mac-control-agent.fxpatch"

SPEC = importlib.util.spec_from_file_location("flexric_sched_ctrl_patch", PATCH_SCRIPT)
assert SPEC is not None and SPEC.loader is not None
PATCH_MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH_MODULE)

MAGIC = 0x5343
POLICIES = {"pf": 0, "rr": 1, "wshare": 2, "wrr": 3, "slice": 4}

# Stand-ins that have the shape of the FlexRIC declarations the xApp uses: headers in
# xapp/test/flexric_stub and a fake RIC in xapp/test/stub_api.c (see its header comment).
STUB = REPOSITORY / "xapp" / "test"
SLICE_STUB = REPOSITORY / "gnb" / "test" / "flexric_stub"


def encode(verb: str, a1: str = "", a2: str = "", a3: str = "") -> tuple[int, int]:
    """The documented encoding, written from the specification in nr_dl_ctrl_e2.h."""

    def slice_arg(text: str) -> int:
        return 0xFF if text == "none" else int(text)

    if verb == "slice-weight":
        return (MAGIC << 16) | (int(a1) << 8) | 5, int(a2)
    if verb == "slice-cap":
        return (MAGIC << 16) | (int(a1) << 8) | 6, int(a2)
    if verb == "assoc":
        return (MAGIC << 16) | (slice_arg(a2) << 8) | 7, int(a1, 16) << 16
    if verb == "nssai-map":
        sd = 0xFFFFFF if a2 == "none" else int(a2, 16)
        return (MAGIC << 16) | (slice_arg(a3) << 8) | 8, (int(a1) << 24) | sd
    if verb == "policy":
        return (MAGIC << 16) | 1, POLICIES[a1]
    if verb == "weight":
        rnti = 0 if a1 == "*" else int(a1, 16)
        return (MAGIC << 16) | 2, (rnti << 16) | int(a2)
    if verb == "cap":
        return (MAGIC << 16) | 3, (int(a1, 16) << 16) | int(a2)
    if verb == "reset":
        return (MAGIC << 16) | 4, 0
    raise ValueError(verb)


@unittest.skipUnless(shutil.which("gcc"), "gcc is not available")
class XappTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.directory = tempfile.TemporaryDirectory()
        root = Path(cls.directory.name)
        ctrl = root / "examples" / "xApp" / "c" / "ctrl"
        shutil.copytree(STUB / "flexric_stub" / "src", root / "src")
        ctrl.mkdir(parents=True)
        shutil.copyfile(XAPP, ctrl / XAPP.name)
        shutil.copyfile(HEADER, ctrl / HEADER.name)
        cls.binary = root / "xapp_sched_ctrl"
        build = subprocess.run(
            ["gcc", "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-I", str(SLICE_STUB), "-I", str(STUB / "flexric_stub"), str(ctrl / XAPP.name), str(STUB / "stub_api.c"),
             "-o", str(cls.binary), "-lpthread"],
            capture_output=True,
            text=True,
        )
        assert build.returncode == 0, build.stderr

    @classmethod
    def tearDownClass(cls) -> None:
        cls.directory.cleanup()

    def run_xapp(self, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        environment = dict(os.environ)
        environment.update(env)
        return subprocess.run([str(self.binary), *args], capture_output=True, text=True, env=environment, timeout=20)

    def words(self, result: subprocess.CompletedProcess[str]) -> list[tuple[int, int]]:
        found = re.findall(r"STAND_IN control rf=(\d+) hdr=([0-9a-f]{8}) msg=([0-9a-f]{8})", result.stdout)
        for rf, _, _ in found:
            self.assertEqual(rf, "142")
        return [(int(h, 16), int(m, 16)) for _, h, m in found]

    def check_command(self, *args: str) -> None:
        result = self.run_xapp(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # only node 0 is a gNB with the MAC service model
        self.assertEqual(self.words(result), [encode(*args)])
        self.assertIn("Scheduler command delivered", result.stdout)

    def test_policy_commands(self) -> None:
        for name in POLICIES:
            self.check_command("policy", name)

    def test_weight_commands(self) -> None:
        self.check_command("weight", "e350", "3000")
        self.check_command("weight", "a6a4", "1")
        self.check_command("weight", "ffff", "65535")
        self.check_command("weight", "*", "2000")

    def test_cap_commands(self) -> None:
        self.check_command("cap", "e350", "20")
        self.check_command("cap", "a6a4", "0")
        self.check_command("cap", "0001", "275")
        self.check_command("cap", "1", "5")

    def test_reset(self) -> None:
        self.check_command("reset")

    def test_slice_commands(self) -> None:
        self.check_command("slice-weight", "0", "1")
        self.check_command("slice-weight", "3", "3000")
        self.check_command("slice-weight", "7", "65535")
        self.check_command("slice-cap", "2", "30")
        self.check_command("slice-cap", "2", "0")
        self.check_command("slice-cap", "7", "275")
        self.check_command("assoc", "56a2", "1")
        self.check_command("assoc", "56a2", "0")
        self.check_command("assoc", "56a2", "none")
        self.check_command("nssai-map", "1", "none", "2")
        self.check_command("nssai-map", "2", "123456", "7")
        self.check_command("nssai-map", "255", "0", "none")

    def test_slice_words_match_the_documented_layout(self) -> None:
        self.assertEqual(encode("slice-weight", "3", "3000"), (0x53430305, 3000))
        self.assertEqual(encode("assoc", "1001", "none"), (0x5343FF07, 0x10010000))
        self.assertEqual(encode("nssai-map", "2", "none", "3"), (0x53430308, 0x02FFFFFF))
        self.assertEqual(encode("policy", "slice"), (0x53430001, 4))

    def test_invalid_commands_send_nothing(self) -> None:
        invalid = [
            [],
            ["policy"],
            ["policy", "bogus"],
            ["weight", "e350"],
            ["weight", "e350", "0"],
            ["weight", "e350", "65536"],
            ["weight", "e350", "-1"],
            ["weight", "0", "5"],
            ["weight", "10000", "5"],
            ["weight", "xyz", "5"],
            ["cap", "e350", "4"],
            ["cap", "e350", "276"],
            ["cap", "*", "20"],
            ["slice-weight", "8", "5"],
            ["slice-weight", "1", "0"],
            ["slice-weight", "1", "65536"],
            ["slice-weight", "1"],
            ["slice-cap", "1", "4"],
            ["slice-cap", "1", "276"],
            ["assoc", "e350", "8"],
            ["assoc", "0", "1"],
            ["assoc", "e350"],
            ["nssai-map", "256", "none", "1"],
            ["nssai-map", "1", "1000000", "1"],
            ["nssai-map", "1", "zz", "1"],
            ["nssai-map", "1", "none", "9"],
            ["nssai-map", "1", "none"],
            ["policy", "slice", "extra", "words", "here"],
            ["frobnicate"],
        ]
        for args in invalid:
            with self.subTest(args=args):
                result = self.run_xapp(*args)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertNotIn("STAND_IN", result.stdout)
                self.assertIn("usage:", result.stderr)

    # ---- the standard slice service model (verbs sm-add, sm-del, sm-assoc, sm-get) ----

    def slice_run(self, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        result = self.run_xapp(*args, **env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # node 0 is the only gNB with the slice service model; the eNB and the gNB without it are skipped
        self.assertEqual(result.stdout.count("STAND_IN control rf=145"), 1, result.stdout)
        self.assertIn("Node 1: no slice service model on a gNB, skipped", result.stdout)
        self.assertIn("Node 2: no slice service model on a gNB, skipped", result.stdout)
        self.assertIn("Slice message delivered", result.stdout)
        return result

    def test_sm_add_nvs(self) -> None:
        out = self.slice_run("sm-add", "nvs", "1:0.75", "2:0.25").stdout
        self.assertIn(
            "STAND_IN control rf=145 ADD\n"
            "  dl sched=PF slices=2\n"
            "  slice id=1 label=slice-1 sched=PF nvs-capacity 0.750\n"
            "  slice id=2 label=slice-2 sched=PF nvs-capacity 0.250\n"
            "  ul sched=round_robin_ul slices=0\n",
            out,
        )
        self.assertIn("Node 0: slice message (2 items) acknowledged", out)

    def test_sm_add_static(self) -> None:
        out = self.slice_run("sm-add", "static", "3:0-9", "5:10-30").stdout
        self.assertIn("  slice id=3 label=slice-3 sched=PF static 0 9\n  slice id=5 label=slice-5 sched=PF static 10 30\n", out)

    def test_sm_add_none_sends_no_slice(self) -> None:
        out = self.slice_run("sm-add", "none").stdout
        self.assertIn("STAND_IN control rf=145 ADD\n  dl sched=PF slices=0\n  ul sched=round_robin_ul slices=0\n", out)
        self.assertIn("slice message (0 items)", out)

    def test_sm_del(self) -> None:
        out = self.slice_run("sm-del", "3", "4", "7").stdout
        self.assertIn("STAND_IN control rf=145 DEL dl=3,4,7 ul=0\n", out)

    def test_sm_assoc_takes_the_rnti_in_hexadecimal(self) -> None:
        out = self.slice_run("sm-assoc", "9e82:1", "53E4:2", "1:0").stdout
        self.assertIn(
            "STAND_IN control rf=145 ASSOC\n"
            "  ue rnti=0x9e82 dl=1 ul=0\n"
            "  ue rnti=0x53e4 dl=2 ul=0\n"
            "  ue rnti=0x0001 dl=0 ul=0\n",
            out,
        )

    def test_sm_values_are_left_to_the_gnb(self) -> None:
        # only the syntax is checked here: the gNB refuses these and says why in its log
        out = self.slice_run("sm-add", "nvs", "1:1.5", "9:0", "4294967295:0.5").stdout
        self.assertIn("slice id=1 label=slice-1 sched=PF nvs-capacity 1.500", out)
        self.assertIn("slice id=9 label=slice-9 sched=PF nvs-capacity 0.000", out)
        self.assertIn("slice id=4294967295 label=slice-4294967295 sched=PF nvs-capacity 0.500", out)
        self.slice_run("sm-add", "static", "3:0-3")
        self.slice_run("sm-del", "8")

    def test_sm_up_to_32_items(self) -> None:
        self.slice_run("sm-del", *[str(i) for i in range(32)])
        result = self.run_xapp("sm-del", *[str(i) for i in range(33)])
        self.assertEqual(result.returncode, 2)
        self.assertIn("too many items", result.stderr)

    def test_sm_get_prints_the_indication(self) -> None:
        result = self.run_xapp("sm-get")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            "Node 0: slice indication\n"
            "scheduler: wrr\n"
            "slices: 2\n"
            "  slice 1 label slice-1 sched wrr nvs capacity pct_reserved 0.750\n"
            "  slice 2 label slice-2 sched wrr nvs capacity pct_reserved 0.250\n"
            "ues: 2\n"
            "  rnti 0x9e82 dl slice 1\n"
            "  rnti 0x53e4 dl slice 2\n",
            result.stdout,
        )
        self.assertIn("Node 1: no slice service model on a gNB, skipped", result.stdout)
        self.assertTrue(result.stdout.endswith("Slice indication read\n"))
        self.assertNotIn("STAND_IN control", result.stdout)

    def test_sm_get_without_an_indication_fails(self) -> None:
        result = self.run_xapp("sm-get", STAND_IN_SILENT="1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no slice indication within 5 s", result.stdout)

    def test_sm_invalid_commands_send_nothing(self) -> None:
        invalid = [
            ["sm-add"],
            ["sm-add", "nvs"],
            ["sm-add", "bogus", "1:0.5"],
            ["sm-add", "none", "1:0.5"],
            ["sm-add", "nvs", "1"],
            ["sm-add", "nvs", "1:"],
            ["sm-add", "nvs", ":0.5"],
            ["sm-add", "nvs", "1:abc"],
            ["sm-add", "nvs", "1:0.5x"],
            ["sm-add", "nvs", "1:nan"],
            ["sm-add", "nvs", "1:inf"],
            ["sm-add", "nvs", "-1:0.5"],
            ["sm-add", "nvs", "4294967296:0.5"],
            ["sm-add", "static", "1:5"],
            ["sm-add", "static", "1:5-"],
            ["sm-add", "static", "1:-5"],
            ["sm-add", "static", "1:5-x"],
            ["sm-del"],
            ["sm-del", "-1"],
            ["sm-del", "x"],
            ["sm-del", "1:2"],
            ["sm-assoc"],
            ["sm-assoc", "9e82"],
            ["sm-assoc", "0:1"],
            ["sm-assoc", "12345:1"],
            ["sm-assoc", "zz:1"],
            ["sm-assoc", "9e82:x"],
            ["sm-get", "now"],
            ["sm-bogus"],
        ]
        for args in invalid:
            with self.subTest(args=args):
                result = self.run_xapp(*args)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertNotIn("STAND_IN", result.stdout)
                self.assertIn("error:", result.stderr)
                self.assertIn("usage:", result.stderr)

    def test_sm_refused_control_is_reported(self) -> None:
        result = self.run_xapp("sm-del", "3", STAND_IN_REFUSE="1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not acknowledged", result.stdout)
        self.assertIn("Slice message not delivered", result.stdout)

    def test_sm_without_a_suitable_node_is_a_failure(self) -> None:
        result = self.run_xapp("sm-del", "3", STAND_IN_NODES="0")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("STAND_IN control", result.stdout)
        result = self.run_xapp("sm-assoc", "9e82:1", STAND_IN_NODES="3")
        self.assertEqual(result.returncode, 0)  # node 0 is a gNB with the slice model

    def test_refused_control_is_reported(self) -> None:
        result = self.run_xapp("policy", "pf", STAND_IN_REFUSE="1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not acknowledged", result.stdout)
        self.assertIn("not delivered", result.stdout)

    def test_no_suitable_node_is_a_failure(self) -> None:
        # nodes 1 (eNB) and 2 (no MAC service model) only
        result = self.run_xapp("reset", STAND_IN_NODES="0")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.words(result), [])


# The flags of the OAI build (the cmake line of the gNB image) apply -Werror to every warning.
OAI_FLAGS = (
    "-O2 -Wall -Wextra -Werror -Wunused-parameter -Wno-unused-result -std=gnu11 "
    "-fno-strict-aliasing -funroll-loops -fno-delete-null-pointer-checks"
).split()

# The shape of the gNB handler in gnb/patches/0002-e2-mac-control.patch: fixed-size
# buffers handed to the helpers of the header.
HANDLER_UNIT = r"""
#include <stdio.h>
#include <stdint.h>
#include "nr_dl_ctrl_e2.h"
int handler(uint32_t h, uint32_t a)
{
  char text[128];
  char reply[256];
  if (nr_ctrl_e2_to_text(h, a, text, sizeof(text)) != 0) {
    printf("ignored %08x %08x\n", (unsigned)h, (unsigned)a);
  } else if (nr_ctrl_e2_send(text, reply, sizeof(reply)) == 0) {
    printf("applied %s\n", text);
  } else {
    printf("refused %s: %s\n", text, reply[0] ? reply : "no answer");
  }
  return 0;
}
"""


@unittest.skipUnless(shutil.which("gcc"), "gcc is not available")
class OaiBuildFlagsTest(unittest.TestCase):
    def test_header_compiles_with_the_oai_build_flags(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "handler.c"
            source.write_text(HANDLER_UNIT)
            for fortify in ("-D_FORTIFY_SOURCE=0", "-D_FORTIFY_SOURCE=2", "-D_FORTIFY_SOURCE=3"):
                with self.subTest(fortify=fortify):
                    result = subprocess.run(
                        ["gcc", *OAI_FLAGS, fortify, "-I", str(HEADER.parent), "-c", str(source), "-o", os.devnull],
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_scheduler_sources_compile_with_the_oai_build_flags(self) -> None:
        # the sources that go into the gNB image, against the stand-in OAI headers, with the -Werror flags of the OAI build
        src = REPOSITORY / "gnb" / "src"
        stub = REPOSITORY / "gnb" / "test" / "oai_stub"
        for name in ("nr_dl_ctrl_state.c", "nr_dl_ctrl_alloc.c", "gNB_scheduler_dlsch_ctrl_policy.c"):
            for fortify in ("-D_FORTIFY_SOURCE=0", "-D_FORTIFY_SOURCE=2", "-D_FORTIFY_SOURCE=3"):
                with self.subTest(source=name, fortify=fortify):
                    result = subprocess.run(
                        ["gcc", *OAI_FLAGS, fortify, "-DMAX_MOBILES_PER_GNB=32", "-I", str(stub), "-I", str(src), "-c", str(src / name), "-o", os.devnull],
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_reply_is_truncated_not_overrun(self) -> None:
        # a reply longer than the caller's buffer is cut and terminated
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "reply.c"
            source.write_text(
                r"""
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include "nr_dl_ctrl_e2.h"
static int fd;
static void *serve(void *unused)
{
  char b[64];
  struct sockaddr_in from;
  socklen_t n = sizeof(from);
  (void)unused;
  ssize_t r = recvfrom(fd, b, sizeof(b), 0, (struct sockaddr *)&from, &n);
  if (r > 0) {
    char big[600];
    memset(big, 'x', sizeof(big));
    memcpy(big, "OK ", 3);
    sendto(fd, big, sizeof(big), 0, (struct sockaddr *)&from, n);
  }
  return NULL;
}
int main(void)
{
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t l = sizeof(a);
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  assert(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
  assert(getsockname(fd, (struct sockaddr *)&a, &l) == 0);
  char port[16];
  snprintf(port, sizeof(port), "%d", ntohs(a.sin_port));
  setenv("SCHED_CTRL_PORT", port, 1);
  pthread_t t;
  pthread_create(&t, NULL, serve, NULL);
  char reply[32];
  memset(reply, 'z', sizeof(reply));
  int rc = nr_ctrl_e2_send("PING", reply, sizeof(reply));
  pthread_join(t, NULL);
  assert(rc == 0);
  assert(strlen(reply) == sizeof(reply) - 1);
  assert(strncmp(reply, "OK xxx", 6) == 0);
  return 0;
}
"""
            )
            binary = Path(directory) / "reply"
            build = subprocess.run(
                ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(HEADER.parent), str(source), "-o", str(binary), "-lpthread"],
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stderr)


class PatchScriptTest(unittest.TestCase):
    RIC_SOURCE = (
        "static void f(void)\n{\n"
        "  mac_ctrl_req_data_t const* req = ctrl;\n"
        "  assert(req->hdr.dummy == 1);\n"
        "  assert(req->msg.action == 42);\n"
        "  use(req);\n}\n"
    )

    def make_tree(self, root: Path, ric_source: str | None = None) -> Path:
        flexric = root / "flexric"
        (flexric / "src" / "sm" / "mac_sm").mkdir(parents=True)
        (flexric / "examples" / "xApp" / "c" / "ctrl").mkdir(parents=True)
        (flexric / "src" / "sm" / "mac_sm" / "mac_sm_ric.c").write_text(ric_source or self.RIC_SOURCE)
        (flexric / "examples" / "xApp" / "c" / "ctrl" / "CMakeLists.txt").write_text(
            "add_executable(xapp_mac_ctrl\n  mac_ctrl.c\n  )\n"
        )
        return flexric

    def test_patches_once_and_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            flexric = self.make_tree(Path(directory))
            self.assertTrue(PATCH_MODULE.patch_tree(flexric))
            snapshot = {p: p.read_bytes() for p in flexric.rglob("*") if p.is_file()}
            self.assertFalse(PATCH_MODULE.patch_tree(flexric))
            self.assertEqual(snapshot, {p: p.read_bytes() for p in flexric.rglob("*") if p.is_file()})

            ric = (flexric / "src" / "sm" / "mac_sm" / "mac_sm_ric.c").read_text()
            self.assertNotIn("assert(req->hdr.dummy == 1)", ric)
            self.assertNotIn("assert(req->msg.action == 42)", ric)
            self.assertIn("use(req);", ric)
            ctrl = flexric / "examples" / "xApp" / "c" / "ctrl"
            self.assertEqual((ctrl / "xapp_sched_ctrl.c").read_bytes(), XAPP.read_bytes())
            self.assertEqual((ctrl / "nr_dl_ctrl_e2.h").read_bytes(), HEADER.read_bytes())
            cmake = (ctrl / "CMakeLists.txt").read_text()
            self.assertEqual(cmake.count("add_executable(xapp_sched_ctrl"), 1)
            self.assertIn("add_executable(xapp_mac_ctrl", cmake)

    def test_updated_xapp_source_is_copied_again(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            flexric = self.make_tree(Path(directory))
            PATCH_MODULE.patch_tree(flexric)
            copied = flexric / "examples" / "xApp" / "c" / "ctrl" / "xapp_sched_ctrl.c"
            copied.write_text("old\n")
            PATCH_MODULE.patch_tree(flexric)
            self.assertEqual(copied.read_bytes(), XAPP.read_bytes())

    def test_rejects_unknown_source_revision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            flexric = self.make_tree(Path(directory), "static void f(void) {}\n")
            with self.assertRaises(RuntimeError):
                PATCH_MODULE.patch_tree(flexric)

    def test_rejects_missing_cmake_target_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            flexric = self.make_tree(Path(directory))
            (flexric / "examples" / "xApp" / "c" / "ctrl" / "CMakeLists.txt").unlink()
            with self.assertRaises(RuntimeError):
                PATCH_MODULE.patch_tree(flexric)

    def test_command_line(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            flexric = self.make_tree(Path(directory))
            ok = subprocess.run(["python3", str(PATCH_SCRIPT), str(flexric)], capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            missing = subprocess.run(["python3", str(PATCH_SCRIPT), str(flexric / "nope")], capture_output=True, text=True)
            self.assertEqual(missing.returncode, 2)
            usage = subprocess.run(["python3", str(PATCH_SCRIPT)], capture_output=True, text=True)
            self.assertEqual(usage.returncode, 2)


class PatchFilesTest(unittest.TestCase):
    def changed_lines(self, path: Path) -> tuple[list[str], list[str], list[str]]:
        text = path.read_text()
        files = re.findall(r"^diff --git a/(\S+) b/", text, re.M)
        added = [l[1:] for l in text.splitlines() if l.startswith("+") and not l.startswith("+++")]
        removed = [l[1:] for l in text.splitlines() if l.startswith("-") and not l.startswith("---")]
        return files, added, removed

    def test_gnb_agent_patch_only_removes_the_two_asserts(self) -> None:
        files, added, removed = self.changed_lines(FLEXRIC_PATCH)
        self.assertEqual(files, ["src/sm/mac_sm/mac_sm_agent.c"])
        self.assertEqual(added, [])
        self.assertEqual(len(removed), 2)
        self.assertTrue(any("hdr.dummy == 1" in l for l in removed))
        self.assertTrue(any("msg.action == 42" in l for l in removed))

    def test_oai_patch_touches_only_the_mac_function(self) -> None:
        files, added, removed = self.changed_lines(OAI_PATCH)
        self.assertEqual(files, ["openair2/E2AP/RAN_FUNCTION/CUSTOMIZED/ran_func_mac.c"])
        self.assertEqual(len(removed), 1)
        self.assertIn("operation not supported", removed[0])
        text = "\n".join(added)
        self.assertIn('#include "openair2/LAYER2/NR_MAC_gNB/nr_dl_ctrl_e2.h"', text)
        self.assertIn("nr_ctrl_e2_to_text", text)
        self.assertIn("nr_ctrl_e2_send", text)

    def test_build_script_applies_both_patch_kinds(self) -> None:
        script = (REPOSITORY / "gnb" / "build-gnb-image.sh").read_text()
        self.assertIn(".fxpatch", script)
        self.assertIn("is not a scheduler command", script)

    def test_core_setup_builds_the_xapp(self) -> None:
        script = (REPOSITORY / "bin" / "core-setup.sh").read_text()
        self.assertIn("patch-flexric-sched-ctrl.py", script)
        self.assertIn("xapp_sched_ctrl", script)


if __name__ == "__main__":
    unittest.main()
