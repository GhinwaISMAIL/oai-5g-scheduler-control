import json
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
GNB = ROOT / "gnb"
CLIENT = ROOT / "bin" / "sched-ctl.py"

needs_gcc = pytest.mark.skipif(shutil.which("gcc") is None or shutil.which("make") is None, reason="gcc and make are required")


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@needs_gcc
def test_native_policy_tests_pass(tmp_path):
    work = tmp_path / "test"
    shutil.copytree(GNB / "test", work, ignore=shutil.ignore_patterns("test_ctrl", "test_ctrl_*"))
    shutil.copytree(GNB / "src", tmp_path / "src")
    built = subprocess.run(["make", "-C", str(work)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr[-3000:]
    ran = subprocess.run([str(work / "test_ctrl")], capture_output=True, text=True, timeout=300)
    assert "failures=0" in ran.stdout, ran.stdout + ran.stderr[-2000:]
    assert ran.returncode == 0


@needs_gcc
def test_sources_compile_in_strict_c11(tmp_path):
    work = tmp_path / "test"
    shutil.copytree(GNB / "test", work, ignore=shutil.ignore_patterns("test_ctrl", "test_ctrl_*"))
    shutil.copytree(GNB / "src", tmp_path / "src")
    assert subprocess.run(["make", "-C", str(work), "strict"], capture_output=True).returncode == 0


@pytest.fixture
def server(tmp_path):
    if shutil.which("gcc") is None:
        pytest.skip("gcc is required")
    exe = tmp_path / "ctrl_server"
    built = subprocess.run(
        ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(GNB / "src"), str(GNB / "test" / "ctrl_server.c"),
         str(GNB / "src" / "nr_dl_ctrl_state.c"), "-o", str(exe)],
        capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    port = free_port()
    proc = subprocess.Popen([str(exe), str(port)], stdout=subprocess.PIPE, text=True)
    assert proc.stdout.readline().strip() == "ready"
    yield port
    proc.kill()
    proc.wait()


def client(port, *args):
    return subprocess.run([sys.executable, str(CLIENT), "--port", str(port), "--timeout", "2", *args],
                          capture_output=True, text=True, timeout=30)


def test_client_ping_and_status(server):
    assert client(server, "ping").stdout.startswith("OK")
    out = client(server, "status").stdout
    assert "policy=pf" in out and "0x1001" in out and "0x1002" in out


def test_client_policy_weight_cap_and_reset_are_read_back(server):
    r = client(server, "policy", "wshare")
    assert r.returncode == 0 and "policy=wshare" in r.stdout
    r = client(server, "policy", "wrr")
    assert r.returncode == 0 and "policy=wrr" in r.stdout
    r = client(server, "weight", "1001", "3000")
    assert r.returncode == 0
    row = [line for line in r.stdout.splitlines() if line.startswith("0x1001")][0].split()
    assert row[1] == "3000" and row[2] == "1"
    assert client(server, "weight", "*", "2000").returncode == 0
    r = client(server, "cap", "1002", "40")
    assert r.returncode == 0
    row = [line for line in r.stdout.splitlines() if line.startswith("0x1002")][0].split()
    assert row[3] == "40"
    r = client(server, "reset")
    assert r.returncode == 0 and "policy=pf" in r.stdout


@pytest.mark.parametrize("args", [("weight", "1001", "0"), ("weight", "9999", "10"), ("cap", "1001", "3"), ("weight", "zz", "10")])
def test_client_reports_rejections(server, args):
    r = client(server, *args)
    assert r.returncode == 1 and "ERROR" in r.stderr


def test_client_measure_reports_shares_from_counter_deltas(server):
    r = client(server, "measure", "--seconds", "0.2")
    assert r.returncode == 0, r.stderr
    rows = {line.split()[0]: line.split() for line in r.stdout.splitlines() if line.startswith("0x")}
    assert float(rows["0x1001"][2]) == pytest.approx(75.0, abs=0.1)
    assert float(rows["0x1002"][2]) == pytest.approx(25.0, abs=0.1)
    assert "total:" in r.stdout


def test_client_fails_cleanly_without_a_gnb():
    r = client(free_port(), "status")
    assert r.returncode == 1 and "no reply" in r.stderr


def test_patch_touches_only_the_registration_points():
    text = (GNB / "patches" / "0001-controllable-dl-rb-policy.patch").read_text()
    files = re.findall(r"^diff --git a/(\S+) b/", text, re.M)
    assert files == ["CMakeLists.txt", "openair2/LAYER2/NR_MAC_gNB/main.c"]
    added = [line[1:] for line in text.splitlines() if line.startswith("+") and not line.startswith("+++")]
    assert any("gNB_scheduler_dlsch_ctrl_policy.c" in a for a in added)
    assert any("nr_dl_ctrl_alloc.c" in a for a in added)
    assert any("nr_dl_ctrl_state.c" in a for a in added)
    assert any("nr_dl_ctrl_policy_init(RC.nrmac[i])" in a for a in added)
    assert not [line for line in text.splitlines() if line.startswith("-") and not line.startswith("---")]


def test_every_source_file_listed_in_the_patch_exists():
    text = (GNB / "patches" / "0001-controllable-dl-rb-policy.patch").read_text()
    for name in re.findall(r"\+\s+\$\{NR_GNB_MAC_DIR\}/(\S+\.c)", text):
        assert (GNB / "src" / name).exists(), name


def test_build_script_uses_the_pinned_revision_and_documented_targets():
    script = (GNB / "build-gnb-image.sh").read_text()
    assert subprocess.run(["bash", "-n", str(GNB / "build-gnb-image.sh")]).returncode == 0
    for target in ("--target ran-base", "--target ran-build", "--target oai-gnb"):
        assert target in script
    assert "oai_revision" in script and "stack-versions.json" in script
    assert "git -C \"${OAI_SRC_DIR}\" apply" in script
    versions = json.loads((ROOT / "etc" / "stack-versions.json").read_text())
    assert re.fullmatch(r"[0-9a-f]{40}", versions["oai_revision"])


def test_cell_setup_defaults_to_the_stock_image_and_uses_the_selected_one():
    text = (ROOT / "bin" / "cell-setup.sh").read_text()
    assert 'GNB_IMAGE="${STOCK_GNB_IMAGE}"' in text
    assert '[ "${GNB_CTRL:-0}" = 1 ]' in text
    assert "    image: ${GNB_IMAGE}\n    network_mode: host" in text
    assert text.count("ghinwa555/oai-gnb-e2-chan@sha256") == 1
