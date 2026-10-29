import re
import shutil
import socket
import subprocess
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "bin" / "ready-lib.sh"

needs_tools = pytest.mark.skipif(
    shutil.which("curl") is None or shutil.which("python3") is None,
    reason="curl and python3 are required",
)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_bash(script, timeout=60):
    return subprocess.run(
        ["bash", "-c", script], capture_output=True, text=True, timeout=timeout
    )


@pytest.fixture
def marker_server(tmp_path):
    port = free_port()
    log = tmp_path / "server.log"
    result = run_bash(
        f'. "{LIB}"; ready_server_start "{tmp_path}" 127.0.0.1 {port} "{log}"'
    )
    assert result.returncode == 0, result.stderr
    time.sleep(0.5)
    yield tmp_path, f"http://127.0.0.1:{port}"
    run_bash(f'pkill -f "http.server {port} --bind 127.0.0.1"')


@needs_tools
def test_wait_returns_zero_when_ready_marker_exists(marker_server):
    directory, url = marker_server
    (directory / "ric-ready").touch()
    result = run_bash(f'. "{LIB}"; wait_for_ready {url}/ric-ready {url}/failed 10 1')
    assert result.returncode == 0


@needs_tools
def test_wait_returns_two_when_failure_marker_exists(marker_server):
    directory, url = marker_server
    (directory / "failed").write_text("build broke")
    result = run_bash(f'. "{LIB}"; wait_for_ready {url}/ric-ready {url}/failed 10 1')
    assert result.returncode == 2


@needs_tools
def test_wait_times_out_with_one_when_nothing_is_published(marker_server):
    _, url = marker_server
    result = run_bash(f'. "{LIB}"; wait_for_ready {url}/ric-ready {url}/failed 2 1')
    assert result.returncode == 1


@needs_tools
def test_wait_picks_up_marker_published_while_waiting(marker_server):
    directory, url = marker_server
    waiter = subprocess.Popen(
        ["bash", "-c", f'. "{LIB}"; wait_for_ready {url}/ric-ready {url}/failed 20 1'],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    time.sleep(2)
    (directory / "ric-ready").touch()
    assert waiter.wait(timeout=30) == 0


def test_core_network_starts_before_the_ric_build():
    text = (ROOT / "bin" / "core-setup.sh").read_text()
    compose_up = text.index("docker-compose-core.yaml up -d")
    build = text.index("cmake -DKPM_VERSION")
    assert compose_up < build


def test_core_publishes_readiness_only_after_the_ric_is_up():
    text = (ROOT / "bin" / "core-setup.sh").read_text()
    assert text.index('touch "${READY_DIR}/cn-ready"') < text.index("nearRT-RIC > /local/logs")
    assert text.index("nearRT-RIC > /local/logs") < text.index('touch "${READY_DIR}/ric-ready"')


def test_cell_does_not_continue_when_core_is_not_ready():
    text = (ROOT / "bin" / "cell-setup.sh").read_text()
    block = text[text.index("wait_for_ready"):text.index("docker pull")]
    assert "fail " in block
    assert "WARNING" not in block
    assert "break" not in block


@pytest.mark.parametrize("script", ["core-setup.sh", "cell-setup.sh"])
def test_setup_scripts_record_an_outcome(script):
    text = (ROOT / "bin" / script).read_text()
    assert re.search(r"_FAILED", text)
    assert re.search(r"_READY", text)
