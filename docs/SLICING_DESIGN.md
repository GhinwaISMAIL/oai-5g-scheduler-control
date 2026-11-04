# Slice-aware downlink scheduling: design

Status: step A (below) is implemented on branch `slicing`, tested on the host and run on a live
gNB with four UEs with the expected shares (`SLICING_LIVE_TEST.md`, section 4). Steps B and C are
not started.

## Goal

Treat groups of UEs as slices. The scheduler divides the downlink resource blocks
between slices by slice weight, and inside a slice between its UEs by UE weight.
The slice share must not depend on how many UEs a slice has or how many of them
currently have data, which is exactly what per-UE weights cannot give (a slice
with three backlogged UEs of weight 1000 gets three times the share of a slice
with one).

Control must work from `bin/sched-ctl.py` on the cell node and from an xApp over E2,
through the same local endpoint as the existing commands.

## What the pinned sources provide (OAI `70508eba`)

| Fact | Where |
| --- | --- |
| Each scheduler candidate carries the S-NSSAI (`sst`, `sd`) of the UE's first DRB; `{0, 0xFFFFFF}` when the UE has none | `gNB_scheduler_dlsch.c` (candidate collection), `nr_dl_candidate_t.nssai` |
| That value comes from the PDU session set up by the core | `rrc_gNB.c` (`drb_info.nssai = pdu->nssai`), `mac_rrc_dl_handler.c` |
| The UE takes `nssai_sst` and `nssai_sd` per PDU session | `openair3/UICC/pdu_session.c`, `usim_interface.c` |
| The standard E2 slice control handler only prints and acknowledges; the slice reader returns random data | `ran_func_slice.c` |

So slice membership can come from the RNTI (set by a command) or from the NSSAI
(set by the core), and the existing hook already receives both.

## Model

- Up to 8 slices, ids 0 to 7. Slice 0 is the default slice: a UE that is not associated
  with another slice is in it.
- Slice settings: `weight` (1 to 1000000, default 1000) and `cap` (0 or 5 to 275).
- UE settings stay as they are (`weight`, `cap`); inside a slice they decide the split between UEs.
- Membership of a UE, first match wins:
  1. an explicit association of its RNTI (`ASSOC`),
  2. an NSSAI rule that matches the UE's `sst` and `sd` (`NSSAIMAP`),
  3. slice 0.
- Associations are per RNTI and dropped when the UE disconnects, like the UE weights.
  NSSAI rules are kept until `RESET`.

## Policy `slice`

A new policy next to `pf`, `rr`, `wrr` and `wshare`. It reuses the sequential visit of
`wrr` (UEs with new data visited one after the other, each taking what it needs from
the largest free block) and changes only the order and the accounting:

1. Retransmissions and UEs that only need a control element are served first, as in every policy.
2. The UEs with new data are ordered by (slice deficit, UE deficit, RNTI), smallest first, where
   the deficit of a slice is its granted resource blocks divided by its weight, and the deficit
   of a UE is as today. Slices with no backlogged UE do not take part.
3. Each granted block is added to the counters of the UE and of its slice.
4. The cap of a slice limits the resource blocks all UEs of the slice take together in one
   scheduler call; the cap of a UE limits that UE inside it. A remainder below the minimum grant
   (5 resource blocks) is not used, and what a capped slice leaves is taken by the next slice in
   the order, so a slot can serve several UEs. Retransmissions are served before the new data
   and do not count against a cap.
5. Credit is bounded. Whenever a slice has a UE with new data, its counter is raised, never
   lowered, so that its deficit is at most `NR_CTRL_SLICE_CREDIT_RBS` (2000 resource blocks, in
   units of its own weight) below the lowest deficit of the other backlogged slices. Without a
   bound, a slice that sends little for a while falls behind and then takes every slot until it has
   caught up: in the first live run this gave a slice of weight 1000 about 75 % of the cell against
   another of weight 1000 for the whole run. The first version only acted after 500 consecutive
   calls without data, which a slice with sparse data never reaches.
6. The same bound applies to a UE: a UE with new data is raised so that its deficit is at most
   `NR_CTRL_UE_CREDIT_RBS` (2000 resource blocks, in units of its own weight) below the lowest
   deficit of the other UEs with new data in its slice (in all UEs for `wrr`). A UE that attaches,
   or reconnects with a new RNTI, therefore gets a burst of at most that size and then its weight.
   Credit is virtual: it is stored apart from the granted resource blocks, and only the order of
   visit uses it. The counters that `status` and `measure` show, for UEs and for slices, are real
   grants, and the counter of a slice equals the sum of the counters of its UEs.

Consequences, stated before they are measured:

- Slices are shared in time, one UE per slot as in `wrr`. Sharing a slot in frequency between
  slices would meet the same ACK/NACK grant allowance that limits `wshare`, so it is not offered.
- A slice share is a share of resource blocks, not a throughput or a guarantee. An overloaded slice
  cannot take more than its weight allows, but there is no reserved minimum.
- The inside-slice policy is fixed to weighted round robin by UE weight. Inside-slice `rr` or
  frequency sharing can be added later; nothing in the state prevents it.

## Commands

Text protocol of the local endpoint (RNTI in hex, slice as a decimal id):

