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


def test_client_slice_commands_are_read_back(server):
    assert client(server, "policy", "slice").returncode == 0
    r = client(server, "slice", "1", "weight", "3000")
    assert r.returncode == 0, r.stderr
    assert any(line.split()[:3] == ["1", "3000", "0"] for line in r.stdout.splitlines() if (line.split() or [''])[0].isdigit())
    r = client(server, "slice", "2", "cap", "30")
    assert r.returncode == 0 and any(line.split()[:3] == ["2", "1000", "30"] for line in r.stdout.splitlines() if (line.split() or [''])[0].isdigit())
    # the default value set explicitly is still read back
    r = client(server, "slice", "3", "weight", "1000")
    assert r.returncode == 0 and any(line.split()[:2] == ["3", "1000"] for line in r.stdout.splitlines() if (line.split() or [''])[0].isdigit())
    r = client(server, "assoc", "1001", "1")
    assert r.returncode == 0, r.stderr
    row = [line for line in r.stdout.splitlines() if line.startswith("0x1001")][0].split()
    assert row[-3:-1] == ["1", "rnti"]
    r = client(server, "nssai-map", "2", "none", "3")
    assert r.returncode == 0 and "rule: S-NSSAI sst=2 sd=none -> slice 3" in r.stdout
    r = client(server, "nssai-map", "2", "123456", "4")
    assert r.returncode == 0 and "sst=2 sd=123456 -> slice 4" in r.stdout
    r = client(server, "nssai-map", "2", "123456", "-")
    assert r.returncode == 0 and "sd=123456" not in r.stdout
    r = client(server, "assoc", "1001", "-")
    assert r.returncode == 0
    row = [line for line in r.stdout.splitlines() if line.startswith("0x1001")][0].split()
    assert row[-3:-1] == ["0", "default"]
    r = client(server, "reset")
    assert r.returncode == 0 and "policy=pf" in r.stdout and "rule:" not in r.stdout
    assert not any((line.split() or [''])[0].isdigit() for line in r.stdout.splitlines())


def test_client_slice_del_resets_one_slice(server):
    assert client(server, "policy", "slice").returncode == 0
    assert client(server, "slice", "3", "weight", "4000").returncode == 0
    assert client(server, "slice", "3", "cap", "20").returncode == 0
    assert client(server, "slice", "2", "weight", "2000").returncode == 0
    assert client(server, "assoc", "1001", "3").returncode == 0
    assert client(server, "assoc", "1002", "2").returncode == 0
    r = client(server, "nssai-map", "1", "none", "3")
    assert r.returncode == 0 and "sst=1 sd=none -> slice 3" in r.stdout
    r = client(server, "slice-del", "3")
    assert r.returncode == 0, r.stderr
    rows = [line.split() for line in r.stdout.splitlines() if (line.split() or [''])[0].isdigit()]
    assert not any(row[0] == "3" for row in rows) and any(row[0] == "2" and row[1] == "2000" for row in rows)
    assert "rule:" not in r.stdout
    row = [line for line in r.stdout.splitlines() if line.startswith("0x1001")][0].split()
    assert row[-3:-1] == ["0", "default"]
    row = [line for line in r.stdout.splitlines() if line.startswith("0x1002")][0].split()
    assert row[-3:-1] == ["2", "rnti"]
    assert client(server, "slice-del", "3").returncode == 0  # an empty slice can be deleted again
    assert client(server, "reset").returncode == 0


@pytest.mark.parametrize("arg", ["8", "-1", "x"])
def test_client_slice_del_rejects_a_bad_slice(server, arg):
    r = client(server, "slice-del", arg)
    assert r.returncode != 0


def slice_handler_source():
    """The new ran_func_slice.c, rebuilt from the lines of the patch that are kept or added (the patch holds the whole file)."""
    text = (GNB / "patches" / "0004-e2-slice-control.patch").read_text()
    hunks = text.split("\n@@", 1)[1].split("\n", 1)[1]
    return "\n".join(line[1:] for line in hunks.split("\n") if line.startswith(("+", " "))) + "\n"


