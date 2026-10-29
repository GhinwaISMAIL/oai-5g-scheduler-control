# First single-cell reservation

Use a fresh two-host POWDER reservation from this repository's `profile.py`.
The defaults are one gNB and two OAI UE processes. No hardware was reserved or
software deployed while preparing this repository.

## What setup includes

- Core host: MySQL, UDR, UDM, AUSF, AMF, SMF, UPF and an external data-network
  container, plus a native FlexRIC RIC built from the pinned submodule.
- Cell host: one E2-enabled gNB and two UEs, with RFsim IQ confined to that host.
- UE authentication and PDU-session subscriber rows generated before core startup.
- Routing for N2/N3 and real UE application traffic through the UE tunnel and UPF.
- Native monitoring xApp binaries built on core, started manually after E2 setup.

Inspect `/local/logs/setup.log` on both hosts. The inherited setup uses package
installation, registry pulls and source compilation; local tests do not exercise
that full deployment. Keep the setup logs if any stage fails.

## Outcome markers

Each setup script ends with a marker file, because the portal's startup status
only says that the script exited:

| Node | Success | Failure |
| --- | --- | --- |
| core | `/local/logs/CORE_READY` | `/local/logs/CORE_FAILED` (contains the reason) |
| cell1 | `/local/logs/CELL_READY` | `/local/logs/CELL_FAILED` (contains the reason) |

The core serves `cn-ready`, `ric-ready` and `failed` from `/local/ready` on
`10.10.1.1:8099` (LAN only). The cell waits for `ric-ready` for up to
`READY_TIMEOUT` seconds (default 3600) and does not start the gNB without it.

## Re-running setup on a live reservation

After pushing a change to the repository, the existing reservation can be
updated without a new one. Core first, then the cell, because restarting the RIC
drops the gNB's E2 association and the UE sessions.

On core (working directory `/local/repository`):

```bash
cd /local/repository && sudo git pull --ff-only
sudo bash bin/node-setup.sh core 0 1 2 uniform AWGN >> /local/logs/setup.log 2>&1
ls /local/logs/CORE_READY /local/logs/CORE_FAILED 2>/dev/null
```

The core network and RIC are restarted. FlexRIC is rebuilt only if the pinned
commits or `bin/patch-flexric-*.py` changed (stamp: `/opt/oai-src/.ric-build-stamp`).
Use the same channel arguments as the reservation (`channel_initial_mode`,
`channel_type`); the defaults above match the profile defaults.

On cell1, once `CORE_READY` exists:

```bash
cd /local/repository && sudo git pull --ff-only
sudo bash bin/node-setup.sh cell 1 1 2 uniform AWGN >> /local/logs/setup.log 2>&1
ls /local/logs/CELL_READY /local/logs/CELL_FAILED 2>/dev/null
```

The previous cell containers are removed and recreated. This does not pick up
changes to `profile.py` itself (node types, parameters); those need a new
reservation.

## Readiness before traffic

On core:

```bash
sudo docker ps --format 'table {{.Names}}\t{{.Status}}'
pgrep -a nearRT-RIC
sudo tail -n 80 /local/logs/nearRT-RIC.log
```

Require the core functions to be healthy/running and successful E2 setup for the
single gNB. A running process or an earlier E2 request line alone is insufficient.

On cell1:

```bash
sudo docker logs --tail 100 ric5g-gnb-cell1
sudo docker exec ric5g-ue-cell1-1 ip -4 addr show oaitun_ue1
sudo docker exec ric5g-ue-cell1-2 ip -4 addr show oaitun_ue1
sudo bash /local/repository/bin/mgen-cell.sh check 1 2
```

Require actual UE registration and PDU-session establishment, distinct assigned
UE IPs, and data-network routes through `oaitun_ue1`. Registration alone is not
proof of a working user plane. Record the running image identities with
`bin/collect-cell-evidence.py`; registry digests do not identify a separately
cached or modified running container automatically.

From the operator laptop, using the reservation's real SSH addresses:

```bash
bash bin/mgen-run-distributed.sh CORE_SSH CELL1_SSH 1 all quick
UES_PER_CELL=2 RATE=10 SIZE=1000 DURATION=5 \
  bash bin/mgen-run-distributed.sh CORE_SSH CELL1_SSH 1 all full
```

These are connectivity tests, not scheduler benchmarks. For long-delay captures,
use an explicit campaign policy and lifecycle reconciliation as described in
`VALIDATION.md`; provisional preflight counts are not final packet-loss estimates.

## Start the monitoring xApp

On core, after readiness and packet reception are verified:

```bash
cd /opt/oai-src/openair2/E2AP/flexric
./build/examples/xApp/c/monitor/xapp_gtp_mac_rlc_pdcp_moni
```

Check that the xApp discovers one E2 node, receives reports for the traffic-active
UEs and writes its database. Confirm counters advance when traffic runs. A compiled
binary, subscription request or SQL row alone is not proof of valid measurements.
Use SIGINT/Ctrl-C for shutdown and inspect its completion message. Start with one
monitor; running multiple high-frequency monitors can perturb the radio workload.

## Scheduler baseline acceptance

First demonstrate two attached UEs carrying competing downlink traffic through
UPF. Save offered traffic, per-UE cumulative allocation/byte counters, receiver
logs, radio/wall-clock progression and cell CPU usage. Compare counter deltas
over a declared common interval, not counters from unrelated runs or the exported
MAC slot field. Keep enough load to make both UE queues active for a fairness test.

Do not run a placeholder slice-control example or unsupported RC action as a
PRB experiment. The real actuator and its matching control xApp need the extension
specified in `SCHEDULER_CONTROL.md`, followed by source/build and live validation.
