# Live test of slice-aware scheduling

Procedure and results of the live runs of the `slice` policy on a reservation with four UEs (run 9: two UEs)
(branch `slicing`). Section 1 deploys the branch, section 2 prepares the variables and helper
scripts, section 3 lists the runs with the expected values, section 4 holds the results.

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

For each run: paste the core block, wait for `=== TRAFFIC STARTED ===`, then paste the cell block
(earlier, it measures commands that are still being applied).

Cell block, the same for every run:

```bash
cd /local/repository
sleep 10; python3 bin/sched-ctl.py status | head -24; python3 bin/sched-ctl.py measure --seconds 20
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
sleep 62; python3 bin/sched-ctl.py measure --seconds 20; python3 bin/sched-ctl.py status | head -9
```
Expect about 25 % for each UE, `U4` included. The counters of `U4` stay far below the others; they
count real grants.

**Run 9, two S-NSSAIs in the core.** Needs a reservation with `second_slice` on. It was run with two UEs:
`U1` attaches with SST 1 and `U2` with SST 2 (with four UEs, UEs 1 and 2 have SST 1 and UEs 3 and 4 SST 2).

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

At the end of the reservation: `~/x.sh reset`.

## 4. Results

Image `ghinwa555/oai-gnb-ctrl:70508eba-31ef394cea46`, single cell, RFsim, AWGN, 20 s windows; runs 0 to 8
with four UEs with SST 1 and no SD, run 9 with two UEs (SST 1 and SST 2). In every window the version was the
same at both ends and every UE received traffic.

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

Limits: one 20 s window per run; the total throughput of a run varies by about 10 % at the same
resource-block rate, so judge by the resource-block shares; RFsim with the same channel for all UEs.
Not tested on the cell: a UE that attaches or reconnects with a new RNTI during a run (run 8
covers a UE that starts sending late; the host tests cover the new RNTI), more than one UE per
S-NSSAI in the core (run 9 had one), and the standard FlexRIC slice message.
