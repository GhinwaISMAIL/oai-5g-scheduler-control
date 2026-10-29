import runpy
import subprocess
import sys
from pathlib import Path
from types import ModuleType, SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]


def render_profile(monkeypatch, ues=None):
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
    portal.ParameterType = SimpleNamespace(INTEGER='integer', STRING='string', NODETYPE='node')
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
