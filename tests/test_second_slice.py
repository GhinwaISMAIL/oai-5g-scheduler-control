"""Second S-NSSAI (SST 2): profile parameter, subscribers, core and cell configuration."""
import hashlib
import re
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

from test_single_cell_profile import render_profile

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'bin'
ETC = ROOT / 'etc'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def bash(script, env=None):
    import os
    full = dict(os.environ)
    full.pop('SECOND_SLICE', None)
    full.update(env or {})
    return subprocess.run(['bash', '-c', script], capture_output=True, text=True, env=full)


# --------------------------------------------------------------- profile

def test_default_profile_does_not_mention_the_second_slice(monkeypatch):
    nodes, defaults = render_profile(monkeypatch)
    assert defaults['second_slice'] is False
    for node in nodes:
        assert 'SECOND_SLICE' not in node.services[0]['command']
    assert nodes[0].services[0]['command'].startswith('sudo mkdir -p /local/logs && sudo bash /local/repository/bin/node-setup.sh core 0 1 2 ')


def test_second_slice_reaches_both_nodes(monkeypatch):
    nodes, _ = render_profile(monkeypatch, overrides={'second_slice': True})
    assert 'sudo SECOND_SLICE=1 bash /local/repository/bin/node-setup.sh core 0 1 2 ' in nodes[0].services[0]['command']
    assert 'sudo SECOND_SLICE=1 GNB_CTRL=1 GNB_CTRL_IMAGE=' in nodes[1].services[0]['command']
    assert ' bash /local/repository/bin/node-setup.sh cell 1 1 2 ' in nodes[1].services[0]['command']


def test_second_slice_needs_two_ues(monkeypatch):
    with pytest.raises(ValueError):
        render_profile(monkeypatch, ues=1, overrides={'second_slice': True})
    render_profile(monkeypatch, ues=2, overrides={'second_slice': True})


# ----------------------------------------------------------- subscribers

def subscriber_ssts(tmp_path, n, second):
    sql = tmp_path / 'db.sql'
    shutil.copy(ETC / 'oai_db.sql', sql)
    env = {'SECOND_SLICE': '1'} if second else {}
    result = bash('bash %s 1 %d %s %s' % (BIN / 'gen-subscribers.sh', n, sql, sql), env)
    assert result.returncode == 0, result.stderr
    text = sql.read_text()
    block = text[text.index('-- BEGIN GENERATED SUBSCRIBERS'):]
    rows = re.findall(r"^\('(\d+)', '20899', '" + re.escape('{\\"sst\\": ') + r"(\d)", block, re.M)
    return block, [int(sst) for _, sst in rows]


@pytest.mark.parametrize('n', [1, 2, 3, 4, 5])
def test_default_subscribers_are_all_sst_1(tmp_path, n):
    block, ssts = subscriber_ssts(tmp_path, n, second=False)
    assert ssts == [1] * n
    assert '-- PLMN 208/99, SST=1 SD=0xffffff, DNN=oai, dynamic PDU addressing' in block


@pytest.mark.parametrize('n,expected', [(1, [1]), (2, [1, 2]), (3, [1, 1, 2]), (4, [1, 1, 2, 2]), (5, [1, 1, 1, 2, 2])])
def test_second_slice_splits_the_subscribers(tmp_path, n, expected):
    block, ssts = subscriber_ssts(tmp_path, n, second=True)
    assert ssts == expected
    assert 'SST=1 and SST=2' in block


# ---------------------------------------------------------- core config

