# Scheduler control: PRB allocation and scheduling policy

The profile deploys one cell with two competing UEs. The gNB can run a
controllable downlink scheduler (`GNB_CTRL=1`, see `DEPLOYMENT.md`) whose policy,
per-UE weights and per-UE resource-block caps are changed at run time and read
back from the gNB. They can be set on the cell node with `bin/sched-ctl.py`, or from
an xApp over E2 (section "Control over E2"); both reach the same state through the
same local endpoint.

## What is controlled

The policy replaces the downlink `dl_rb_alloc` function of the OAI MAC scheduler
(a pluggable hook at the pinned revision). HARQ retransmissions and UEs that only
need a MAC control element are served first in every mode, as in the stock policy.

| Policy | Behaviour |
| --- | --- |
| `pf` | The stock proportional-fair policy, unchanged. This is the default after start and after `reset`. |
| `rr` | Time-domain round robin: backlogged UEs in RNTI order, each takes what it needs from the largest free block, so normally one UE per slot. |
| `wrr` | Weighted round robin in the time domain: the UE with the fewest granted resource blocks per unit of weight takes the slot, so resource-block shares follow the weights while each slot normally serves one UE (as `pf` does). |
| `slice` | Weighted round robin between slices of UEs, then between the UEs of the slice. A slice's share depends on its own weight, not on how many UEs it has. See the section on slices below. |
| `wshare` | Weighted share of the largest free block among all UEs with downlink data, at least 5 resource blocks each, unused share handed to the others, in as many slots as the gNB accepts. In the other slots one UE at a time takes the slot, least served relative to its weight first, so the long-run split follows the weights either way. See the section on the acknowledgement limit below. |

Per-UE settings (not used by `pf`):

- `weight` (1 to 1000000, default 1000): relative share in `wshare`.
- `cap` (0 or 5 to 275): most resource blocks the UE gets in one slot, `0` = no cap. Applies to `wshare`, `wrr` and `rr`, and stays set when the policy is changed; `reset` clears it. With `rr` or `wrr` a capped UE leaves the rest of the slot to the next UE. A cap leaves resource blocks unused when the other UEs cannot take them.

## Slices

With `POLICY slice` the UEs are grouped in up to 8 slices (ids 0 to 7; slice 0 is the
default, which every UE not placed elsewhere belongs to). The slice that has received the
fewest resource blocks relative to its weight takes the slot, and inside it the UE that
has received the fewest relative to its UE weight. As in `wrr`, each slot normally serves
one UE, retransmissions and control-element-only UEs are served first and are not charged
to the counters. A slice that had little or nothing to send does not collect credit
without limit: whenever a slice has data, its counter is raised if needed so that it is at
most 2000 resource blocks (of its own weight) behind the other slices that have data.
Without this bound a slice that sent at a low rate for a while took most of the slots for
the next minute (see the results in `SLICING_LIVE_TEST.md`). The same bound applies to a UE
against the other UEs with data in its slice (against all UEs for `wrr`), so a UE that
attaches or reconnects does not take every slot until it has caught up with its peers. The
credit is kept apart from the counters: the grant counters in `status` and `measure` count
resource blocks that were really granted. Design and rationale: `SLICING_DESIGN.md`.

Membership of a UE, first match wins: its RNTI association, a rule for its S-NSSAI, slice 0.
The S-NSSAI is the one the MAC reports for the UE's first data bearer, which comes from the
PDU session set up by the core. Rules apply once the policy has seen the UE with data.

```bash
python3 bin/sched-ctl.py policy slice
python3 bin/sched-ctl.py slice 1 weight 3000          # slice weight (1..1000000, default 1000)
python3 bin/sched-ctl.py slice 2 cap 30               # most resource blocks the UEs of the slice take together in one slot
python3 bin/sched-ctl.py assoc <rnti> 1               # put a UE in slice 1 (assoc <rnti> - removes it)
python3 bin/sched-ctl.py nssai-map 2 none 1           # UEs with SST 2 and no SD go to slice 1 (sd in hex)
python3 bin/sched-ctl.py status                       # slice table, rules, slice and source per UE
python3 bin/sched-ctl.py measure --seconds 20         # adds one row per slice
```

