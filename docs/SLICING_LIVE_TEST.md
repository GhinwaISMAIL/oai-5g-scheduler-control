# Live test of slice-aware scheduling

Run on a reservation with four UEs. Everything below is for the `slicing` branch. What each
run should show is written down first, so that a different result is a finding and not
something to explain afterwards. Sections 1 to 4 are the procedure; section 5 holds the results
of the first live session (runs 0 to 6 on the first version of the slice policy). Run 7 was added
afterwards, with the revised policy, and has not been run yet.

## 1. Bring the reservation to the `slicing` branch

The sources of the gNB image changed, so the image has to be built or pulled and the xApp on
the core node rebuilt.

**Reservation started from the `slicing` branch** (the profile was instantiated from it): the
nodes already run this code. The core builds the xApp by itself. The cell node finds that
`gnb_ctrl_image` does not exist on the registry and builds the image itself, about 25 minutes;
**run nothing else on cell1 meanwhile**. `bin/cell-setup.sh` restarts Docker, so a second
`node-setup.sh cell` while the build runs cancels it ("context canceled" in
`/local/logs/gnb-build.log`). Watch it with:

```bash
tail -n 1 /local/logs/gnb-build.log; ls /local/logs/GNB_IMAGE /local/logs/CELL_READY
```

If the build was cancelled or failed, start one build alone, in the background, wait for the
tag, then start the cell once:

```bash
cd /local/repository
sudo bash -c 'nohup bash gnb/build-gnb-image.sh > /local/logs/gnb-build.stdout 2>&1 &'
tail -n 1 /local/logs/gnb-build.log; cat /local/logs/gnb-build.tag 2>/dev/null   # repeat until the tag appears
sudo GNB_CTRL=1 bash bin/node-setup.sh cell 1 1 4 uniform AWGN >> /local/logs/setup.log 2>&1
cat /local/logs/GNB_IMAGE /local/logs/CELL_READY
```

**Running reservation started from `main`:** switch both nodes, core first.

```bash
cd /local/repository && sudo git fetch origin slicing && sudo git checkout slicing && sudo git pull --ff-only
sudo bash bin/node-setup.sh core 0 1 4 uniform AWGN >> /local/logs/setup.log 2>&1      # core node only
```

then on cell1 run the same `git` line and the four commands of the previous block.

**A newer commit of the `slicing` branch on a running reservation** (the gNB sources changed, so
`profile.py` carries a new image tag): only cell1 needs the new image. The core node and its xApp
stay as they are unless `xapp/` changed in the commit. Nothing else may run on cell1 during the
build, and the gNB and the UEs restart afterwards, so the RNTIs change: redo section 2.

```bash
cd /local/repository
sudo git pull --ff-only origin slicing
sudo bash -c 'nohup bash gnb/build-gnb-image.sh > /local/logs/gnb-build.stdout 2>&1 &'
tail -n 1 /local/logs/gnb-build.log; cat /local/logs/gnb-build.tag 2>/dev/null   # repeat until the tag appears
sudo GNB_CTRL=1 bash bin/node-setup.sh cell 1 1 4 uniform AWGN >> /local/logs/setup.log 2>&1
cat /local/logs/GNB_IMAGE /local/logs/CELL_READY
```

Check on the core node that the xApp is the new one (it does not contact the gNB):

```bash
/opt/oai-src/openair2/E2AP/flexric/build/examples/xApp/c/ctrl/xapp_sched_ctrl bogus 2>&1 | head -12
```

The usage lists `slice-weight`, `slice-cap`, `assoc` and `nssai-map`. On cell1,
`/local/logs/GNB_IMAGE` must show a tag that ends with the hash of this checkout's `gnb/src` and
`gnb/patches` (`profile.py` and `tests/test_single_cell_profile.py` carry the same tag).

## 2. Variables and helper scripts

On cell1, read the RNTIs and map them to the containers:

```bash
cd /local/repository
python3 bin/sched-ctl.py status | head -8
sudo python3 bin/sched-ctl.py map --containers ric5g-ue-cell1-1,ric5g-ue-cell1-2,ric5g-ue-cell1-3,ric5g-ue-cell1-4
```

Read the address of each UE container (the order differs between reservations) and start an
iperf3 server in each. The iperf3 server in a UE container can crash (a `Segmentation fault` at
the end of `/tmp/srv.txt`, then `Connection refused` for the next client), and a UE without a server
has no traffic, which invalidates a run. Each container therefore runs a loop that starts a fresh
one-shot server after every test:

```bash
for i in 1 2 3 4; do echo -n "ue-$i "; sudo docker exec ric5g-ue-cell1-$i ip -4 -o addr show oaitun_ue1 | awk '{print $4}'; done
for i in 1 2 3 4; do sudo docker exec ric5g-ue-cell1-$i pkill -f "iperf3 -s"; done
sleep 2
for i in 1 2 3 4; do sudo docker exec -d ric5g-ue-cell1-$i sh -c 'while true; do iperf3 -s -1 >> /tmp/srv.txt 2>&1; sleep 1; done'; done
sleep 3
for i in 1 2 3 4; do echo -n "ue-$i "; sudo docker exec ric5g-ue-cell1-$i pgrep iperf3 || echo "NO iperf3 server"; done
```

On the core node set the four RNTIs and the four addresses it printed (RNTIs in hexadecimal,
container 1 to 4) in **every** terminal you use for the runs. An empty `$U1` makes a command such
as `assoc $U1 1` fail without a visible error; the version counter on the cell then stays one
too low. For example:

```bash
U1=56a2; U2=34b1; U3=b38a; U4=8f45
IP1=12.1.1.2; IP2=12.1.1.3; IP3=12.1.1.4; IP4=12.1.1.5
echo "[$U1] [$U2] [$U3] [$U4] [$IP1] [$IP2] [$IP3] [$IP4]"   # none may be empty
```

The helper scripts (create them once on the core node, if you do not have them):

```bash
cat > ~/x.sh <<'EOF'
#!/bin/bash
cd /opt/oai-src/openair2/E2AP/flexric && ./build/examples/xApp/c/ctrl/xapp_sched_ctrl "$@" | tail -1
EOF
cat > ~/dl.sh <<'EOF'
#!/bin/bash
# usage: [RATE=15M] [DUR=75] dl.sh <run-name> [ue-ip ...]   (default: the four UEs, 15 Mbit/s, 75 s)
RUN=$1; shift
RATE=${RATE:-15M}; DUR=${DUR:-75}
IPS=${@:-12.1.1.2 12.1.1.3 12.1.1.4 12.1.1.5}
for ip in $IPS; do
  sudo docker exec -d ric5g-oai-ext-dn sh -c "iperf3 -c $ip -u -b $RATE -t $DUR -i 5 --get-server-output > /tmp/$RUN-$ip.txt 2>&1"
done
EOF
cat > ~/chk.sh <<'EOF'
#!/bin/bash
# usage: chk.sh <run-name>   last line of each client log; a client that failed shows an iperf3 error
sleep 5
sudo docker exec ric5g-oai-ext-dn sh -c "for f in /tmp/$1-*.txt; do echo \"\$f: \$(tail -n 1 \$f)\"; done"
EOF
chmod +x ~/x.sh ~/dl.sh ~/chk.sh
```

The default list of `dl.sh` is the four UEs, whatever their order. For a subset use the
variables, for example `~/dl.sh q3 $IP1 $IP4`. One iperf3 server takes one test at a time: a new
client started while the previous test of the same UE is still running fails ("server is busy"
or "control socket has closed unexpectedly"). `chk.sh` prints the last line of each client log;
iperf3 buffers its output when it writes to a file, so an **empty** line means no error so far and
an error shows as text. The `map` command of `sched-ctl.py` cannot tell the UEs apart while data
from a previous run is still being delivered; read the RNTIs soon after the cell is up.

## 3. The runs