| Command | Effect |
| --- | --- |
| `POLICY slice` | selects the policy |
| `SLICE <id> WEIGHT <1..1000000>` | slice weight |
| `SLICE <id> CAP <0\|5..275>` | slice cap, 0 = none |
| `ASSOC <rnti> <id\|->` | puts the UE in a slice; `-` removes the association |
| `NSSAIMAP <sst> <sd\|none> <id\|->` | rule for UEs with that NSSAI; `none` is `sd` `0xFFFFFF`; `-` deletes the rule |

`RESET` clears slice weights, caps, associations and rules. The reply format, the
version counter and the read-back by `sched-ctl.py` are the same as for the existing commands.

`status` gets a slice table (id, weight, cap, granted resource blocks, UEs) and a slice and
source column (`rnti`, `nssai`, `default`) per UE. `measure` adds one row per slice.

## E2 encoding

Same two words as today. The header word becomes `0x5343 << 16 | arg << 8 | opcode`; the
existing opcodes 1 to 4 have `arg` 0, so old images and xApps keep working, and an image
without this feature ignores the new words like any other non-scheduler word.

| Command | Opcode | `arg` | Message word |
| --- | --- | --- | --- |
| `policy slice` | 1 | 0 | policy id 4 |
| `slice-weight <id> <w>` | 5 | slice id | weight (1 to 65535) |
| `slice-cap <id> <c>` | 6 | slice id | cap |
| `assoc <rnti> <id\|none>` | 7 | slice id, 0xFF = remove | rnti << 16 |
| `nssai-map <sst> <sd\|none> <id\|none>` | 8 | slice id, 0xFF = delete | sst << 24 \| sd (24 bits) |

As for the existing commands, an acknowledgement in the xApp only means the gNB received the
message; `sched-ctl.py status` is the check.

## Tests

Host side, in `gnb/test/test_ctrl.c` and `tests/`:

- Command parser: valid and malformed input for every new command, limits, version handling, `RESET`.
- Shares: with a slice of two UEs and a slice of one UE, weights 3 : 1, the resource blocks split 75 : 25
  whatever the number of UEs and whether one of them stops having data (the property per-UE weights do not have).
- Inside a slice the split follows the UE weights.
- Credit: a slice that returns after a long idle period, or that had data in only one call out of 20,
  gets a burst of at most the credit and then its weight; with equal and with unequal weights.
  The same for a UE that joins late, in `wrr` and inside a slice, with equal and unequal weights;
  a late UE does not disturb the other slice; the slice counter equals the sum of its UE counters.
- Caps: the slice cap limits the slice in total per slot; the UE cap works inside it; a remainder below
  the minimum grant is not used; without a slice cap the first UE takes the slot as before.
- Membership: precedence of `ASSOC` over `NSSAIMAP` over slice 0; a disconnecting UE loses its association; a rule survives.
- Retransmissions are still served first and the counters are not charged for them.
- E2: round trip of every new word, rejection of out-of-range values, old words unchanged.
- The existing ThreadSanitizer, AddressSanitizer and UBSan runs with a control thread active, and the OAI
  build-flags test that compiles the handler with `-Werror`.

Live, four UEs, `slice` policy, exact commands written when the code exists:

1. Slice 1 = two UEs, slice 2 = two UEs, weights 3000 : 1000. Expect about 75 : 25.
2. Same weights, slice 1 = three UEs, slice 2 = one UE. Expect the same 75 : 25.
3. Stop the traffic to one UE of slice 1. Expect the slice shares to stay at 75 : 25.
4. Slice cap, then `RESET`.
5. Same through `xapp_sched_ctrl`, then membership by NSSAI (needs the core and UE configuration of step B).

## Implementation steps

A. Scheduler and control (host-testable): the state and parser in `nr_dl_ctrl_state.[ch]`, the ordering
   in `gNB_scheduler_dlsch_ctrl_policy.c`, the encoding in `nr_dl_ctrl_e2.h`, `sched-ctl.py`, `xapp_sched_ctrl.c`,
   tests, documentation. Membership by RNTI only.

B. Membership by NSSAI: a second S-NSSAI (SST 2 without `sd`) in the gNB `snssaiList`, in the AMF, SMF and
   UPF configuration, in the generated subscribers, and a per-UE `nssai_sst` in the UE configuration. It is
   switched on by the profile parameter `second_slice` (off by default; see `DEPLOYMENT.md`) and covered by
   host tests of the generated files. The scheduler side is the `NSSAIMAP` rule above. Not yet run on a
   reservation: whether the minimal core, with one SMF and one UPF, serves two S-NSSAIs
   (`SLICING_LIVE_TEST.md`, run 9).

C. Standard FlexRIC slice message: a handler in `ran_func_slice.c` that maps slice add and association to
   the commands above. Requires the FlexRIC slice message definitions, which have not been read.

## Decisions to confirm

1. Eight slices, ids 0 to 7, slice 0 as default.
2. Inside-slice policy fixed to weighted round robin by UE weight.
3. Slice cap as the total of the slice per slot (revised: the first version limited each UE of the
   slice separately, so a slice with n UEs could take n times the cap).
4. Bounded credit of 2000 resource blocks, for slices and for UEs (items 5 and 6 of the policy;
   revised: the first version had an idle rule of 500 calls and no bound for UEs).
5. A guaranteed minimum share per slice is not part of this step.