The cap of a slice limits the resource blocks all its UEs take together in one slot; the cap
of a UE limits that UE inside it, and a remainder below 5 resource blocks is not used. A
capped slice leaves the rest of the slot to the next slice, so a slot can serve several UEs.
Retransmissions are served before the new data and do not count against the cap. `reset`
clears the slices, the associations and the rules. Associations belong to an RNTI and are
dropped when the UE disconnects; rules stay until `reset`.

Status of this feature: tested on the host (`gnb/test`, `tests/`), including the property that
a slice share does not change with the number of UEs in the slice or with how many of them have
data. It was also run on a live single-cell gNB with four UEs and gave the expected shares for
slice weights, UE weights, the slice cap, membership by RNTI and by S-NSSAI, and the bounded
credit of slices and UEs (`SLICING_LIVE_TEST.md`, section 4).

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

## Control over E2

`xapp_sched_ctrl` (run on the core node, see `DEPLOYMENT.md`) sends a MAC service
model control message to every connected gNB that registered the MAC service model.
The message has two 32-bit words, packed as follows.

| Command | Header word | Message word |
| --- | --- | --- |
| `policy <pf\|rr\|wshare\|wrr\|slice>` | `0x53430001` | policy id: 0 pf, 1 rr, 2 wshare, 3 wrr, 4 slice |
| `weight <rnti\|*> <1..65535>` | `0x53430002` | `rnti << 16 \| weight`, RNTI 0 = default weight |
| `cap <rnti> <0\|5..275>` | `0x53430003` | `rnti << 16 \| cap` |
| `reset` | `0x53430004` | 0 |
| `slice-weight <0..7> <1..65535>` | `0x5343<slice>05` | weight |
| `slice-cap <0..7> <0\|5..275>` | `0x5343<slice>06` | cap |
| `assoc <rnti> <0..7\|none>` | `0x5343<slice>07` (`ff` = remove) | `rnti << 16` |
| `nssai-map <sst> <sd\|none> <0..7\|none>` | `0x5343<slice>08` (`ff` = delete) | `sst << 24 \| sd` |

`<slice>` is one byte in bits 8 to 15 of the header word; the first four commands have 0
there, so words built before the slice commands existed keep their meaning. `policy slice`
is policy id 4.

The gNB turns the words into the text command of the local endpoint and sends it
there, so a command over E2 has exactly the validation and effect of one from
`sched-ctl.py`. Words that are not a valid scheduler command (the stock example sends
header 1, message 42) are ignored and logged. The weight range over E2 is smaller than
on the cell node (65535 against 1000000).

An acknowledgement received by the xApp means that the gNB received the message. The
FlexRIC agent of the pinned revision returns no control outcome, so a refused command
(unknown RNTI, value out of range) is visible only in the gNB log and in `status`.

## Implementation

| File | Role |
| --- | --- |
| `gnb/src/nr_dl_ctrl_alloc.c` | weighted max-min split of the free block (largest-remainder rounding, minimum grant, work conserving) |
| `gnb/src/nr_dl_ctrl_state.c` | per-UE state and the text command protocol, with validation |
| `gnb/src/gNB_scheduler_dlsch_ctrl_policy.c` | the policy, the lock handling and the UDP endpoint on `127.0.0.1:9099` (`SCHED_CTRL_ADDR`, `SCHED_CTRL_PORT`) |
| `gnb/patches/0001-controllable-dl-rb-policy.patch` | registers the sources in the OAI build and installs the policy at MAC start |
| `gnb/build-gnb-image.sh` | builds the gNB image from the stock OAI Dockerfiles at the pinned commit with the above applied |
| `bin/sched-ctl.py` | client with read-back, `measure` and UE-to-RNTI `map` |
| `gnb/src/nr_dl_ctrl_e2.h` | encoding of a command in the two words of the MAC control message, shared by the xApp and the gNB |
| `gnb/patches/0002-e2-mac-control.patch` | `write_ctrl_mac_sm` hands the command to the local endpoint |
| `gnb/patches/0003-flexric-mac-control-agent.fxpatch` | removes the two checks in the gNB's FlexRIC agent that only allow header 1 and message 42 |
| `xapp/xapp_sched_ctrl.c`, `bin/patch-flexric-sched-ctrl.py` | the control xApp and its installation into the FlexRIC build |