For each run: paste the **core block**, wait for `=== TRAFFIC STARTED ===`, then paste the
**cell block**. Pasting the cell block earlier measures commands that are still being applied
(`sched-ctl.py` says so on the version line).

Cell block, the same for every run:

```bash
cd /local/repository
sleep 10; python3 bin/sched-ctl.py status | head -24; python3 bin/sched-ctl.py measure --seconds 20
```

Every core block starts with the following. The queues from the previous run take minutes to
drain on the radio side (the cell stayed busy for about 220 s after a 75 s run), so a run does not
start from an empty cell however long you wait; the policy must not depend on that, see run 7.

```bash
while sudo docker exec ric5g-oai-ext-dn pgrep iperf3 > /dev/null; do echo "previous run still draining"; sleep 10; done
sleep 30
~/x.sh reset
```

### Run 0: stock policy unchanged

Core block, after the start above, then `~/dl.sh q0` and the marker:

```bash
~/dl.sh q0
~/chk.sh q0
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

Expect `policy=pf` and about 25 % per UE, as before. This shows the new image does not change
the stock policy.

### Run 1: two slices of two UEs, weights 3000 : 1000

```bash
~/x.sh policy slice
~/x.sh slice-weight 1 3000
~/x.sh slice-weight 2 1000
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 1; ~/x.sh assoc $U3 2; ~/x.sh assoc $U4 2
~/dl.sh q1
~/chk.sh q1
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

Expect the slice rows of `measure` at about 75 : 25 %, the two UEs of a slice about equal,
and `status` listing slice 1 with weight 3000 and slice 2 with weight 1000, each with 2 UEs,
every UE with `via rnti`.

### Run 2: three UEs against one, same weights

```bash
~/x.sh policy slice
~/x.sh slice-weight 1 3000
~/x.sh slice-weight 2 1000
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 1; ~/x.sh assoc $U3 1; ~/x.sh assoc $U4 2
~/dl.sh q2
~/chk.sh q2
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

Expect again about 75 : 25 % between the slices, with the three UEs of slice 1 sharing their
75 % equally. Run 3 shows the property that matters: with per-UE weights of 1000, three UEs
against one also give 75 : 25, but it falls to 50 : 50 as soon as two of the three stop.

### Run 3: the same, with two UEs of slice 1 sending nothing

Same setup as run 2 (the setup lines are repeated so the counters start clean); traffic only
to `U1` and `U4`:

```bash
~/x.sh policy slice
~/x.sh slice-weight 1 3000
~/x.sh slice-weight 2 1000
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 1; ~/x.sh assoc $U3 1; ~/x.sh assoc $U4 2
~/dl.sh q3 $IP1 $IP4
~/chk.sh q3
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

Expect `U1` about 75 % and `U4` about 25 %, and the other two UEs close to 0.

### Run 4: weights of the UEs inside a slice

```bash
~/x.sh policy slice
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 1; ~/x.sh assoc $U3 2; ~/x.sh assoc $U4 2
~/x.sh weight $U1 3000
~/x.sh weight $U2 1000
~/dl.sh q4
~/chk.sh q4
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

Slice weights are left at the default 1000, so expect the slices at about 50 : 50 %, `U1` about
37.5 %, `U2` about 12.5 %, `U3` and `U4` about 25 % each.

### Run 5: slice cap

```bash
~/x.sh policy slice
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 1; ~/x.sh assoc $U3 2; ~/x.sh assoc $U4 2
~/x.sh slice-cap 1 20
~/dl.sh q5
~/chk.sh q5
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

The cap is a limit on what the two UEs of slice 1 take together in a slot, so expect slice 1 at
about 19 % (20 of about 105 resource blocks in every slot it leads, which is every slot because it
stays behind), slice 2 about 81 %, `U1` and `U2` about 9.5 % each, `U3` and `U4` about 40 % each,
and the resource blocks per scheduler call still close to 104. (The first version applied the cap
to each UE separately and gave slice 1 31.9 %, with 95.1 resource blocks per call; see section 5.)

