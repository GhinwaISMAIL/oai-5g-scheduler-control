import runpy
import subprocess
import sys
from pathlib import Path
from types import ModuleType, SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]


def render_profile(monkeypatch, ues=None, overrides=None):
    defaults = {}
    errors = []
    nodes = []
    class Node:
        def __init__(self, name):
            self.name = name
            self.services = []
        def addInterface(self, name):
            return SimpleNamespace(addAddress=lambda address: setattr(self, 'address', address))
        def addService(self, service):
            self.services.append(service)
    class Request:
        def RawPC(self, name):
            node = Node(name)
            nodes.append(node)
            return node
        def LAN(self, name):
            return SimpleNamespace(addInterface=lambda interface: None)
        def addTour(self, tour):
            pass
    class Context:
        def defineParameter(self, name, label, kind, value):
            defaults[name] = value
        def bindParameters(self):
            values = dict(defaults)
            if ues is not None:
                values['ues_per_cell'] = ues
            values.update(overrides or {})
            return SimpleNamespace(**values)
        def reportError(self, error):
            errors.append(error)
        def verifyParameters(self):
            if errors:
                raise ValueError('invalid profile parameters')
        def makeRequestRSpec(self):
            return Request()
        def printRequestRSpec(self):
            pass
    class Tour:
        TEXT = 'text'
        def Description(self, *args):
            pass
        def Instructions(self, *args):
            pass
    geni, portal, rspec, pg, ig = [ModuleType(name) for name in ('geni','geni.portal','geni.rspec','geni.rspec.pg','geni.rspec.igext')]
    geni.portal = portal
    geni.rspec = rspec
    rspec.pg, rspec.igext = pg, ig
    portal.context = Context()
    portal.ParameterType = SimpleNamespace(INTEGER='integer', STRING='string', NODETYPE='node', BOOLEAN='boolean')
    portal.ParameterError = lambda *args: args
    pg.IPv4Address = lambda address, mask: (address, mask)
    pg.Execute = lambda **kwargs: kwargs
    ig.Tour = Tour
    for mod in (geni,portal,rspec,pg,ig):
        monkeypatch.setitem(sys.modules,mod.__name__,mod)
    runpy.run_path(str(ROOT / 'profile.py'))
    return nodes, defaults


def test_default_reservation_has_one_core_one_cell_two_ues(monkeypatch):
    nodes, defaults = render_profile(monkeypatch)
    assert 'num_cells' not in defaults
    assert defaults['ues_per_cell'] == 2
    assert [node.name for node in nodes] == ['core', 'cell1']
    assert [node.address[0] for node in nodes] == ['10.10.1.1','10.10.1.11']
    assert 'node-setup.sh core 0 1 2 ' in nodes[0].services[0]['command']
    assert 'node-setup.sh cell 1 1 2 ' in nodes[1].services[0]['command']


def test_controllable_scheduler_is_requested_on_the_cell_only_by_default(monkeypatch):
    nodes, defaults = render_profile(monkeypatch)
    assert defaults['controllable_scheduler'] is True and defaults['gnb_ctrl_image'] == DEFAULT_GNB_IMAGE
    assert 'GNB_CTRL' not in nodes[0].services[0]['command']
    assert 'sudo GNB_CTRL=1 GNB_CTRL_IMAGE=%s bash /local/repository/bin/node-setup.sh cell 1 1 2 ' % DEFAULT_GNB_IMAGE in nodes[1].services[0]['command']
    assert 'sudo GNB_CTRL=1 GNB_CTRL_IMAGE=%s bash ' % DEFAULT_GNB_IMAGE in nodes[1].services[0]['command']


def test_prebuilt_image_and_opt_out(monkeypatch):
    nodes, _ = render_profile(monkeypatch, overrides={'gnb_ctrl_image': 'ghinwa555/oai-gnb-ctrl:abc-123'})
    assert 'sudo GNB_CTRL=1 GNB_CTRL_IMAGE=ghinwa555/oai-gnb-ctrl:abc-123 bash ' in nodes[1].services[0]['command']
    nodes, _ = render_profile(monkeypatch, overrides={'controllable_scheduler': False})
    assert 'GNB_CTRL' not in nodes[1].services[0]['command']
    nodes, _ = render_profile(monkeypatch, overrides={'gnb_ctrl_image': ''})
    assert 'sudo GNB_CTRL=1 bash ' in nodes[1].services[0]['command']
    assert 'GNB_CTRL_IMAGE' not in nodes[1].services[0]['command']


@pytest.mark.parametrize('overrides', [
    {'gnb_ctrl_image': 'x; reboot'},
    {'gnb_ctrl_image': 'a b'},
])
def test_profile_rejects_unsafe_or_inconsistent_image(monkeypatch, overrides):
    with pytest.raises(ValueError):
        render_profile(monkeypatch, overrides=overrides)


def test_profile_rejects_zero_ues_before_allocation(monkeypatch):
    with pytest.raises(ValueError):
        render_profile(monkeypatch,ues=0)


