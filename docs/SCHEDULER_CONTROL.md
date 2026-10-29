# Scheduler control: PRB allocation and scheduling policy

The profile deploys one cell with two competing UEs. The gNB can run a
controllable downlink scheduler (`GNB_CTRL=1`, see `DEPLOYMENT.md`) whose policy,
per-UE weights and per-UE resource-block caps are changed at run time and read
back from the gNB. The control path is local to the cell node; there is no E2
control path yet (see the findings below for why the stock E2 MAC control cannot
be used).

## What is controlled

The policy replaces the downlink `dl_rb_alloc` function of the OAI MAC scheduler
(a pluggable hook at the pinned revision). HARQ retransmissions and UEs that only
need a MAC control element are served first in every mode, as in the stock policy.

| Policy | Behaviour |
| --- | --- |
| `pf` | The stock proportional-fair policy, unchanged. This is the default after start and after `reset`. |
| `rr` | Time-domain round robin: backlogged UEs in RNTI order, each takes what it needs from the largest free block, so normally one UE per slot. |
| `wrr` | Weighted round robin in the time domain: the UE with the fewest granted resource blocks per unit of weight takes the slot, so resource-block shares follow the weights while each slot normally serves one UE (as `pf` does). |
| `wshare` | Frequency-domain weighted share: the largest free block of every slot is divided between all UEs with downlink data in proportion to their weights, never giving a UE more than it can use, never less than 5 resource blocks, and handing unused share to the others. |

Per-UE settings (not used by `pf`):

- `weight` (1 to 1000000, default 1000): relative share in `wshare`.
- `cap` (0 or 5 to 275): most resource blocks the UE gets in one slot, `0` = no cap. Applies to `wshare`, `wrr` and `rr`, and stays set when the policy is changed; `reset` clears it. With `rr` or `wrr` a capped UE leaves the rest of the slot to the next UE. A cap leaves resource blocks unused when the other UEs cannot take them.

Meaning of the numbers: a weight is a share of the resource blocks scheduled
*among UEs that are backlogged in the same slot*. It is not a guaranteed
throughput: UEs at different MCS turn the same resource blocks into different bit
rates, retransmissions and control signaling take resource blocks first, and a UE
with little traffic uses less than its share. Use `measure` for the allocation
that actually happened.

## Using it (on the cell node)

```bash
cd /local/repository
python3 bin/sched-ctl.py status                       # policy, weights, caps, counters per UE (RNTI)
python3 bin/sched-ctl.py map --containers ric5g-ue-cell1-1,ric5g-ue-cell1-2   # which RNTI is which UE
python3 bin/sched-ctl.py policy wshare                # or wrr, rr, pf
python3 bin/sched-ctl.py weight 1001 3000             # RNTI in hex, as printed by status
python3 bin/sched-ctl.py weight 1002 1000
python3 bin/sched-ctl.py measure --seconds 20         # resource blocks actually scheduled per UE
python3 bin/sched-ctl.py cap 1001 30
python3 bin/sched-ctl.py reset                        # back to the stock policy and defaults
```

Every changing command is acknowledged by the gNB and then read back with a second
request; the command fails unless the read-back shows the requested value. The
version number in `status` increases with every accepted change. RNTIs belong to
one attachment: when a UE disconnects its settings are dropped, so apply weights
again after UEs re-register.

## Implementation

| File | Role |
| --- | --- |
| `gnb/src/nr_dl_ctrl_alloc.c` | weighted max-min split of the free block (largest-remainder rounding, minimum grant, work conserving) |
| `gnb/src/nr_dl_ctrl_state.c` | per-UE state and the text command protocol, with validation |
| `gnb/src/gNB_scheduler_dlsch_ctrl_policy.c` | the policy, the lock handling and the UDP endpoint on `127.0.0.1:9099` (`SCHED_CTRL_ADDR`, `SCHED_CTRL_PORT`) |
| `gnb/patches/0001-controllable-dl-rb-policy.patch` | registers the sources in the OAI build and installs the policy at MAC start |
| `gnb/build-gnb-image.sh` | builds the gNB image from the stock OAI Dockerfiles at the pinned commit with the above applied |
| `bin/sched-ctl.py` | client with read-back, `measure` and UE-to-RNTI `map` |

State is read and written only with the MAC scheduler lock held, so a control
command is applied between two scheduling slots.

## Verification status

Done (`gnb/test`, `tests/test_scheduler_control.py`):

