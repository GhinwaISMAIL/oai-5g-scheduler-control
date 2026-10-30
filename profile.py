#!/usr/bin/env python3
"""
POWDER profile: oai-5g-scheduler-control
Single-cell OAI 5G SA RFsim with FlexRIC near-RT RIC and two competing UEs.

Topology:
  1 x core node  : CN NFs + FlexRIC nearRT-RIC                   (default d430)
  1 x cell node  : one E2-enabled, chanmod-capable gNB + K UEs   (default d740)

Shared experimental LAN 10.10.1.0/26: core = .1, cell-k = .(10+k)

INVARIANT: each cell is a colocated island -- its gNB and its UEs run on the
SAME node, so the RFsim IQ path stays local and the channel model behaves as in
the validated single-node baseline. Only NGAP/N3/E2 cross the LAN.
"""

import re

import geni.portal as portal
import geni.rspec.pg as rspec
import geni.rspec.igext as IG

NUM_CELLS = 1

pc = portal.context

pc.defineParameter("ues_per_cell", "UEs per cell",
                   portal.ParameterType.INTEGER, 2)
pc.defineParameter("core_type", "Core node hardware type",
                   portal.ParameterType.NODETYPE, "d430")
pc.defineParameter("cell_type", "Cell node hardware type",
                   portal.ParameterType.NODETYPE, "d740")
pc.defineParameter("channel_type", "RFsim channel family",
                   portal.ParameterType.STRING, "AWGN")
pc.defineParameter("channel_initial_mode", "Initial per-UE channel layout",
                   portal.ParameterType.STRING, "uniform")

pc.defineParameter("controllable_scheduler",
                   "Controllable downlink scheduler in the gNB (per-UE PRB shares and policy, changed at run time)",
                   portal.ParameterType.BOOLEAN, True)
pc.defineParameter("gnb_ctrl_image",
                   "Pre-built controllable gNB image to pull instead of building it on the cell node "
                   "(for example ghinwa555/oai-gnb-ctrl:TAG; empty = build, about 25 minutes)",
                   portal.ParameterType.STRING, "")

params = pc.bindParameters()

if params.ues_per_cell < 1:
    pc.reportError(portal.ParameterError("ues_per_cell must be >= 1", ["ues_per_cell"]))
if params.channel_type not in ("AWGN", "TDL_A", "TDL_B", "TDL_C", "EPA", "EVA", "ETU"):
    pc.reportError(portal.ParameterError(
        "channel_type must be AWGN, TDL_A, TDL_B, TDL_C, EPA, EVA, or ETU",
        ["channel_type"]))
if params.channel_initial_mode not in ("uniform", "gradient"):
    pc.reportError(portal.ParameterError(
        "channel_initial_mode must be uniform or gradient",
        ["channel_initial_mode"]))
if params.gnb_ctrl_image and not re.match(r"^[A-Za-z0-9][A-Za-z0-9._/:@-]*$", params.gnb_ctrl_image):
    pc.reportError(portal.ParameterError(
        "gnb_ctrl_image must be an image reference such as name/repo:tag",
        ["gnb_ctrl_image"]))
if params.gnb_ctrl_image and not params.controllable_scheduler:
    pc.reportError(portal.ParameterError(
        "gnb_ctrl_image needs controllable_scheduler to be enabled",
        ["gnb_ctrl_image"]))
pc.verifyParameters()

request = pc.makeRequestRSpec()

IMAGE = "urn:publicid:IDN+emulab.net+image+emulab-ops:UBUNTU22-64-STD"
MASK = "255.255.255.192"
CORE_LAN_IP = "10.10.1.1"

lan = request.LAN("ran-lan")

def add_iface(node, ip):
    iface = node.addInterface("if0")
    iface.addAddress(rspec.IPv4Address(ip, MASK))
    lan.addInterface(iface)

def setup(node, role, index):
    env = ""
    if role == "cell" and params.controllable_scheduler:
        env = "GNB_CTRL=1 "
        if params.gnb_ctrl_image:
            env += "GNB_CTRL_IMAGE=%s " % params.gnb_ctrl_image
    node.addService(rspec.Execute(
        shell="bash",
        command=("sudo mkdir -p /local/logs && "
                 "sudo %sbash /local/repository/bin/node-setup.sh %s %d %d %d %s %s "
                 ">> /local/logs/setup.log 2>&1"
                 % (env, role, index, NUM_CELLS, params.ues_per_cell,
                    params.channel_initial_mode, params.channel_type))))

core = request.RawPC("core")
core.hardware_type = params.core_type
core.disk_image = IMAGE
add_iface(core, CORE_LAN_IP)
setup(core, "core", 0)

for k in range(1, NUM_CELLS + 1):
    cell = request.RawPC("cell%d" % k)
    cell.hardware_type = params.cell_type
    cell.disk_image = IMAGE
    add_iface(cell, "10.10.1.%d" % (10 + k))
    setup(cell, "cell", k)

tour = IG.Tour()
tour.Description(IG.Tour.TEXT,
    "Single-cell OAI 5G SA RFsim with FlexRIC near-RT RIC and channel "
    "modelling. One core node runs the CN + RIC; each cell node runs one "
    "E2-enabled, chanmod-capable gNB with its UEs colocated. Start xApps by hand "
    "after E2 registration and UE attachment. This profile provides the baseline "
    "network and telemetry. The gNB carries a controllable downlink scheduler "
    "(per-UE PRB shares and policy, changed at run time on the cell node with "
    "bin/sched-ctl.py; see docs/SCHEDULER_CONTROL.md); it is built on the cell node "
    "at boot unless a pre-built image is given. Changes are made on the cell node, "
    "not over E2: the E2 control path is not implemented yet. "
    "PLMN 208/99, TAC=1, SST=1, DNN=oai.")
tour.Instructions(IG.Tour.TEXT,
    "Bring-up is staged: the core node brings up the core network and then builds "
    "and starts the RIC. The cell node waits for the core to report ready (up to "
    "60 min) before starting its gNB. Check /local/logs/setup.log on each node. "
    "Outcome markers: CORE_READY / CORE_FAILED on the core, CELL_READY / "
    "CELL_FAILED on the cell. The first boot builds the controllable gNB image on "
    "the cell node while the core is brought up (log: /local/logs/gnb-build.log); "
    "the image tag is written to /local/logs/GNB_IMAGE. To re-run after a repository update, see "
    "docs/DEPLOYMENT.md.")
request.addTour(tour)

pc.printRequestRSpec()