@pytest.mark.parametrize('script,args',[
    ('node-setup.sh',['cell','2','1','2']),
    ('node-setup.sh',['core','0','2','2']),
    ('cell-setup.sh',['2','1','2']),
    ('cell-setup.sh',['1','2','2']),
    ('core-setup.sh',['10.10.1.1','2','2']),
])
def test_setup_rejects_extra_cells_before_mutation(script,args):
    result=subprocess.run(['bash',str(ROOT/'bin'/script),*args],capture_output=True,text=True)
    assert result.returncode == 2
    assert 'exactly one cell' in result.stderr


def test_cell_setup_builds_the_image_while_waiting_for_the_core():
    text = (ROOT / 'bin' / 'cell-setup.sh').read_text()
    start = text.index('build-gnb-image.sh" > "${GNB_BUILD_OUT}"')
    wait_core = text.index('wait_for_ready "${READY_URL}/ric-ready"')
    collect = text.index('wait "${GNB_BUILD_PID}"')
    assert start < wait_core < collect


def test_image_tag_is_passed_through_a_file_not_the_tee_output():
    cell = (ROOT / 'bin' / 'cell-setup.sh').read_text()
    build = (ROOT / 'gnb' / 'build-gnb-image.sh').read_text()
    assert 'cat /local/logs/gnb-build.tag' in cell
    assert build.count('publish_tag') >= 3  # defined, early exit, normal exit


def test_flexric_api_export_collects_control_interface_files(tmp_path):
    src = tmp_path / 'oai'
    files = [
        'openair2/E2AP/flexric/src/sm/mac_sm/ie/mac_data_ie.h',
        'openair2/E2AP/flexric/examples/xApp/c/ctrl/xapp_mac_ctrl.c',
        'openair2/E2AP/RAN_FUNCTION/CUSTOMIZED/ran_func_mac.c',
    ]
    for name in files:
        path = src / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('x\n')
    out = tmp_path / 'api.tar.gz'
    result = subprocess.run(['bash', str(ROOT / 'bin' / 'export-flexric-api.sh'), str(src), str(out)],
                            capture_output=True, text=True)
    assert result.returncode == 0
    listing = subprocess.run(['tar', 'tzf', str(out)], capture_output=True, text=True).stdout.split()
    assert sorted(listing) == sorted(files)
    missing = subprocess.run(['bash', str(ROOT / 'bin' / 'export-flexric-api.sh'), str(tmp_path / 'none'), str(tmp_path / 'b.tar.gz')],
                             capture_output=True, text=True)
    assert missing.returncode == 0 and not (tmp_path / 'b.tar.gz').exists()


DEFAULT_GNB_IMAGE = 'ghinwa555/oai-gnb-ctrl:70508eba-3cc4a7aa00f0'


def test_default_gnb_image_matches_the_sources_in_this_checkout():
    """The default image must have been built from the current gnb/src and gnb/patches.

    The tag is <oai commit, 8 characters>-<12 characters of the hash the build script
    computes over gnb/src and gnb/patches>.  When this fails, build and push a new image
    (docs/DEPLOYMENT.md) and put its tag in profile.py and here.
    """
    import hashlib
    import json
    gnb = ROOT / 'gnb'
    data = b''.join(path.read_bytes() for sub in ('src', 'patches') for path in sorted((gnb / sub).glob('*')))
    oai = json.loads((ROOT / 'etc' / 'stack-versions.json').read_text())['oai_revision']
    expected = 'ghinwa555/oai-gnb-ctrl:%s-%s' % (oai[:8], hashlib.sha256(data).hexdigest()[:12])
    assert DEFAULT_GNB_IMAGE == expected


def test_cell_setup_falls_back_to_a_build_and_warns_about_stale_images():
    text = (ROOT / 'bin' / 'cell-setup.sh').read_text()
    assert subprocess.run(['bash', '-n', str(ROOT / 'bin' / 'cell-setup.sh')]).returncode == 0
    assert 'building the image on this node instead' in text
    assert 'check_image_sources "${GNB_CTRL_IMAGE}"' in text


def _image_check(tmp_path, tag):
    text = (ROOT / 'bin' / 'cell-setup.sh').read_text()
    start = text.index('check_image_sources() {')
    function = text[start:text.index('\n}\n', start) + 3]
    repo = tmp_path / 'repo'
    (repo / 'gnb' / 'src').mkdir(parents=True, exist_ok=True)
    (repo / 'gnb' / 'patches').mkdir(exist_ok=True)
    (repo / 'gnb' / 'src' / 'a.c').write_text('int a;\n')
    (repo / 'gnb' / 'patches' / 'p.patch').write_text('patch\n')
    import hashlib
    good = hashlib.sha256(b'int a;\npatch\n').hexdigest()[:12]
    script = 'REPO_DIR=%s CELL_IDX=1\n%s\ncheck_image_sources "$1"\n' % (repo, function)
    run = subprocess.run(['bash', '-c', script, 'x', tag.replace('GOOD', good)], capture_output=True, text=True)
    return run


def test_stale_image_check_warns_only_on_a_different_hash(tmp_path):
    ok = _image_check(tmp_path, 'u/r:70508eba-GOOD')
    assert ok.returncode == 0 and ok.stderr == ''
    stale = _image_check(tmp_path, 'u/r:70508eba-000000000000')
    assert stale.returncode == 0 and 'different gNB sources' in stale.stderr
    other = _image_check(tmp_path, 'u/r:latest')
    assert other.returncode == 0 and other.stderr == ''