### Run 6: membership by S-NSSAI (checks what the MAC reports; no change of the core)

All four UEs attach with SST 1 and no SD, so one rule moves all of them:

```bash
~/x.sh policy slice
~/dl.sh q6
sleep 5
~/x.sh nssai-map 1 none 2
~/x.sh assoc $U1 1
~/chk.sh q6
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

Expect `status` to show a rule `S-NSSAI sst=1 sd=none -> slice 2`, every UE with `nssai 1:ffffff`,
`U1` in slice 1 `via rnti` and the others in slice 2 `via nssai`. If the NSSAI column shows `-` or
`0:ffffff` the MAC does not report the bearer's S-NSSAI and membership by S-NSSAI (step B of
`SLICING_DESIGN.md`) cannot work as designed; record that and stop there.

### Run 7: credit of a slice that sends little (regression of the first live session)

Two slices of equal weight, `U1` alone in slice 1. For 60 s `U1` receives 1 Mbit/s and the other
three 15 Mbit/s each, so slice 1 is backlogged in only a small fraction of the calls and never
idle for long. Then all four receive 15 Mbit/s. The first version of the policy let slice 1 collect
credit during the first phase and took about 75 % of the cell in the second (run 6c); with the
credit bounded at 2000 resource blocks the second phase must start at the weights.

```bash
while sudo docker exec ric5g-oai-ext-dn pgrep iperf3 > /dev/null; do echo "previous run still draining"; sleep 10; done
sleep 30
~/x.sh reset
~/x.sh policy slice
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 2; ~/x.sh assoc $U3 2; ~/x.sh assoc $U4 2
RATE=1M DUR=60 ~/dl.sh q7a $IP1
DUR=60 ~/dl.sh q7b $IP2 $IP3 $IP4
~/chk.sh q7a; ~/chk.sh q7b
echo "=== phase 1 started: waiting 65 s ==="
sleep 65
while sudo docker exec ric5g-oai-ext-dn pgrep iperf3 > /dev/null; do sleep 5; done
~/dl.sh q7c
~/chk.sh q7c
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

Expect, in the window 10 s to 30 s after the second phase starts: slice rows 50 / 50 (within
about 2 points), `U1` about 50 % and the others about 16.7 % each, the policy-grants line equal
to the slice rows. With the unbounded credit the slice rows would read about 75 / 25.
To see the first phase as well, run `sched-ctl.py measure --seconds 20` on cell1 during it. The one
window taken showed slice 1 at 21.9 % of the resource blocks for 1.12 Mbit/s (expected under 10 %;
the window probably fell into the ramp-up of the traffic, total 7.58 Mbit/s), so that premise is not
confirmed by a measurement; the counters afterwards show it (see section 6).

### Run 8: a UE that starts sending late (regression of the UE-level credit)

Policy `wrr`, all four UEs with the default weight. `U1`, `U2` and `U3` receive 15 Mbit/s for 75 s;
`U4` receives nothing for the first 60 s and then 15 Mbit/s. Its grant counter is then about
60 s x 18,900 resource blocks per second / 3 = 380,000 behind the others. The image of section 6
(no bound on the credit of a UE) serves `U4` in nearly every slot until that gap is closed, about
20 s; the image of section 7 gives it a burst of at most 2000 resource blocks and then its share.

```bash
while sudo docker exec ric5g-oai-ext-dn pgrep iperf3 > /dev/null; do echo "previous run still draining"; sleep 10; done
sleep 30
~/x.sh reset
~/x.sh policy wrr
~/dl.sh q8a $IP1 $IP2 $IP3
~/chk.sh q8a
echo "=== phase 1 started: waiting 60 s ==="
sleep 60
DUR=60 ~/dl.sh q8b $IP4
~/chk.sh q8b
echo "=== U4 STARTED: paste the cell1 block now (it measures from 5 s after the start of U4) ==="
```

On cell1, at the moment the last line appears on the core:

```bash
sleep 5; python3 bin/sched-ctl.py measure --seconds 20
```

Expect, in the 20 s window: `U4` about 25 % of the resource blocks (within 3 points), the other
three about 25 % each. The grant counters in `status` of `U4` stay far below those of the others;
that is correct, they count real grants. With an unbounded credit `U4` would read 50 % or more in
this window. If the iperf3 client of `U4` finishes before the window ends, the window is invalid
(use `DUR=75`).

### After the runs

```bash
~/x.sh reset
```

Paste the cell-block outputs of all runs. A run is valid only when the version on the
`window=` line is the same at both ends and there is no "changed during the window" note.

## 4. What this does and does not test

It tests the policy, the commands over E2 and the read-back on a live gNB, with membership by
RNTI and the S-NSSAI the MAC reports. It does not test two different S-NSSAIs in the core
(configuration of the gNB, AMF, SMF, UPF, subscribers and UEs), the standard FlexRIC slice
message, throughput guarantees, or a UE that attaches or reconnects during a run (run 8 covers a UE that starts sending late, not a new attachment). Results of the earlier policies show that the total
throughput of a run varies by about ±10 % at the same resource-block rate, so judge by the
resource-block shares.

## 5. Results of the first live session

Single cell, RFsim, four UEs, all with S-NSSAI SST 1 without SD, 15 Mbit/s of UDP offered to each
UE. Image `ghinwa555/oai-gnb-ctrl:70508eba-f5d66d814b12`, built from the first version of the slice
policy (idle rule of 500 calls, cap applied to each UE of the slice). Shares are shares of the
resource blocks the MAC counted in a 20 s window, 10 s after the commands; every window below has
the same version at both ends. The cell carried 12.1 to 13.7 Mbit/s in total in every run.

| Run | Setup | Expected | Measured | Verdict |
| --- | --- | --- | --- | --- |
| 0 | `pf`, nothing else | 25 % per UE | 24.0 to 25.6 %; 104.9 RBs per call | as expected |
| 1, first attempt | slice 1 = `U1`,`U2`, slice 2 = `U3`,`U4`; weights 3000 : 1000 | 75 : 25 | 69.5 : 30.5; UEs inside a slice equal | **not as expected**; not reproduced, cause not found |
| 1, repeated | the same, 30 s wait before `reset` | 75 : 25 | 75.0 : 25.0 (37.5 / 37.5 / 12.5 / 12.5); policy-side 75.0 : 25.0 | as expected |
| 3 | slice 1 = `U1`,`U2`,`U3` with only `U1` sending; slice 2 = `U4`; 3000 : 1000 | `U1` 75, `U4` 25 | 75.0 and 25.0; `U2`, `U3` 10 RBs each | as expected |
| 4 | UE weights 3000 and 1000 in slice 1; slice weights equal | 37.5 / 12.5 / 25 / 25 | exactly that; slices 50.0 : 50.0 | as expected |
| 5 | slice cap 20 on slice 1 (two UEs); weights equal | about 19 % for slice 1 | 31.9 %; 95.1 RBs per call; 2214 refused grants in the window | **not as expected**: the cap limited each UE, not the slice |
| 6 | rule `sst=1 sd=none -> 2` applied 5 s into the traffic | all UEs `via nssai` in slice 2 | as expected; slice 2 100 %, 25.0 % per UE; the last command (`assoc`) was not applied, probably an empty `$U1` | rule works; second part repeated in 6b |
| 6b | the same rule plus `assoc $U1 1` 5 s into the traffic | `U1` in slice 1 `via rnti`, 50 % | `U1` 50.0 %; slices 50.0 : 50.0; the three others 15.5 / 19.0 / 15.5 % instead of 16.7 each | RNTI association beats the rule; the spread inside slice 2 is not explained (over the 3 minutes before the window the policy and MAC counters of the three agree to the resource block) |
| 6c | the same state, traffic repeated | 50 : 50 | **74.7 : 25.3**; `U3` 0.4 % because its iperf3 client failed | **not as expected**: unbounded slice credit |
| 2 | 3 UEs against 1 | 75 : 25 | not run | it adds little to run 1 (per-UE weights also give 75 : 25 there) |