State is read and written only with the MAC scheduler lock held, so a control
command is applied between two scheduling slots.

## Verification status

Done (`gnb/test`, `tests/test_scheduler_control.py`):

- The split is checked on 200,000 random cases for range, minimum grant, sum, work conservation and weighted fairness, plus fixed examples.
- The command parser is checked with valid and malformed input, version handling, reset and table limits.
- The policy runs for thousands of slots against copies of the gNB scheduler helpers (`find_largest_free_block` and `get_rb_alloc` are the pinned code): 3:1 weights give 3:1 resource blocks with every block used, equal weights give 1:1, a cap is honoured, a small backlog frees capacity for the other UE, retransmissions are served first with their exact size, round robin alternates strictly, and a failed commit leaves no gap or accounting.
- The same program runs under ThreadSanitizer, AddressSanitizer and UBSan with a control thread issuing commands while slots are scheduled.

First reservation, earlier image (single cell, two UEs, 15 Mbit/s UDP downlink offered to each through the UPF, 20 s windows, counters read from the gNB; weights 3000 : 1000 where given). The `wshare` row is the version that split every slot and is superseded by the measurements further down:

| Policy | Resource-block share UE1 / UE2 | Cell total | Resource blocks per scheduler call |
| --- | --- | --- | --- |
| `pf` | 50.0 / 50.0 % | 24.4 Mbit/s | 104.9 |
| `wrr` | 74.7 / 25.3 % (17.5 / 6.6 Mbit/s) | 24.1 Mbit/s | 104.9 |
| `wshare` | 75.5 / 24.5 % (13.3 / 4.4 Mbit/s) | 17.7 Mbit/s | 78.3 |
| `rr` | 48.6 / 51.4 % | 24.3 Mbit/s | 104.9 |

A cap of 30 on one UE gave it 28.7 % of the resource blocks (30 of 106 is 28.3 %).
The receivers inside the UEs agree with the gNB counters on who got the larger share.

Second reservation, image `ric5g/oai-gnb-ctrl:70508eba-c8747dfc6bc3` (single cell, two UEs, AWGN
channel, 15 Mbit/s UDP offered to each UE for 60 s, `measure` over a 20 s window in the middle
of the transfer). Every policy and weight was set from the xApp over E2 (`xapp_sched_ctrl`) and
read back with `sched-ctl.py status`. `ric5g-ue-cell1-1` is `0x1c04` (12.1.1.3) and
`ric5g-ue-cell1-2` is `0xa02c` (12.1.1.2); in the 3:1 runs `0x1c04` has weight 3000 and
`0xa02c` has 1000.

| Run | Share `0xa02c` / `0x1c04` | Cell total | Slots shared / one UE at a time |
| --- | --- | --- | --- |
| `pf` | 49.9 / 50.1 % | 19.07 Mbit/s | - |
| `pf`, repeat at the end | 49.9 / 50.1 % | 19.02 Mbit/s | - |
| `wshare`, equal weights | 50.0 / 50.0 % | 18.87 Mbit/s | 2626 / 2388 |
| `wshare`, equal weights, repeat | 50.0 / 50.0 % | 18.91 Mbit/s | 2616 / 2407 |
| `wshare`, 3:1 | 25.6 / 74.4 % | 19.29 Mbit/s | 102 / 4952 |
| `wshare`, 3:1, repeat | 25.5 / 74.5 % | 18.64 Mbit/s | 98 / 4788 |

What this shows, and what it does not:

- The cell total of `wshare` is within about 2 % of `pf` in this session, against a loss of about 25 % before the change that makes the sharing fraction depend only on ACK/NACK refusals. The run-to-run spread of the total is about 3 %, so a difference of 1 to 2 % is not resolved.
- With equal weights the slot is shared in about half of the contested slots (48 to 54 %). With 3:1 weights the fraction falls to its floor (2 %), the refusals fall in slot indices 5, 6, 15 and 16, and the policy behaves like `wrr`: the split is 74.5 : 25.5, at the limit that the ACK/NACK allowance permits, not 75 : 25 by design.
- The receivers agree with the gNB: in the 3:1 runs the UE with weight 3000 received 12.6 to 12.8 Mbit/s without loss, the other 6.8 to 7.1 Mbit/s averaged over a longer drain with 24 to 27 % of its datagrams lost.
- The `pf` total here (about 19 Mbit/s at 104.6 to 104.8 resource blocks per call) is lower than in the first reservation (24.4 Mbit/s at 104.9). The stock policy shows the same, so it is not caused by the scheduler; the cause was not determined. Compare only runs of the same session.
- Two runs per `wshare` configuration, three of `pf`, one reservation. `wrr`, `rr` and caps were not repeated in this session; they were measured with four UEs in the next reservation (below).

