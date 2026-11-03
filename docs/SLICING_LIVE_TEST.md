# Live test of slice-aware scheduling

Run on a reservation with four UEs. Everything below is for the `slicing` branch; the
results are not known yet. What each run should show is written down first, so that a
different result is a finding and not something to explain afterwards.

## 1. Bring the reservation to the `slicing` branch

The sources of the gNB image changed, so the image has to be built or pulled again and the
xApp on the core node rebuilt. On a running reservation, in this order.

On the core node:

```bash
cd /local/repository && sudo git fetch origin slicing && sudo git checkout slicing && sudo git pull --ff-only
sudo bash bin/node-setup.sh core 0 1 4 uniform AWGN >> /local/logs/setup.log 2>&1
cat /local/logs/CORE_READY
ls /opt/oai-src/openair2/E2AP/flexric/build/examples/xApp/c/ctrl/xapp_sched_ctrl
```

On cell1 (the build takes about 25 minutes; if the image from the profile default has been
pushed, `gnb/build-gnb-image.sh` is not needed and the profile pulls it):

```bash
cd /local/repository && sudo git fetch origin slicing && sudo git checkout slicing && sudo git pull --ff-only
sudo bash gnb/build-gnb-image.sh | tail -n 1
sudo GNB_CTRL=1 bash bin/node-setup.sh cell 1 1 4 uniform AWGN >> /local/logs/setup.log 2>&1
cat /local/logs/GNB_IMAGE /local/logs/CELL_READY
```

`GNB_IMAGE` must show a tag that ends with the hash of this checkout's `gnb/src` and
`gnb/patches` (`profile.py` and `tests/test_single_cell_profile.py` carry the same tag).

## 2. Variables and helper scripts

On cell1, read the RNTIs and map them to the containers:

```bash
cd /local/repository
python3 bin/sched-ctl.py status | head -8
sudo python3 bin/sched-ctl.py map --containers ric5g-ue-cell1-1,ric5g-ue-cell1-2,ric5g-ue-cell1-3,ric5g-ue-cell1-4
```

On the core node set the four RNTIs it printed (hexadecimal, container 1 to 4) in every
terminal you use for the runs, for example:

```bash
U1=56a2; U2=34b1; U3=b38a; U4=8f45
```

The helper scripts (create them once on the core node, if you do not have them):

```bash
cat > ~/x.sh <<'EOF'
#!/bin/bash
cd /opt/oai-src/openair2/E2AP/flexric && ./build/examples/xApp/c/ctrl/xapp_sched_ctrl "$@" | tail -1
EOF
cat > ~/dl.sh <<'EOF'
#!/bin/bash
# usage: dl.sh <run-name> [ue-ip ...]   (default: all four UEs)
RUN=$1; shift
IPS=${@:-12.1.1.2 12.1.1.3 12.1.1.4 12.1.1.5}
for ip in $IPS; do
  sudo docker exec -d ric5g-oai-ext-dn sh -c "iperf3 -c $ip -u -b 15M -t 75 -i 5 --get-server-output > /tmp/$RUN-$ip.txt 2>&1"
done
EOF
chmod +x ~/x.sh ~/dl.sh
```

`12.1.1.2` to `12.1.1.5` are the UEs of containers 1 to 4. iperf3 servers must be running
in the UE containers (`iperf3 -s`), as in the earlier runs.

## 3. The runs

For each run: paste the **core block**, wait for `=== TRAFFIC STARTED ===`, then paste the
**cell block**. Pasting the cell block earlier measures commands that are still being applied
(`sched-ctl.py` says so on the version line).

Cell block, the same for every run:

```bash
cd /local/repository
sleep 10; python3 bin/sched-ctl.py status | head -24; python3 bin/sched-ctl.py measure --seconds 20
```

Every core block starts with:

```bash
while sudo docker exec ric5g-oai-ext-dn pgrep iperf3 > /dev/null; do echo "previous run still draining"; sleep 10; done
~/x.sh reset
```

### Run 0: stock policy unchanged

Core block, after the start above, then `~/dl.sh q0` and the marker:

```bash
~/dl.sh q0
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
~/dl.sh q3 12.1.1.2 12.1.1.5
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
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

A cap is a hard limit per grant, so expect slice 1 at about 19 % (20 of 106 resource blocks in
the slots it leads), slice 2 about 81 %, and the resource blocks per scheduler call still close
to 104.

### Run 6: membership by S-NSSAI (checks what the MAC reports; no change of the core)

All four UEs attach with SST 1 and no SD, so one rule moves all of them:

```bash
~/x.sh policy slice
~/dl.sh q6
sleep 5
~/x.sh nssai-map 1 none 2
~/x.sh assoc $U1 1
echo "=== TRAFFIC STARTED: paste the cell1 block now ==="
```

Expect `status` to show a rule `S-NSSAI sst=1 sd=none -> slice 2`, every UE with `nssai 1:ffffff`,
`U1` in slice 1 `via rnti` and the others in slice 2 `via nssai`. If the NSSAI column shows `-` or
`0:ffffff` the MAC does not report the bearer's S-NSSAI and membership by S-NSSAI (step B of
`SLICING_DESIGN.md`) cannot work as designed; record that and stop there.

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
message, or throughput guarantees. Results of the earlier policies show that the total
throughput of a run varies by about ±10 % at the same resource-block rate, so judge by the
resource-block shares.