What was established:

- The commands arrive and are read back: `status` showed the weights, caps, associations and rules
  that were set, with the version counter as predicted.
- The MAC reports the S-NSSAI of the bearer (`1:ffffff` for every UE), so membership by S-NSSAI
  works with the values the gNB provides. Two different S-NSSAIs in the core have not been tried.
- The policy keeps shares at the slice weights and independent of the number of backlogged UEs
  (runs 1 repeated, 3, 4), and the policy-side counters equal the MAC counters.

Two defects, both from the first version of the policy and both corrected in the revision of this
branch (bounded credit, slice-level cap), which has been tested on the host and not yet run live:

- **Credit.** In run 6c the counter of slice 1 was 1,035,561 resource blocks behind that of slice 2
  at equal weights, so slice 1 took the slot until it had caught up (about 75 % for the whole
  run). The gap had built up in the preceding minutes, during which slice 1 had less data than
  slice 2. The idle rule only acted after 500 consecutive calls without data and never applied.
  Run 7 reproduces this on purpose.
- **Cap.** In run 5 each UE of slice 1 took up to the cap in the same slot, so the slice could take
  `n` times the cap, and the second and third grants of the slot ran into the PDCCH/PUCCH limit:
  95.1 resource blocks per call instead of 104.

Open points:

- The first attempt of run 1 (69.5 : 30.5) was not reproduced and the cause is not known. Its
  `status` showed equal slice deficits at the start of the window, so a credit gap like the one of
  run 6c is not the explanation. Slice 0 also showed grants made before the `assoc` commands in
  runs 3, 5 and 6 (the cell is still serving the UEs from the run before), and those runs were
  not affected, so that is not the explanation either.
- In run 6b the three UEs of slice 2 had 15.5 / 19.0 / 15.5 % in the window although their
  counters agreed over the preceding minutes. Not repeated.
- The iperf3 server of one UE container stopped accepting after a run (run 6c, `U3`). The
  procedure now restarts the servers and `chk.sh` shows the failure at once.
- The queues of the previous run keep the cell busy for minutes (about 220 s after a 75 s run),
  so every run starts with data in the buffers. Some values in `status` therefore include grants
  made before the run's commands: use the per-UE counters and the `measure` lines. (With the
  image of section 7 the slice counters are real grants too; on the image of section 6 they also
  contain the credit rule's adjustments and are not a count of grants.)

## 6. Results on the revised image

Image `ghinwa555/oai-gnb-ctrl:70508eba-e4382b42bb15` (built on the cell node as
`ric5g/oai-gnb-ctrl:70508eba-e4382b42bb15`): credit of a slice bounded at 2000 resource blocks, slice
cap as a limit on the whole slice. Same reservation type and method as section 5. In every window the
version was the same at both ends, and the policy-side line equals the MAC-side slice rows unless
stated.