def test_slice_handler_patch_touches_only_the_slice_handler():
    text = (GNB / "patches" / "0004-e2-slice-control.patch").read_text()
    assert re.findall(r"^diff --git a/(\S+) b/", text, re.M) == ["openair2/E2AP/RAN_FUNCTION/CUSTOMIZED/ran_func_slice.c"]
    assert text.count("\n@@") == 1  # one hunk with the whole file, so the test below sees all of it
    source = slice_handler_source()
    assert "fill_slice_ind_data" not in source and "fill_rnd_data_slice" not in source  # no random data
    assert "nr_ctrl_sm_plan_add" in source and "nr_ctrl_sm_plan_del" in source and "nr_ctrl_sm_plan_assoc" in source


@needs_gcc
def test_slice_handler_runs_against_the_control_endpoint(tmp_path):
    (tmp_path / "ran_func_slice.c").write_text(slice_handler_source())
    inc = tmp_path / "inc" / "openair2" / "LAYER2"
    inc.mkdir(parents=True)
    (inc / "NR_MAC_gNB").symlink_to(GNB / "src")
    stub = GNB / "test" / "flexric_stub"
    flags = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wunused-parameter", "-fsanitize=address,undefined"]
    handler = tmp_path / "handler"
    r = subprocess.run(["gcc", *flags, "-I", str(stub), "-I", str(stub / "openair2/E2AP/RAN_FUNCTION/CUSTOMIZED"), "-I", str(tmp_path / "inc"),
                        str(tmp_path / "ran_func_slice.c"), str(GNB / "test" / "test_slice_handler.c"), "-o", str(handler), "-lm"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    srv = tmp_path / "ctrl_server"
    r = subprocess.run(["gcc", "-std=gnu11", "-I", str(GNB / "src"), str(GNB / "test" / "ctrl_server.c"), str(GNB / "src" / "nr_dl_ctrl_state.c"), "-o", str(srv), "-lm"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    port = free_port()
    proc = subprocess.Popen([str(srv), str(port)], stdout=subprocess.PIPE, text=True)
    try:
        assert proc.stdout.readline().strip() == "ready"
        r = subprocess.run([str(handler), str(port)], capture_output=True, text=True, timeout=60)
    finally:
        proc.kill()
        proc.wait()
    assert r.returncode == 0, r.stdout + r.stderr
    assert "failures=0" in r.stdout


def test_client_measure_reports_slices(server):
    assert client(server, "policy", "slice").returncode == 0
    assert client(server, "assoc", "1001", "1").returncode == 0
    assert client(server, "assoc", "1002", "2").returncode == 0
    r = client(server, "measure", "--seconds", "0.2")
    assert r.returncode == 0, r.stderr
    rows = {line.split()[0]: line.split() for line in r.stdout.splitlines() if line[:1] in "12" and len(line.split()) == 5}
    assert float(rows["1"][3]) == pytest.approx(75.0, abs=0.1)
    assert float(rows["2"][3]) == pytest.approx(25.0, abs=0.1)
    # the policy-side share is printed under the slice table, from the per-UE grant counters
    grants = [line for line in r.stdout.splitlines() if line.startswith("policy grants in window")]
    assert len(grants) == 1 and "1:" in grants[0] and "2:" in grants[0], r.stdout


@pytest.mark.parametrize("args", [("weight", "1001", "0"), ("weight", "9999", "10"), ("cap", "1001", "3"), ("weight", "zz", "10"),
                                  ("slice", "9", "weight", "10"), ("slice", "1", "weight", "0"), ("slice", "1", "cap", "3"),
                                  ("assoc", "9999", "1"), ("assoc", "1001", "8"), ("assoc", "1001", "x"),
                                  ("nssai-map", "1", "none", "8"), ("nssai-map", "300", "none", "1"), ("nssai-map", "1", "zz", "1"),
                                  ("nssai-map", "1", "none", "-")])
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


def test_build_script_refuses_to_run_twice_at_once():
    script = (GNB / "build-gnb-image.sh").read_text()
    assert "flock -n 9" in script
    assert script.index("flock -n 9") < script.index("git clone")