Third reservation, same image, single cell, four UEs, AWGN channel. 15 Mbit/s UDP offered to each
UE (60 Mbit/s in total against about 13 Mbit/s that the cell carries) for 75 s, `measure` over a 20 s
window started about 10 s after the traffic. Every policy and weight was set from the xApp
over E2 and read back with `sched-ctl.py status`. The UEs are `0x56a2` (`ric5g-ue-cell1-1`, 12.1.1.2),
`0x34b1` (`-2`, 12.1.1.3), `0xb38a` (`-3`, 12.1.1.4) and `0x8f45` (`-4`, 12.1.1.5). Weighted runs use
4000 : 2000 : 1000 : 1000 in that order. Runs where `status` or `measure` showed that a command was still
being applied during the window were discarded and repeated, except the one marked below.

| Run | Share `0x56a2` / `0x34b1` / `0xb38a` / `0x8f45` | Cell total |
| --- | --- | --- |
| `pf` | 25.8 / 25.5 / 23.6 / 25.1 % | 13.83 Mbit/s |
| `pf`, repeat | 25.2 / 25.3 / 24.0 / 25.5 % | 11.16 Mbit/s |
| `rr` | 23.9 / 23.5 / 21.8 / 30.8 % | 13.27 Mbit/s |
| `wrr`, equal weights | 25.0 / 25.0 / 25.0 / 25.0 % | 11.79 Mbit/s |
| `wrr`, equal weights, repeat | 25.1 / 25.1 / 24.6 / 25.1 % | 13.77 Mbit/s |
| `wrr`, 4:2:1:1 (see note) | 50.0 / 25.0 / 12.5 / 12.5 % | 8.64 Mbit/s |
| `wshare`, equal weights | 25.0 / 25.0 / 25.0 / 25.0 % | 13.83 Mbit/s |
| `wshare`, 4:2:1:1 | 50.0 / 25.0 / 12.5 / 12.5 % | 13.74 Mbit/s |
| `wshare`, equal weights, cap 20 on `0x56a2` | 14.5 / 28.5 / 28.5 / 28.5 % | 13.44 Mbit/s |

- Weights are followed with four UEs by both weighted policies: 4:2:1:1 gives 50.0 / 25.0 / 12.5 / 12.5 % of the resource blocks and 3.9 : 2.0 : 1 : 1 in throughput. The `wshare` run used 104.5 resource blocks per call and 50 % sharing, but in 1771 of its 1833 shared slots only one UE was actually granted, so with these weights it behaves mostly like time sharing. It was not tested at ratios the ACK/NACK allowance cannot meet.
- Note on the `wrr` 4:2:1:1 row: the `measure` window started before the last two `weight` commands had been delivered and before the traffic was complete (13,186 resource blocks per second against about 18,800 in the other runs). Those two commands set 1000, the value the UEs already had, so the weights in effect were 4:2:1:1 for the whole window and the shares are credible, but the total is not comparable and the run was not repeated cleanly.
- The cap holds: `0x56a2` with a cap of 20 resource blocks per grant received 14.5 % of the resource blocks, the others 28.5 % each, and the cell still used 104.2 resource blocks per call.
- The cell total scatters from 11.16 to 13.83 Mbit/s between runs of the same policy at the same resource blocks per second (18,800 to 18,900): `pf` gave both 13.83 and 11.16, `wrr` both 11.79 and 13.77. The apparent loss of `wrr` against `pf` in the first pair of runs was therefore noise, not a property of the policy. The cause of the scatter was not determined; no policy ranking by total throughput can be drawn from these runs, only by resource-block share.
- With four UEs the simulation runs at about 180 scheduler calls per second instead of about 250 with two UEs, with the same number of bits per call, so absolute Mbit/s are not comparable with the two-UE tables. Shares are.
- `rr` gave `0x8f45` 30.8 % in the one run made. A repeat was not made. A possible cause is the round-robin order lining up with the TDD frame of the cell, since `wrr`, which counts resource blocks, does not show it; this was not verified.
- The sender side of the iperf3 test shows a steady 15.0 Mbit/s per UE for the whole transfer, so the cell was the bottleneck in every run. The receiver summaries were not captured, so the figures above come from the gNB counters only.