| Run | Setup | Expected | Measured | Verdict |
| --- | --- | --- | --- | --- |
| 7 | `U1` alone in slice 1 at 1 Mbit/s for 60 s, then all four at 15 Mbit/s; equal slice weights | 50 : 50 | **50.0 : 50.0**; `U1` 50.0 %, the others 16.7 % each (62,246 / 62,311 / 62,239 RBs) | pass; the credit rule had raised the counter of slice 1 by about 1.6 million RBs |
| 5 | slice cap 20 on slice 1 (two UEs), weights equal | about 19 % | **19.2 : 80.8**; UEs 9.6 / 9.6 / 40.4 / 40.4 %; 104.2 RBs per call; 167 refused grants (2214 with the first version) | pass |
| 1 | 2 + 2 UEs, weights 3000 : 1000 | 75 : 25 | 74.0 : 26.0 (UEs 37.0 / 37.0 / 13.0 / 13.0) | about 1 point short, cause not found |
| 3 | 3 UEs against 1 with two silent, 3000 : 1000 | `U1` 75, `U4` 25 | `U1` 74.9, `U4` 25.1, silent UEs 15 RBs each | pass |
| 1, repeated | as run 1 | 75 : 25 | **75.0 : 25.0** (37.5 / 37.5 / 12.5 / 12.5) | pass; the 74.0 did not recur |
| 4 | UE weights 3000 and 1000 in slice 1 | 37.5 / 12.5 / 25 / 25 | `U1` received no traffic (its iperf3 server had crashed): invalid | invalid; the slices stayed at 50 : 50 |
| 4, repeated | as run 4, servers restarted | 37.5 / 12.5 / 25 / 25 | **37.5 / 12.5 / 25.0 / 25.0**; slices 50.0 : 50.0 | pass |
| 6 | rule `sst=1 sd=none -> 2` applied 5 s into the traffic | all four in slice 2 `via nssai` | exactly that; 25.0 % each | pass |
| 6b | the rule plus `assoc U1 1` | `U1` in slice 1 `via rnti` at 50 %, the others 16.7 % | `U1` 50.0 %, slices 50.0 : 50.0; the three UEs of slice 2 had 6.5 / 7.1 / 36.4 % | pass at slice level; see below |
| 6c | the same state, traffic repeated (74.7 : 25.3 with the first version) | 50 : 50 | **50.0 : 50.0**, `U1` 50.0 %; slice 2: 4.9 / 3.8 / 41.3 % | pass at slice level; see below |

What this establishes: the commands, the weights of slices and of UEs, the caps, membership by
RNTI and by the S-NSSAI the MAC reports (rule, and association over rule), and the bound on the
credit of a slice, all on a live single-cell gNB with four UEs. The slice shares were within
0.2 point of the target in eight of the nine valid windows and 1.0 point off in one (run 1).

**A finding, corrected in the image of section 7: the credit of a single UE was not bounded.** Between runs 6
and 6b the UE of `ric5g-ue-cell1-2` re-established its connection without a container restart (same
address, new RNTI: `0x8d99` became `0x874e`). A new UE starts with a grant counter of 0 while its
peers were at about 1.2 million resource blocks, so inside its slice it was served first until it
had caught up: 36.4 % and 41.3 % of the cell in two consecutive windows, against 6.5 and 3.8 to
4.9 % for its peers. The gap closed within the third window. The slice split was not affected. The
same holds for `wrr`, whose counters have the same property, and for any UE that attaches during a
run. The remedy is the same as for slices (a UE that becomes backlogged enters at the level of
its backlogged peers, less a bounded allowance); see section 7.
Also seen in these windows and not explained: `0xf85a` received 0.05 Mbit/s with 18,414 resource
blocks in the 6c window, and `0x874e` 0.46 Mbit/s with 137,321 resource blocks in the 6b window.

Other points:

- The iperf3 server in a UE container crashed twice (`Segmentation fault`, then `Connection
  refused` or `control socket has closed unexpectedly` for the client). The loop in section 2
  restarts it after every test. This is probably also why the iperf3 summaries were missing in
  all earlier runs.
- Four minutes after the last run, two UEs were still receiving about 1.8 MB per probe interval:
  the queues of a 75 s run at 15 Mbit/s per UE take minutes to drain.

## 7. Image with bounded credit for UEs

Image `ghinwa555/oai-gnb-ctrl:70508eba-31ef394cea46`: the credit of a UE is bounded at 2000
resource blocks against the other UEs with data in its slice (all UEs for `wrr`), and the credit is
kept apart from the grant counters, so `granted_rbs` of UEs and slices are real grants. Tested on the
host (`gnb/test`: 203,446 checks, address and thread sanitizers, strict C11, and the compile with
the OAI flags). The live runs below are to be done and their results added here: run 8, then
repeats of runs 7, 5, 1 and 6c to show that nothing else changed.
