# Scheduler control: verified baseline and implementation boundary

The initial target is one cell with two competing UEs. The network and monitoring
xApps are included; an effective per-UE PRB actuator is not yet implemented.

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

## First actuator design

Keep slot-level decisions inside the gNB; let an xApp update policy parameters at
a slower, explicitly measured cadence. Implement and test these pieces together:

1. A bounded per-UE downlink weight or resource-budget state, keyed by the actual
   attached UE identity, with update synchronization against the MAC scheduler.
2. A defined E2 control schema and matching xApp encoder. Validate identifiers,
   ranges and stale UE contexts; reject unsupported operations without assertions.
3. A scheduler hook that consumes the state while preserving mandatory control
   signaling and retransmissions. A weighted priority is not a guaranteed PRB
   percentage; name the policy according to what it actually enforces.
4. Read-back of the applied version/parameters and actual MAC allocation counters.
   A command receipt, E2 ACK or desired-policy file is not sufficient evidence.
5. Deterministic baseline/rollback behavior and a reproducible gNB image build
   pinned to source, patches, build flags and resulting digest.

Two equally backlogged UEs are the first comparison. Equal and unequal shares
must be evaluated over a declared observation window, separately from offered
traffic and delivered bytes. HARQ, channel quality, inactive queues, TDD direction
and control overhead affect the result. Do not interpret traffic shaping as radio
PRB enforcement or expect a scheduling change to fix RFsim CPU slowdown.

The next implementation milestone is the real E2-to-MAC actuator and its xApp.
This repository currently supplies the complete single-cell stack and telemetry
baseline for that work. Handovers and multi-cell coordination are outside scope.
