# oai-5g-scheduler-control

Single-cell OAI 5G Standalone testbed for scheduler-control research, with a
complete software 5G protocol stack and FlexRIC near-RT RIC/xApp integration.
The default reservation uses **two physical hosts, one gNB and two OAI UEs**.

The UE and gNB execute the NR PHY/MAC/RLC/PDCP/RRC and NAS procedures. RFsim
transports IQ samples instead of over-the-air radios. This is a full software
network, not a packet-only model; it does not provide physical RF coverage or
commercial-device connectivity without a separate radio deployment.

## Architecture

```text
core host: 10.10.1.1                 cell1 host: 10.10.1.11
  MySQL + UDR/UDM/AUSF                 OAI gNB + embedded E2 agent
  AMF/SMF/UPF                            | RFsim on this same host
  external data network                  +-- OAI UE1 + oaitun_ue1
  FlexRIC near-RT RIC                     +-- OAI UE2 + oaitun_ue1
  monitoring xApps
        | N2 / N3 / E2 over experimental LAN |
```

The complete inherited core includes subscriber authentication, registration,
PDU-session establishment and a real user plane through UPF to the data network.
The RIC is built natively on core from the pinned OAI FlexRIC submodule.
UEs remain colocated with their gNB, retaining the baseline local IQ transport.
There is no handover prototype or extra cell in this repository's active profile.

## What is implemented

| Capability | Status |
| --- | --- |
| One-cell POWDER allocation, two UEs by default | Implemented; new repository deployment pending |
| Full OAI 5G SA core and RFsim RAN setup | Inherited setup retained; fresh reservation validation required |
| Embedded E2 agent and matched FlexRIC build | Included, with pinned radio artifact references |
| Monitoring xApps and SQLite telemetry | Included through the FlexRIC build; start after E2 setup |
| Bounded MGEN traffic and receiver observation safeguards | Included with local regression and loopback evidence |
| xApp-enforced per-UE PRB shares or scheduler switching | **Not implemented in the pinned upstream control path** |

The first scheduler target is configurable **downlink allocation between two
competing UEs**, with the unmodified scheduler as the baseline. Inspecting the
exact pinned source exposed unsupported or placeholder control callbacks; sending
an E2 control message is not evidence that allocation changed. See
[docs/SCHEDULER_CONTROL.md](docs/SCHEDULER_CONTROL.md). No unsupported scheduler
xApp is auto-started, and no fixed-share enforcement is claimed.

## Deploy later on a separate reservation

Create a new POWDER profile from this private repository, using `profile.py`.
Authorize POWDER to read the private repository, as required by your project.
The entrypoint fixes the number of cells to one; `ues_per_cell` defaults to two.
It reserves core (default d430) and cell1 (default d740). Availability is not checked
here. The profile automatically executes setup on the newly reserved hosts.

The profile, setup and subscriber generator default to one cell and two UEs.
Cell/core setup reject extra cells before installing or changing services.
The core release family remains v2.0.0; the radio artifacts are pinned by digest.
See [BUILD.md](BUILD.md) for provenance limitations. Do not deploy this on the
hosts serving an existing measurement campaign.

Boot order: **core network and RIC → gNB → UEs → traffic → xApp**.
Setup logs are `/local/logs/setup.log` on each host; RIC output is
`/local/logs/nearRT-RIC.log` on core. Inspect setup errors rather than treating
container existence as registration or PDU-session success. Bring-up requires
package downloads and source compilation; it has not been timed on a fresh
reservation for this repository.

Check the core containers, gNB E2 registration, and both UEs' tunnels before
sending traffic. MGEN helpers discover assigned UE IPs rather than assuming them.
The readiness commands in [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) are for the
later reservation; creating this repository does not execute them.

## Monitoring xApps

After the gNB is registered with the RIC and UE traffic has been verified, run on
core, choosing **one** monitor for the first experiment:

```bash
cd /opt/oai-src/openair2/E2AP/flexric
./build/examples/xApp/c/monitor/xapp_kpm_moni
# Or, for MAC/RLC/PDCP/GTP telemetry:
./build/examples/xApp/c/monitor/xapp_gtp_mac_rlc_pdcp_moni
```

These executables are built by `bin/core-setup.sh`; they do not require a separate
standalone FlexRIC installation. The source pins OAI
`70508ebaf52f2aae420566d380c6537f2efb9f0c` and FlexRIC
`ef6d722f22191eea74089966983da1f5ec1fedd4`.

Start with the smallest reporting scope/rate needed. Do not start a monitor before
E2 registration or treat placeholder slice values as scheduler state. In the
pinned source, the customized slice reader uses synthetic fill data; it is not
proof of enforced slice allocation. MAC cumulative counters can support allocation
measurement, but the exported slot field is not a reliable per-slot index.

Stop xApps with SIGINT/Ctrl-C and verify their clean shutdown; do not force-kill
them. Restarting RIC requires restarting/re-registering the gNB, which drops UE
sessions. Keep this separate from scheduler-policy comparisons.

## Local verification

```bash
python3 -m pip install -r tests/requirements.txt
python3 -m pytest -q
```

Inherited tests cover channel mapping, FlexRIC patches, QoS generation, receiver
lifecycle and packet reconciliation. The Linux-only isolated MGEN compatibility
test and raw evidence are retained under `tests/integration/` and `validation/`.
Local tests do not certify this new deployment or future scheduler actuation.

See [VALIDATION.md](VALIDATION.md) for explicit drain policies and censored packets,
and [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) for the initial two-UE acceptance run.