def generate(tmp_path):
    etc = tmp_path / 'etc'
    etc.mkdir()
    for name in ('mini_nonrf_config.yaml', 'docker-compose-core.yaml'):
        shutil.copy(ETC / name, etc / name)
    result = subprocess.run(['python3', str(BIN / 'gen-second-slice.py'), str(etc)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return etc


def test_core_configuration_serves_both_slices(tmp_path):
    etc = generate(tmp_path)
    cfg = yaml.safe_load((etc / 'mini_nonrf_config.second.yaml').read_text())
    both = [{'sst': 1}, {'sst': 2}]
    assert cfg['snssais'] == both
    assert cfg['amf']['plmn_support_list'][0]['nssai'] == both
    assert [x['sNssai'] for x in cfg['smf']['smf_info']['sNssaiSmfInfoList']] == both
    assert [x['single_nssai'] for x in cfg['smf']['local_subscription_infos']] == both
    assert [x['sNssai'] for x in cfg['upf']['upf_info']['sNssaiUpfInfoList']] == both
    for entry in cfg['smf']['smf_info']['sNssaiSmfInfoList'] + cfg['upf']['upf_info']['sNssaiUpfInfoList']:
        assert [d['dnn'] for d in entry.get('dnnSmfInfoList', entry.get('dnnUpfInfoList'))] == ['oai']
    first, second = cfg['smf']['local_subscription_infos']
    assert {k: v for k, v in first.items() if k != 'single_nssai'} == {k: v for k, v in second.items() if k != 'single_nssai'}


def test_everything_but_the_second_slice_is_unchanged(tmp_path):
    etc = generate(tmp_path)
    original = yaml.safe_load((ETC / 'mini_nonrf_config.yaml').read_text())
    patched = yaml.safe_load((etc / 'mini_nonrf_config.second.yaml').read_text())
    original['snssais'] = patched['snssais'] = None
    original['amf']['plmn_support_list'][0]['nssai'] = patched['amf']['plmn_support_list'][0]['nssai'] = None
    original['smf']['smf_info'] = patched['smf']['smf_info'] = None
    original['smf']['local_subscription_infos'] = patched['smf']['local_subscription_infos'] = None
    original['upf']['upf_info'] = patched['upf']['upf_info'] = None
    assert original == patched


def test_compose_copy_only_changes_the_configuration_file_name(tmp_path):
    etc = generate(tmp_path)
    original = (ETC / 'docker-compose-core.yaml').read_text()
    patched = (etc / 'docker-compose-core.second.yaml').read_text()
    assert patched.count('./mini_nonrf_config.second.yaml:') == 6
    assert patched.replace('./mini_nonrf_config.second.yaml:', './mini_nonrf_config.yaml:') == original


@pytest.mark.skipif(shutil.which('docker') is None, reason='docker CLI not installed')
def test_generated_compose_file_is_valid(tmp_path):
    etc = generate(tmp_path)
    result = subprocess.run(['docker', 'compose', '-f', str(etc / 'docker-compose-core.second.yaml'), 'config', '-q'],
                            capture_output=True, text=True, cwd=etc)
    assert result.returncode == 0, result.stderr


def test_generation_leaves_the_sources_alone_and_is_repeatable(tmp_path):
    before = {n: digest(ETC / n) for n in ('mini_nonrf_config.yaml', 'docker-compose-core.yaml')}
    etc = generate(tmp_path)
    first = digest(etc / 'mini_nonrf_config.second.yaml')
    subprocess.run(['python3', str(BIN / 'gen-second-slice.py'), str(etc)], check=True, capture_output=True)
    assert digest(etc / 'mini_nonrf_config.second.yaml') == first
    assert {n: digest(ETC / n) for n in before} == before
    assert {n: digest(etc / n) for n in before} == before


def test_generation_refuses_a_configuration_that_changed_shape(tmp_path):
    etc = generate(tmp_path)
    cfg = etc / 'mini_nonrf_config.yaml'
    cfg.write_text(cfg.read_text().replace('dnnUpfInfoList', 'dnnUpfInfoListX'))
    result = subprocess.run(['python3', str(BIN / 'gen-second-slice.py'), str(etc)], capture_output=True, text=True)
    assert result.returncode != 0 and 'anchor' in result.stderr


# ------------------------------------------------------------ cell side

LIB = str(BIN / 'slice-lib.sh')


def test_ue_sst_follows_the_split():
    off = bash('. %s; for u in 1 2 3 4; do ue_sst $u 4; done' % LIB)
    assert off.stdout.split() == ['1', '1', '1', '1']
    on = bash('. %s; for u in 1 2 3 4; do ue_sst $u 4; done' % LIB, {'SECOND_SLICE': '1'})
    assert on.stdout.split() == ['1', '1', '2', '2']
    odd = bash('. %s; for u in 1 2 3; do ue_sst $u 3; done' % LIB, {'SECOND_SLICE': '1'})
    assert odd.stdout.split() == ['1', '1', '2']
    off_value = bash('. %s; for u in 1 2; do ue_sst $u 2; done' % LIB, {'SECOND_SLICE': '0'})
    assert off_value.stdout.split() == ['1', '1']


def test_gnb_and_ue_configuration_for_the_second_slice(tmp_path):
    gnb = tmp_path / 'gnb.conf'
    gnb.write_text((ETC / 'gnb-cell.conf.tmpl').read_text())
    assert bash('. %s; gnb_add_second_slice %s' % (LIB, gnb)).returncode == 0
    assert 'snssaiList = ({ sst = 1, sd = 0xffffff }, { sst = 2, sd = 0xffffff })' in gnb.read_text()
    assert gnb.read_text().count('sst = 1') == 1
    assert bash('. %s; gnb_add_second_slice %s' % (LIB, gnb)).returncode != 0   # not twice

    ue = tmp_path / 'ue.conf'
    ue.write_text((ETC / 'nr-ue.conf.tmpl').read_text())
    assert bash('. %s; ue_set_sst %s 2' % (LIB, ue)).returncode == 0
    assert 'nssai_sst=2;' in ue.read_text() and 'nssai_sst=1;' not in ue.read_text()
    assert bash('. %s; ue_set_sst %s 2' % (LIB, ue)).returncode != 0


def test_setup_scripts_use_the_second_slice_only_when_asked():
    core = (BIN / 'core-setup.sh').read_text()
    assert 'SECOND_SLICE:-0}" = 1 ]' in core
    assert core.index('gen-second-slice.py') < core.index('docker-compose-core.second.yaml up -d')
    assert 'docker compose -f docker-compose-core.yaml up -d' in core      # the single-slice path is unchanged
    cell = (BIN / 'cell-setup.sh').read_text()
    assert '. "${REPO_DIR}/bin/slice-lib.sh"' in cell
    assert 'gnb_add_second_slice' in cell and 'ue_set_sst' in cell
