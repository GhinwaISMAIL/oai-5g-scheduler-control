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
POLICIES = {"pf": 0, "rr": 1, "wshare": 2, "wrr": 3}

# Stand-ins that have the shape of the FlexRIC declarations the xApp uses.
STAND_IN_API = r"""
#ifndef STAND_IN_E42_XAPP_API_H
#define STAND_IN_E42_XAPP_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { int unused; } fr_args_t;
typedef enum { ngran_gNB = 2, ngran_gNB_DU = 4, ngran_eNB = 0 } ngran_node_t;
typedef struct { ngran_node_t type; } global_e2_node_id_t;
typedef struct { uint16_t id; } sm_ran_function_t;
typedef struct { global_e2_node_id_t id; size_t len_rf; sm_ran_function_t *rf; } e2_node_connected_xapp_t;
typedef struct { int len; e2_node_connected_xapp_t *n; } e2_node_arr_xapp_t;
typedef struct { uint32_t dummy; } mac_ctrl_hdr_t;
typedef struct { uint32_t action; } mac_ctrl_msg_t;
typedef struct { mac_ctrl_hdr_t hdr; mac_ctrl_msg_t msg; } mac_ctrl_req_data_t;
typedef struct { bool success; } sm_ans_xapp_t;

static inline fr_args_t init_fr_args(int argc, char *argv[]) { (void)argc; (void)argv; fr_args_t a = {0}; return a; }
static inline void init_xapp_api(fr_args_t const *a) { (void)a; }
static inline bool try_stop_xapp_api(void) { return true; }
static inline e2_node_arr_xapp_t e2_nodes_xapp_api(void)
{
  /* node 0: a gNB with the MAC service model; node 1: an eNB without it;
   * node 2: a gNB that only has another service model */
  static sm_ran_function_t mac[] = {{142}, {2}};
  static sm_ran_function_t kpm[] = {{2}};
  static e2_node_connected_xapp_t nodes[3];
  nodes[0] = (e2_node_connected_xapp_t){{ngran_gNB}, 2, mac};
  nodes[1] = (e2_node_connected_xapp_t){{ngran_eNB}, 2, mac};
  nodes[2] = (e2_node_connected_xapp_t){{ngran_gNB}, 1, kpm};
  int count = getenv("STAND_IN_NODES") ? atoi(getenv("STAND_IN_NODES")) : 3;
  return (e2_node_arr_xapp_t){count, nodes};
}
static inline void free_e2_node_arr_xapp(e2_node_arr_xapp_t *a) { (void)a; }
static inline sm_ans_xapp_t control_sm_xapp_api(global_e2_node_id_t *id, uint32_t rf_id, void *wr)
{
  mac_ctrl_req_data_t *r = wr;
  (void)id;
  printf("STAND_IN control rf=%u hdr=%08x msg=%08x\n", (unsigned)rf_id, (unsigned)r->hdr.dummy, (unsigned)r->msg.action);
  return (sm_ans_xapp_t){getenv("STAND_IN_REFUSE") == NULL};
}
#endif
"""

STAND_IN_DEFER = r"""
#ifndef STAND_IN_DEFER_H
#define STAND_IN_DEFER_H
#define STAND_IN_JOIN2(a, b) a##b
#define STAND_IN_JOIN(a, b) STAND_IN_JOIN2(a, b)
#define defer(code) \
  void STAND_IN_JOIN(defer_fn_, __LINE__)(int *u) { (void)u; code; } \
  int STAND_IN_JOIN(defer_var_, __LINE__) __attribute__((cleanup(STAND_IN_JOIN(defer_fn_, __LINE__)))) = 0
#endif
"""


def encode(verb: str, a1: str = "", a2: str = "") -> tuple[int, int]:
    """The documented encoding, written from the specification in nr_dl_ctrl_e2.h."""
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
        (root / "src" / "xApp").mkdir(parents=True)
        (root / "src" / "util" / "alg_ds" / "alg").mkdir(parents=True)
        ctrl.mkdir(parents=True)
        (root / "src" / "xApp" / "e42_xapp_api.h").write_text(STAND_IN_API)
        (root / "src" / "util" / "alg_ds" / "alg" / "defer.h").write_text(STAND_IN_DEFER)
        shutil.copyfile(XAPP, ctrl / XAPP.name)
        shutil.copyfile(HEADER, ctrl / HEADER.name)
        cls.binary = root / "xapp_sched_ctrl"
        build = subprocess.run(
            ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(ctrl / XAPP.name), "-o", str(cls.binary)],
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
            ["frobnicate"],
        ]
        for args in invalid:
            with self.subTest(args=args):
                result = self.run_xapp(*args)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertNotIn("STAND_IN", result.stdout)
                self.assertIn("usage:", result.stderr)

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
