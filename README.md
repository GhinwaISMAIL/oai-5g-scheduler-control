# oai-5g-scheduler-control

Single-cell OAI 5G SA testbed on POWDER with a controllable downlink MAC scheduler.
The scheduling policy and the sharing of resource blocks (PRBs) between UEs and slices are
changed at run time, from the cell node or from an xApp over E2, and read back from the gNB.
The RAN is simulated at the PHY layer (RFsim); the core network, control plane and user plane
are real.

Instantiating the POWDER profile brings up the whole system: core network, near-RT RIC, one
cell node with its gNB and UEs.

## Architecture

```
core (10.10.1.1)                     cell1 (10.10.1.11)
  mysql udr udm ausf                   gNB (controllable scheduler, E2 agent)
  amf smf upf ext-dn                     | RFsim on this host
  near-RT RIC (host process)             +-- UE 1 ... UE n
  xapp_sched_ctrl
        |  N2 / N3 / E2 over the experimental LAN  |
```

The gNB and its UEs stay on the same node, so the RFsim IQ path is local. Only control and user
plane cross the LAN.

## Components

| component | contents |
|-----------|----------|
| `ghinwa555/oai-gnb-ctrl:<tag>` | OAI gNB with the controllable downlink scheduler (`gnb/src`, `gnb/patches`) and the E2 agent |
| `ghinwa555/oai-nr-ue-chan` | OAI UE with RFsim and channel model |
| near-RT RIC | FlexRIC, built from OAI's submodule on the core node at boot, run as a host process |
| `xapp_sched_ctrl` | xApp that sends scheduler commands to the gNB over E2 (MAC service model) |

The setup pins OAI `70508ebaf52f2aae420566d380c6537f2efb9f0c` and FlexRIC
`ef6d722f22191eea74089966983da1f5ec1fedd4`. The gNB image tag ends with a hash of `gnb/src` and
`gnb/patches`; if the image cannot be pulled, the cell node builds it (about 25 minutes).
`BUILD.md` describes the images and what is not recorded about them.

## Instantiating

Create a POWDER profile from this repository (`profile.py`) with these parameters:

| parameter | default | meaning |
|-----------|---------|---------|
| `ues_per_cell` | 2 | UEs on the cell |
| `core_type`, `cell_type` | d430, d740 | hardware of the core and the cell node |
| `channel_type`, `channel_initial_mode` | AWGN, uniform | RFsim channel model |
| `controllable_scheduler` | true | use the controllable gNB image |
| `gnb_ctrl_image` | pinned tag | image to pull; empty builds it on the cell node |
| `second_slice` | false | add S-NSSAI SST 2 to the gNB, core and subscribers; half of the UEs attach with it (needs 2 or more UEs) |

Progress is in `/local/logs/setup.log` on every node, the RIC output in
`/local/logs/nearRT-RIC.log` on the core. The cell node writes `/local/logs/CELL_READY` when the
gNB and the UEs are up. Bring-up order: **core, RIC, gNB, UEs, traffic, xApp**. Details and
re-running setup on a live reservation: [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md).

## Verifying the deployment

On the core:

```bash
grep "E2 SETUP-REQUEST rx" /local/logs/nearRT-RIC.log        # the gNB registered over E2
```

On the cell node:

```bash
cat /local/logs/GNB_IMAGE /local/logs/CELL_READY               # image in use, cell ready
sudo docker exec ric5g-ue-cell1-1 ip -4 addr show oaitun_ue1  # a UE has its tunnel address
cd /local/repository && python3 bin/sched-ctl.py status        # the scheduler answers: policy, one row per UE
```

## Controlling the scheduler

Policies:

| policy | behaviour |
|--------|-----------|
| `pf` | stock proportional fair (default after start and after `reset`) |
| `rr` | round robin in the time domain |
| `wrr` | weighted round robin: resource-block shares follow the UE weights |
| `wshare` | weighted share of the free block among the UEs with data |
| `slice` | weighted round robin between slices, then between the UEs of a slice |

On the cell node:

```bash
cd /local/repository
python3 bin/sched-ctl.py policy wrr                 # pf | rr | wrr | wshare | slice
python3 bin/sched-ctl.py weight <rnti> 3000         # RNTI in hex, as printed by status
python3 bin/sched-ctl.py cap <rnti> 30              # most resource blocks per slot, 0 = none
python3 bin/sched-ctl.py slice 1 weight 3000        # slice share (slices 0..7, 0 is the default)
python3 bin/sched-ctl.py slice 1 cap 20             # most resource blocks the slice takes per slot
python3 bin/sched-ctl.py assoc <rnti> 1             # put a UE in slice 1
python3 bin/sched-ctl.py slice-del 1                # slice 1 back to its defaults, its members fall back to slice 0
python3 bin/sched-ctl.py nssai-map 1 none 2         # UEs with this S-NSSAI go to slice 2
python3 bin/sched-ctl.py map --containers ric5g-ue-cell1-1,ric5g-ue-cell1-2   # RNTI of each UE container
python3 bin/sched-ctl.py reset                      # back to the stock policy
```

