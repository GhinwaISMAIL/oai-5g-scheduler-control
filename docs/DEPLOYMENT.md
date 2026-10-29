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