The acknowledgement limit. Measured on the live gNB (two UEs, saturated downlink):
a `wshare` that split every slot between both UEs used only 74 to 78 % of the
resource blocks (17.2 to 18.1 Mbit/s against 24.0 with `pf`). 94 % of the refused
grants were refused for lack of an ACK/NACK occasion (PUCCH), only 6 % for lack of
a PDCCH candidate, and they fell in a handful of slot indices of the frame: in the
last downlink slots before the uplink slots almost every grant to either UE was
refused. The MAC accepts only a limited number of grants per UE for each
acknowledgement occasion; granting both UEs in every slot spends that allowance
before the end of the period and the remaining slots are lost for everybody.
`pf` and `wrr` give one UE the whole slot and so need one grant per slot.

`wshare` therefore shares a slot in frequency only in a fraction of the contested
slots and gives the others to one UE at a time. The fraction starts at 50 %, drops
by 6 % points when every grant of a slot was refused for lack of an ACK/NACK
occasion, by 2 % points when one grant was, grows by 0.2 % points per clean shared
slot, and never goes below 2 %. Refusals for lack of a PDCCH candidate (they occur
in fixed slots of the frame, for example slot indices 0 and 14 in the measurements)
are counted but do not change the fraction.
`status` and `measure` show how many slots were shared, how many went to one UE, how
many lost every grant and the current fraction, plus the refusals by cause and by
slot index. A consequence of the limit: weights cannot be followed beyond what the
allowance permits. With an allowance of 5 grants per UE in 7 slots a UE can hold at
most 5/7 of the slots (71 %), so a 3:1 weight ratio is met as 71:29. This is
verified against a host-side model of the allowance and then on a live gNB (results above:
cell total close to `pf`, shares following the weights as far as the allowance allows).
`wrr` remains the choice for time-multiplexed weights.

Not done yet, and required before relying on it:

- Building the gNB image (the stand-in headers in `gnb/test/oai_stub` are transcribed from the pinned source; the real compile happens in the image build).
- Longer and repeated runs, other channel conditions and more than four UEs: the live results above come from three reservations, with two and four UEs, AWGN. Single runs of 20 s were used for most four-UE rows.
- The E2 path is confirmed on one reservation: policy, weight, cap and reset sent by `xapp_sched_ctrl` through the RIC were applied by the gNB (read back with `sched-ctl.py status`), a command for an unknown RNTI was refused by the gNB while the xApp still reported it as delivered, and an invalid word was rejected by the xApp. It has not been tried with several gNBs or with the E42 subscription load of the monitoring xApps running at the same time.
- Slice scheduling (policy `slice`, associations, S-NSSAI rules): measured on a live gNB with four UEs (`SLICING_LIVE_TEST.md`, section 4), including the bounded credit of slices and UEs and the slice-level cap. A UE that attaches with a new RNTI during a run was not tested on the cell, and whether two different S-NSSAIs work with the minimal core is not verified. The standard FlexRIC slice message is not mapped.
- Uplink scheduling is not controlled. Handovers and multi-cell coordination are outside scope.

The sources are compiled into the OAI gNB and keep the gNB's licensing in mind
when distributing the resulting image.

## Exact pinned-source findings

OAI revision: `70508ebaf52f2aae420566d380c6537f2efb9f0c`.

| Interface | Source behavior | Consequence |
| --- | --- | --- |
| Customized MAC control | `write_ctrl_mac_sm` prints that the operation is unsupported; the agent and the RIC accept only header 1 and message 42 | Replaced by the E2 path above; a stock `xapp_mac_ctrl` message is ignored by the new handler, and an acknowledgement never shows that scheduling changed |
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

The monitoring xApps remain read-only. Control goes through the local endpoint
above, directly or from `xapp_sched_ctrl` over E2; the E2 path was confirmed on one
reservation (see the verification status).