The same commands go over E2 from the core node:

```bash
/opt/oai-src/openair2/E2AP/flexric/build/examples/xApp/c/ctrl/xapp_sched_ctrl policy wrr
```

The standard FlexRIC slice message goes the same way (`sm-add`, `sm-del`, `sm-assoc`, `sm-get`, see
`docs/SCHEDULER_CONTROL.md`).

Every command is read back from the gNB and fails if the value differs. An E2 acknowledgement only
says that the gNB received the message; check the result with `sched-ctl.py status`. Full
description: [docs/SCHEDULER_CONTROL.md](docs/SCHEDULER_CONTROL.md), design of the slice
policy: [docs/SLICING_DESIGN.md](docs/SLICING_DESIGN.md).

## Measuring

```bash
python3 bin/sched-ctl.py measure --seconds 20
```

reports the resource blocks the MAC scheduled per UE and per slice in the window, with the share in
percent and the throughput. This is the evidence for any allocation claim; judge by the resource-block
shares, because the throughput of RFsim varies by about 10 %. Monitoring xApps (KPM, MAC, RLC, PDCP,
GTP telemetry) are described in [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md).

## Results

The policies and the slicing were run on a live gNB with four UEs (RFsim, AWGN). The measured shares
matched the configured targets for slice weights, UE weights, the slice cap, membership by RNTI and by
S-NSSAI, the bounded credit of slices and UEs, and three slices at once. One measurement window after a
rule deletion deviated from its target and is not explained. The procedure and the numbers are in
[docs/SLICING_LIVE_TEST.md](docs/SLICING_LIVE_TEST.md).

## Operational constraints

- **Restarting the RIC orphans every gNB.** Restart the gNB after the RIC.
- **Restarting a gNB drops all of its UEs** and resets the scheduler to `pf`. RNTIs belong to one
  attachment: read them again with `map` and set weights, caps and slice associations again.
- **Stop an xApp with `Ctrl-C`, never `SIGKILL`.** Do not start an xApp before the gNB has completed E2 setup.
- **Run nothing else on the cell node while the gNB image builds.** `cell-setup.sh` restarts Docker,
  which cancels the build.

## Limits

One cell, up to the UEs a single `nr-softmodem` serves. A weight is a share of the resource blocks
among the UEs that have data in the same slot, not a guaranteed throughput. Uplink scheduling is not
controlled. RFsim does not provide physical RF coverage.
Slices: 8 slices, one UE per slot, membership by RNTI association, then by the S-NSSAI the MAC
reports (SST 1 without SD for all UEs by default). Two different S-NSSAIs are available with `second_slice` (run on a reservation with two and with four UEs, see `docs/SLICING_LIVE_TEST.md`).
The standard FlexRIC slice message (ADD, DEL, UE association, indication) is translated to the same commands (`docs/SCHEDULER_CONTROL.md`); run on a reservation with the stock slice xApp (DEL, association and indication); a valid ADD is host-tested only.
Not tested on the cell: a UE that attaches with a new RNTI during a run.

## Layout

```
profile.py                 POWDER profile: 1 core node + 1 cell node
bin/node-setup.sh          setup dispatcher (core | cell); core-setup.sh and cell-setup.sh do the work
bin/sched-ctl.py           scheduler control and measurement on the cell node
bin/gen-second-slice.py    core configuration with a second S-NSSAI (profile parameter second_slice)
gnb/src, gnb/patches       controllable scheduler and OAI patches; build-gnb-image.sh builds the image
gnb/test                   native tests of the scheduler
xapp/xapp_sched_ctrl.c     xApp that sends the scheduler commands and the standard slice message over E2
xapp/test                  stand-ins for the FlexRIC xApp interface, used by the xApp tests
etc/                       gNB and UE templates, core configuration, pinned versions
BUILD.md                   image provenance
VALIDATION.md              traffic (MGEN) helpers and receiver validation
docs/                      DEPLOYMENT, SCHEDULER_CONTROL, SLICING_DESIGN,
                           SLICING_LIVE_TEST
tests/                     host tests
```

## Tests

```bash
python3 -m pip install -r tests/requirements.txt
python3 -m pytest -q tests
make -C gnb/test check          # native tests of the scheduler
```