- The split is checked on 200,000 random cases for range, minimum grant, sum, work conservation and weighted fairness, plus fixed examples.
- The command parser is checked with valid and malformed input, version handling, reset and table limits.
- The policy runs for thousands of slots against copies of the gNB scheduler helpers (`find_largest_free_block` and `get_rb_alloc` are the pinned code): 3:1 weights give 3:1 resource blocks with every block used, equal weights give 1:1, a cap is honoured, a small backlog frees capacity for the other UE, retransmissions are served first with their exact size, round robin alternates strictly, and a failed commit leaves no gap or accounting.
- The same program runs under ThreadSanitizer, AddressSanitizer and UBSan with a control thread issuing commands while slots are scheduled.

Live results so far (single cell, two UEs, 15 Mbit/s UDP downlink offered to each through the UPF, 20 s windows, counters read from the gNB):

| Policy | Resource-block share UE1 / UE2 | Cell total |
| --- | --- | --- |
| `pf` | 49.6 / 50.4 % | 24.1 Mbit/s |
| `wshare`, equal weights | 50.0 / 50.0 % | 17.5 Mbit/s |
| `wshare`, 3000 : 1000 | 75.5 / 24.5 % (13.2 / 4.4 Mbit/s) | 17.5 Mbit/s |
| `wshare`, UE1 cap 30 | 28.7 / 71.3 % (30 of 106 is 28.3 %) | 17.6 Mbit/s |

The shares follow the settings. The cell total, however, is about 27 % lower with
`wshare` than with `pf`, independent of the weights: the policy is called about a
quarter less often per second while using the same number of resource blocks per
call. The likely cause is that serving two UEs in every slot makes each slot more
expensive for the RF simulator, so it completes fewer slots per second. That is a
property of this simulated setup, not of a radio, and it is the reason for `wrr`,
which shares by time slot and keeps one UE per slot. Whether `wrr` recovers the
total has to be measured (`sched-ctl.py measure` prints the call rate).

Not done yet, and required before relying on it:

- Building the gNB image (the stand-in headers in `gnb/test/oai_stub` are transcribed from the pinned source; the real compile happens in the image build).
- Running it on a live gNB: the resource-block shares above are properties of the policy given the gNB's candidate list; with real traffic they must be measured with `measure` alongside the delivered traffic.
- The E2 path: an E2 service-model control message and a matching xApp. The stock E2 MAC control is a stub in the gNB; implementing the message on both ends needs changes in the FlexRIC service model and the xApp.
- Uplink scheduling is not controlled. Handovers and multi-cell coordination are outside scope.

The sources are compiled into the OAI gNB and keep the gNB's licensing in mind
when distributing the resulting image.

## Exact pinned-source findings

OAI revision: `70508ebaf52f2aae420566d380c6537f2efb9f0c`.

| Interface | Source behavior | Consequence |
| --- | --- | --- |
| Customized MAC control | `write_ctrl_mac_sm` prints that the operation is unsupported | Do not send a purported PRB command through it and label it applied |
| Customized slice control | Add/delete/association requests are logged without applying NR scheduling changes | A received request/control outcome does not establish enforcement |
| Customized slice reader | Uses synthetic fill data | Do not use those slice values as real scheduler read-back |
| RC control | Advertises bearer/QoS mapping; handler asserts action ID 2 and parses/logs values | Does not implement a PRB-share control path |

Sources:

- [MAC implementation](https://github.com/OPENAIRINTERFACE/openairinterface5g/blob/70508ebaf52f2aae420566d380c6537f2efb9f0c/openair2/E2AP/RAN_FUNCTION/CUSTOMIZED/ran_func_mac.c)
- [Slice implementation](https://github.com/OPENAIRINTERFACE/openairinterface5g/blob/70508ebaf52f2aae420566d380c6537f2efb9f0c/openair2/E2AP/RAN_FUNCTION/CUSTOMIZED/ran_func_slice.c)
- [RC implementation](https://github.com/OPENAIRINTERFACE/openairinterface5g/blob/70508ebaf52f2aae420566d380c6537f2efb9f0c/openair2/E2AP/RAN_FUNCTION/O-RAN/ran_func_rc.c)

The radio images report the matching abbreviated source version, but undocumented
patches remain possible. Inspect the running artifacts before making a runtime
capability claim. Unsupported RC actions may hit assertions; do not probe them
by sending arbitrary control requests to a live gNB.

## Remaining boundary

The monitoring xApps remain read-only. Control currently goes through the local
endpoint above; an xApp-driven path needs the E2 extension described in the
findings and is not implemented here.
