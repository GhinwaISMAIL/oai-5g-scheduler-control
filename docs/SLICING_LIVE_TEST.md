# Live test of slice-aware scheduling

Procedure and results of the live runs of the `slice` policy on a reservation with four UEs (run 9: two UEs)
(branch `slicing`). Section 1 deploys the branch, section 2 prepares the variables and helper
scripts, section 3 lists the runs with the expected values, section 4 holds the results.

## 1. Bring the reservation to the `slicing` branch

The sources of the gNB image changed, so the image has to be built or pulled and the xApp on
the core node rebuilt. The image of the current commit is on the registry
(`ghinwa555/oai-gnb-ctrl:70508eba-3cc4a7aa00f0`), so a new reservation pulls it in a minute.

**Reservation started from the `slicing` branch** (the profile was instantiated from it): the
nodes already run this code. The core builds the xApp by itself. The cell node pulls
`gnb_ctrl_image`. If the pull fails (a tag that was never pushed, a private repository), it builds the
image itself, about 25 minutes, and `setup.log` says so;
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
python3 bin/sched-ctl.py status
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

For each run: paste the core block, wait for `=== TRAFFIC STARTED ===`, then paste the cell block
(earlier, it measures commands that are still being applied).

Cell block, the same for every run:

```bash
cd /local/repository
sleep 10; python3 bin/sched-ctl.py status; python3 bin/sched-ctl.py measure --seconds 20
```

Every core block starts with the following (the queues of the previous run take minutes to drain,
so a run never starts from an empty cell; the policy must not depend on that):

```bash
while sudo docker exec ric5g-oai-ext-dn pgrep iperf3 > /dev/null; do echo "previous run still draining"; sleep 10; done
sleep 30
~/x.sh reset
```

and ends with:

```bash
~/chk.sh <run>     # the lines must be empty; any text is an iperf3 error and the run is invalid
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

A run is valid only if the version on the `window=` line is the same at both ends.

**Run 0, stock policy.** `~/dl.sh q0`. Expect `policy=pf` and about 25 % per UE.

**Run 1, slice weights 3000 : 1000.**
```bash
~/x.sh policy slice; ~/x.sh slice-weight 1 3000; ~/x.sh slice-weight 2 1000
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 1; ~/x.sh assoc $U3 2; ~/x.sh assoc $U4 2
~/dl.sh q1
```
Expect slices 75 : 25 and UEs 37.5 / 37.5 / 12.5 / 12.5 %.

**Run 4, UE weights inside a slice.**
```bash
~/x.sh policy slice
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 1; ~/x.sh assoc $U3 2; ~/x.sh assoc $U4 2
~/x.sh weight $U1 3000; ~/x.sh weight $U2 1000
~/dl.sh q4
```
Expect slices 50 : 50 and UEs 37.5 / 12.5 / 25 / 25 %.

**Run 5, slice cap.**
```bash
~/x.sh policy slice
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 1; ~/x.sh assoc $U3 2; ~/x.sh assoc $U4 2
~/x.sh slice-cap 1 20
~/dl.sh q5
```
Expect slice 1 at about 19 % (20 of about 105 resource blocks per slot), slice 2 about 81 %, and
about 104 resource blocks per scheduler call.

**Run 6, membership by S-NSSAI.** All UEs attach with SST 1 and no SD, so one rule moves them all.
```bash
~/x.sh policy slice
~/dl.sh q6
sleep 5
~/x.sh nssai-map 1 none 2
~/x.sh assoc $U1 1
```
Expect the rule `sst=1 sd=none -> slice 2`, every UE with `nssai 1:ffffff`, `U1` in slice 1 `via rnti`
at 50 %, the others in slice 2 `via nssai` at about 16.7 % each.

**Run 7, credit of a slice that sends little.** `U1` alone in slice 1 receives 1 Mbit/s for 60 s,
the others 15 Mbit/s; then all four receive 15 Mbit/s. With unbounded credit slice 1 would take about
75 % of the second phase.
```bash
~/x.sh policy slice
~/x.sh assoc $U1 1; ~/x.sh assoc $U2 2; ~/x.sh assoc $U3 2; ~/x.sh assoc $U4 2
RATE=1M DUR=60 ~/dl.sh q7a $IP1
DUR=60 ~/dl.sh q7b $IP2 $IP3 $IP4
~/chk.sh q7a; ~/chk.sh q7b
sleep 65
while sudo docker exec ric5g-oai-ext-dn pgrep iperf3 > /dev/null; do sleep 5; done
~/dl.sh q7c
```
Expect slices 50 : 50, `U1` about 50 % and the others about 16.7 % each.

**Run 8, credit of a UE that starts sending late.** Policy `wrr`; `U1` to `U3` receive 15 Mbit/s for
100 s, `U4` starts after 60 s. Without a bound on the credit of a UE, `U4` would take nearly every
slot for about 20 s.
```bash
~/x.sh policy wrr
DUR=100 ~/dl.sh q8a $IP1 $IP2 $IP3
~/chk.sh q8a
echo "=== phase 1 started: start the cell1 command NOW, U4 starts in 60 s ==="
sleep 60
DUR=60 ~/dl.sh q8b $IP4
~/chk.sh q8b
```
On cell1, as soon as the first marker appears:
```bash
cd /local/repository
sleep 62; python3 bin/sched-ctl.py measure --seconds 20; python3 bin/sched-ctl.py status
```
Expect about 25 % for each UE, `U4` included. The counters of `U4` stay far below the others; they
count real grants.

**Run 9, two S-NSSAIs in the core.** Needs a reservation with `second_slice` on. It was run with two UEs:
`U1` attaches with SST 1 and `U2` with SST 2; with four UEs, UEs 1 and 2 have SST 1 and UEs 3 and 4 SST 2
(`dl.sh q9` then takes all four addresses, and the expected shares are 37.5 / 37.5 / 12.5 / 12.5 %).

First check the attachment: both UEs must have a tunnel address (`ip -4 addr show oaitun_ue1` in the UE
container) and answer a ping through it. If one does not, the core did not accept the second slice: look at
`sudo docker logs ric5g-oai-amf` and `ric5g-oai-smf` for `NSSAI` and stop there; that is the finding.

Part 1, membership by S-NSSAI:
```bash
~/x.sh policy slice
~/x.sh nssai-map 1 none 1; ~/x.sh nssai-map 2 none 2
~/x.sh slice-weight 1 3000; ~/x.sh slice-weight 2 1000
~/dl.sh q9 $IP1 $IP2
```
On cell1, after 10 s: `python3 bin/sched-ctl.py status` and `measure --seconds 20`. Expect `U1` in slice 1 with
`nssai 1:ffffff` and `U2` in slice 2 with `nssai 2:ffffff`, both `via nssai`, and shares of 75 : 25.
The `slice`, `via` and `nssai` columns of `status` appear once a slice weight or a rule is set.

Part 2, rule deleted during traffic: `sched-ctl.py nssai-map 2 none -` on cell1 while `dl.sh` is running.
Expect `U2` in slice 0 `via default`, still with `nssai 2:ffffff`, and the shares unchanged (slice 0 has
weight 1000, like slice 2). Restoring the rule (`nssai-map 2 none 2`) moves `U2` back to slice 2.

**Run 10, credit of two UEs that return together.** Policy `wrr`; `U1` and `U2` receive 15 Mbit/s for
100 s, `U3` and `U4` start after 60 s. Before the credit bound compared against the furthest-ahead UE, the two
late UEs took about 85 % of the cell for a long time.
```bash
~/x.sh policy wrr
DUR=100 ~/dl.sh q10a $IP1 $IP2
~/chk.sh q10a
echo "=== phase 1 started: start the cell1 command NOW, U3 and U4 start in 60 s ==="
sleep 60
DUR=60 ~/dl.sh q10b $IP3 $IP4
~/chk.sh q10b
```
On cell1, as soon as the first marker appears:
```bash
cd /local/repository
sleep 62; python3 bin/sched-ctl.py measure --seconds 20; python3 bin/sched-ctl.py status
```
Expect about 25 % for each UE.

**Run 11, standard slice message.** The stock slice xApp of FlexRIC sends an ADD, a DEL and an association
and reads the indication. On the core node:
```bash
~/x.sh policy slice
X=$(find /opt/oai-src -name xapp_slice_moni_ctrl -type f | head -1); echo $X
$X
```
On cell1, in a second terminal, before starting the xApp: `sudo docker logs -f ric5g-gnb-cell1 2>&1 | grep --line-buffered "SLICE CONTROL"`.
The handler flushes each of these lines; an image built before that change keeps them in the block buffer
of the container's output, and `status` with the version counter is then the only check.
Expect in the gNB log:
- `SLICE CONTROL ADD refused: static slice width must be 5..275 resource blocks, slice (0)`: the example's slices are 4, 4 and 5 resource blocks wide, so the whole message is refused and nothing changes;
- `SLICE CONTROL DEL applied (1 commands)` (slice 2);
- `SLICE CONTROL ASSOC applied (1 commands)`: the xApp takes the RNTI of the first UE from the indication, which now holds the real table, and puts it in slice 5.

Then `python3 bin/sched-ctl.py status` on cell1 shows that UE in slice 5 `via rnti`, and the version has
gone up by one for the policy, one for the DEL and one for the association (the refused ADD does not change it).
The stock example cannot send a valid ADD; run 13 does, with `xapp_sched_ctrl`.

**Run 12, three slices with traffic.** Slices 1 and 2 come from the S-NSSAI rules (as in run 9), slice 3 from
an association that overrides the rule. With the four UEs of run 9, `U4` (SST 2) is moved to slice 3, so the
three slices hold two, one and one UE.
```bash
~/x.sh reset
~/x.sh policy slice
~/x.sh nssai-map 1 none 1; ~/x.sh nssai-map 2 none 2
~/x.sh slice-weight 1 3000; ~/x.sh slice-weight 2 1000; ~/x.sh slice-weight 3 2000
~/x.sh assoc $U4 3
~/dl.sh q12
~/chk.sh q12
```
On cell1, after 10 s: `python3 bin/sched-ctl.py status` and `measure --seconds 20`. Expect slices 50.0 : 16.7 : 33.3
and UEs 25.0 / 25.0 / 16.7 / 33.3 %; `U4` in slice 3 `via rnti` with its `nssai 2:ffffff`, the others `via nssai`.
The version counts every command: 8 more than before the `reset` plus the `reset` itself.

**Run 13, standard slice message from `xapp_sched_ctrl`.** Needs the xApp with the `sm-` verbs on the core node:
```bash
cd /local/repository && sudo git pull --ff-only
sudo python3 bin/patch-flexric-sched-ctrl.py /opt/oai-src/openair2/E2AP/flexric
cd /opt/oai-src/openair2/E2AP/flexric/build && sudo make xapp_sched_ctrl 2>&1 | tail -n 3
./examples/xApp/c/ctrl/xapp_sched_ctrl bogus 2>&1 | grep -c "sm-"
```
Expect the build to end with `Built target xapp_sched_ctrl` and the last command to print `6` (the usage lists the
`sm-` verbs). Only the xApp changed; the gNB image stays.

On cell1, in a second terminal, before the first command: `sudo docker logs -f ric5g-gnb-cell1 2>&1 | grep --line-buffered "SLICE CONTROL"`.
On the core node, with `U1` to `U4` as in section 2 (RNTIs without `0x`):
```bash
X=/opt/oai-src/openair2/E2AP/flexric/build/examples/xApp/c/ctrl/xapp_sched_ctrl
~/x.sh reset
~/x.sh policy slice
$X sm-get                                       # before: policy slice, no slice, the four UEs in slice 0
$X sm-add nvs 1:0.75 2:0.25
$X sm-assoc $U1:1 $U2:1 $U3:2 $U4:2
$X sm-get                                       # after
~/dl.sh q13
~/chk.sh q13
```
Expect in the gNB log `SLICE CONTROL ADD applied (4 commands)` and `SLICE CONTROL ASSOC applied (4 commands)`.
Both `sm-get` outputs end with `Slice indication read`; the second lists slices 1 and 2 with `pct_reserved 0.750` and
`0.250` and the slice of every UE. On cell1, after 10 s: `python3 bin/sched-ctl.py status` and `measure --seconds 20`.
Expect slice weights 750 and 250 with cap 0, all four UEs `via rnti`, the version 9 higher than after the `reset`
(policy 1, ADD 4, ASSOC 4), and shares of 75 : 25 and 37.5 / 37.5 / 12.5 / 12.5 %.

Refused messages, with the traffic still running (each leaves the version and the shares unchanged):
```bash
$X sm-add nvs 1:1.5          # SLICE CONTROL ADD refused: NVS pct_reserved must be in (0,1], slice (1)
$X sm-add static 3:0-3       # SLICE CONTROL ADD refused: static slice width must be 5..275 resource blocks, slice (3)
$X sm-add nvs 9:0.5          # SLICE CONTROL ADD refused: slice id outside 0..7 (9)
$X sm-assoc 7777:1           # SLICE CONTROL ASSOC: "ASSOC 7777 1" refused: ERR unknown_rnti (0 of 1 commands applied)
```
The xApp still prints "Slice message delivered" for these: the acknowledgement only says that the gNB received
the message. On cell1, `measure --seconds 15` must show the same version at both ends and shares of 75 : 25.
The traffic of `dl.sh` ends after its duration; if `measure` reports 0.00 Mbit/s, start `dl.sh` again, because
the shares of an idle cell say nothing.

A static slice with a cap, with `U3` moved into it (the width is the minimum, 5 resource blocks, so the cap is easy to see):
```bash
$X sm-add static 3:0-4       # SLICE CONTROL ADD applied (2 commands)
$X sm-assoc $U3:3            # SLICE CONTROL ASSOC applied (1 commands)
```
On cell1, after 5 s: `status` lists slice 3 with weight 1000, cap 5 and one UE, and `U3` in slice 3 `via rnti`; the
version is 3 higher than before. In `measure`, the resource blocks of `U3` stay below 5 times the number of scheduler
calls of the window (calls per second times 15), against about 12.5 % of the cell without the cap. The capped slice takes
its few blocks first; the rest of the cell is shared between the other two slices in the ratio of their weights.

Clearing: `$X sm-del 3` removes one slice, `$X sm-add none` brings all slices back to their defaults and removes the
associations (`ADD applied (8 commands)`): `status` then shows neither the slice table nor the slice columns.

At the end of the reservation: `~/x.sh reset`.

## 4. Results

Single cell, RFsim, AWGN, 20 s windows. Runs 0 to 8 used the image `...:70508eba-31ef394cea46` with four UEs
with SST 1 and no SD, and run 9 with two UEs (SST 1 and SST 2). The credit bound was changed afterwards (it now
refers to the furthest-ahead other slice or UE instead of the furthest-behind); runs 9 (four UEs), 10 and 11
used `...:70508eba-dc6cb0734c96`, which has the new bound. The repeats of runs 9 and 11 and run 12 (below the
first four-UE rows) were made on a second reservation with the image pulled from the registry,
`ghinwa555/oai-gnb-ctrl:70508eba-3cc4a7aa00f0` (digest `sha256:7acce171...801b12`), which differs from
`dc6cb0734c96` only in the flushed log lines of the slice handler. Runs 0 to 9 with two UEs were not repeated with it. In every window the version was the same at both ends and every UE
received traffic.

| Run | Setup | Expected | Measured |
| --- | --- | --- | --- |
| 0 | `pf` | 25 % each | 25.4 / 25.7 / 23.6 / 25.3 % |
| 1 | slice weights 3000 : 1000 | 75 : 25 | **75.0 : 25.0**; UEs 37.5 / 37.5 / 12.5 / 12.5 % |
| 4 | UE weights 3000 / 1000 in slice 1 | 37.5 / 12.5 / 25 / 25 | **37.5 / 12.5 / 25.0 / 25.0**; slices 50.0 : 50.0 |
| 5 | slice cap 20 on slice 1 | about 19 : 81 | **19.4 : 80.6**; 103.1 RBs per call |
| 6 | rule `sst=1 sd=none -> 2` and `assoc U1 1` | `U1` 50 %, others 16.7 % | **50.0**; 16.6 / 16.7 / 16.7 %; `U1` `via rnti`, others `via nssai` |
| 7 | slice 1 sends 1 Mbit/s for 60 s, then all at 15 Mbit/s | 50 : 50 | **50.0 : 50.0**; `U1` 50.0 %, others 16.6 / 16.7 / 16.7 % |
| 8 | `wrr`, `U4` starts after 60 s | 25 % each | **25.0 / 25.0 / 25.0 / 25.0** %; `U4` at 122,996 granted RBs, the others about 530,000 |
| 9 | two UEs, SST 1 and SST 2, rules to slices 1 and 2, weights 3000 : 1000 | 75 : 25 | **74.7 : 25.3**; `nssai 1:ffffff` and `2:ffffff`, both `via nssai`; 17.47 and 6.62 Mbit/s |
| 9 | rule for SST 2 deleted during traffic | `U2` in slice 0 `via default`, 75 : 25 | `U2` slice 0 `via default`, `nssai 2:ffffff` kept, version 7 to 8; windows 66.7 : 33.3, then 74.7 : 25.3 |
| 9 | rule restored | `U2` in slice 2, 75 : 25 | **72.2 : 27.8** |
| 9 | four UEs, UEs 1 and 2 SST 1, UEs 3 and 4 SST 2, rules to slices 1 and 2, weights 3000 : 1000 | 75 : 25; 37.5 / 37.5 / 12.5 / 12.5 | **75.0 : 25.0**; **37.5 / 37.5 / 12.5 / 12.5** %; all `via nssai` with `nssai 1:ffffff` and `2:ffffff`; version 13 at both ends; slice counters equal the sums of their UEs (325,962 = 163,007 + 162,955; 110,273 = 55,114 + 55,159) |
| 10 | `wrr`, `U3` and `U4` start after 60 s | 25 % each | **25.0 %** each (92,724 / 92,705 / 92,724 / 92,677 RBs in the window); version 2 at both ends; the late UEs ended at about 230,000 granted RBs, the others at about 834,000 |
| 11 | stock slice xApp, policy `slice` | ADD refused, DEL and ASSOC applied, indication received | version 2 to 6 (reset, policy, DEL, ASSOC; the refused ADD changed nothing); one UE in slice 5 `via rnti`, the others in slice 0 `via default`; about 26 indications received, the xApp stopped cleanly |
| 9 | repeat on that image: slice weights 3000 : 1000, rules to slices 1 and 2 | 75 : 25; 37.5 / 37.5 / 12.5 / 12.5 | **75.0 : 25.0**; **37.5 / 37.5 / 12.5 / 12.5** %; all `via nssai` with `1:ffffff` and `2:ffffff`; version 11 at both ends; slice counters equal the sums of their UEs once the 5 RBs of slice 0 (a grant before the rules were set) are added; 8.08 Mbit/s in total |
| 11 | repeat: stock slice xApp | ADD refused, DEL and ASSOC applied | version 4 (reset, policy, DEL, ASSOC); `SLICE CONTROL ADD refused: static slice width must be 5..275 resource blocks, slice (0)`, `DEL applied (1 commands)` and `ASSOC applied (1 commands)` are in `docker logs`; one UE in slice 5 `via rnti`, the others in slice 0 `via default`; clean stop |
| 13 | `xapp_sched_ctrl`: `sm-add nvs 1:0.75 2:0.25`, `sm-assoc` of the four UEs, `sm-get` before and after | `ADD applied (4 commands)`, `ASSOC applied (4 commands)`; 75 : 25 | applied; **75.0 : 25.0**, UEs 37.5 / 37.5 / 12.5 / 12.5 %, all `via rnti`; the indication lists both slices and the slice of every UE; repeated on a clean start with the same result |
| 13 | four refused messages (NVS above 1, static width 4, slice id 9, unknown RNTI) under traffic | reason in the gNB log, nothing changed | all four reasons in `docker logs`; version and shares (75.0 : 25.0) unchanged |
| 13 | static slice `3:0-4` with `U3` associated to it | slice 3 weight 1000, cap 5; `U3` limited to 5 RBs per call | slice 3 weight 1000, cap 5, one UE; **`U3` 3.6 % of the RBs** (about 3.8 per call) instead of 12.5 %; the other two slices share the rest 75 : 25 |
| 13 | `sm-add static 3:0-9`, `sm-del 3`, `sm-add none` | applied; slices cleared | applied; `status` shows neither the slice table nor the slice columns |
| 12 | three slices: weights 3000 / 1000 / 2000; `U4` associated to slice 3 | 50.0 : 16.7 : 33.3; UEs 25.0 / 25.0 / 16.7 / 33.3 | **50.0 : 16.7 : 33.3** (182,701 / 60,872 / 121,813 RBs); UEs **25.0 / 25.0 / 16.7 / 33.3** %; `U4` slice 3 `via rnti`, the others `via nssai`; slice counters equal the sums of their UEs (106,742; 35,560; 71,101); version 19 at both ends; 13.31 Mbit/s in total |

Runs 0 to 8 are within 0.4 point of the target (run 0 is the stock policy). In runs 7, 4, 5, 1 and 6
the sum of the grant counters of the UEs equals the sum of the slice counters. The `granted_rbs`
columns count real grants; the credit that the bounds add is kept apart and only orders the UEs.

Not explained: in run 8 `U4` received 2.06 Mbit/s for the same resource blocks as the others
(about 3.35 Mbit/s); it did not recur in the other runs. In run 9 the first window after the rule
deletion gave 66.7 : 33.3 instead of 75 : 25; with the same slices and rule state in the next traffic
session the shares were 74.7 : 25.3 and 72.2 : 27.8, so it is not specific to slice 0. All windows that
fell short (66.7, 69.5, 72.2, 74.0) are below the target for the heavier slice; this is consistent with a
work-conserving scheduler giving the slot to the other UE while the heavier UE has little queued, but
it was not checked.

Limits: one 20 s window per run; the total throughput varies at the same resource-block rate (8.08 Mbit/s in
the repeat of run 9 and 13.31 Mbit/s in run 12, both at about 18,300 RBs/s, on the same reservation; not
explained), so judge by the resource-block shares; RFsim with the same channel for all UEs.
Run 10 was measured with the new credit bound only; the old bound was not run on the cell, so its 85 % is a
host-test figure. In run 11 the log lines of the handler did not appear in `docker logs` because the container
buffers the output of the gNB (see `DEPLOYMENT.md`); the refusal of the ADD and the effect of the DEL and the
association are therefore shown by the version counter and by `status` only. With the flush the three lines
were in `docker logs` after the repeat of run 11; whether each line appears at the moment it is printed was not
timed, because the log watch may have been started after the xApp ran.

Not tested on the cell: a UE that attaches or reconnects with a new RNTI during a run (run 8 and
run 10 cover UEs that start sending late; the host tests cover the new RNTI). The static cap of run 13 was measured at one width (5 resource blocks); other widths are covered by the host tests.
