#!/bin/bash
# =============================================================================
# cell-setup.sh — cell node: one E2-enabled, chanmod-capable gNB + K UEs
#
# Usage: cell-setup.sh <cell_idx> <num_cells> <ues_per_cell>
#
# The gNB runs on the host network (N2/N3/E2 originate from this node's LAN IP).
# Its UEs are colocated on a cell-local bridge so the RFsim IQ path stays local
# and the channel model behaves as in the validated single-node baseline.
#
# Cross-node reachability: the core node's Docker bridges (192.168.71/72.128/26)
# are reached via a route through the core node's LAN IP, with ip_forward enabled
# on the core. This is the same hop pattern validated on the OTA track.
#
# The gNB is only started once the core node has published ric-ready (see
# ready-lib.sh). If the core reports a failure, or never becomes ready within
# READY_TIMEOUT, this script stops without starting the gNB. The script can be
# re-run on a live reservation; the previous cell containers are removed first.
# Outcome markers: /local/logs/CELL_READY or /local/logs/CELL_FAILED.
# =============================================================================

set +e

CELL_IDX="${1:-1}"
NUM_CELLS="${2:-1}"
UES_PER_CELL="${3:-2}"

if [ "$CELL_IDX" != 1 ] || [ "$NUM_CELLS" != 1 ]; then
    echo "ERROR: this repository supports exactly one cell (cell1)" >&2
    exit 2
fi
[[ "$UES_PER_CELL" =~ ^[1-9][0-9]*$ ]] || { echo "ERROR: invalid UE count" >&2; exit 2; }

CORE_LAN_IP="10.10.1.1"
CELL_LAN_IP="10.10.1.$((10 + CELL_IDX))"

AMF_IP="192.168.71.132"
UPF_IP="192.168.71.134"
RIC_IP="10.10.1.1"        # the native RIC binds to the core node's LAN IP
DN_SUBNET="192.168.72.128/26"
READY_URL="http://${CORE_LAN_IP}:8099"
READY_TIMEOUT="${READY_TIMEOUT:-3600}"

REPO_DIR="/local/repository"
. "${REPO_DIR}/bin/ready-lib.sh"

# Per-cell identities. These MUST be unique across cells or the AMF/RIC will
# reject or confuse the second gNB.
GNB_ID=$((0xe00 + CELL_IDX - 1))
PCI=$((CELL_IDX - 1))
NR_CELL_ID=$((12345678 + CELL_IDX - 1))
UE_SUBNET="172.$((20 + CELL_IDX)).0.0/24"
DOCKER_BRIDGE_CIDR="172.30.${CELL_IDX}.1/24"

# IMSI slice for this cell: base + (cell-1)*K .. + K-1
IMSI_BASE=208990100001100
IMSI_START=$((IMSI_BASE + (CELL_IDX - 1) * UES_PER_CELL))

COMPOSE="docker-compose-cell${CELL_IDX}.yaml"

mkdir -p /local/logs /local/logs/mgen
chmod 777 /local/logs/mgen
rm -f /local/logs/CELL_READY /local/logs/CELL_FAILED

fail() {
    echo "[CELL${CELL_IDX}] ERROR: $*" >&2
    echo "$*" > /local/logs/CELL_FAILED
    exit 1
}

echo "============================================"
echo "[CELL${CELL_IDX}] started at $(date)"
echo "[CELL${CELL_IDX}] lan=${CELL_LAN_IP} gnb_id=${GNB_ID} pci=${PCI}"
echo "[CELL${CELL_IDX}] imsi ${IMSI_START}..$((IMSI_START + UES_PER_CELL - 1))"
echo "============================================"

# ------------------------------------------------------------------ #
# 0. Remove containers from a previous run of this script.
# ------------------------------------------------------------------ #
if command -v docker > /dev/null 2>&1 && [ -f "${REPO_DIR}/etc/${COMPOSE}" ]; then
    (cd "${REPO_DIR}/etc" && docker compose -f "${COMPOSE}" down > /dev/null 2>&1)
fi

# ------------------------------------------------------------------ #
# 1. Install Docker
#
#    FlexRIC's SCTP client advertises every address on the cell host. If every
#    node keeps Docker's default 172.17.0.1 address, the RIC sends an
#    SCTP heartbeat for a cell to its own local docker0 and the kernel answers
#    with ABORT, destroying an otherwise healthy E2 association. Configure a
#    unique default bridge before Docker starts for the first time.
# ------------------------------------------------------------------ #
apt-get update -y
apt-get install -y ca-certificates curl gnupg lsb-release \
    || fail "apt-get install of base packages failed"

install -m 0755 -d /etc/docker
cat > /etc/docker/daemon.json <<EOF
{
  "bip": "${DOCKER_BRIDGE_CIDR}"
}
EOF

if ! docker compose version > /dev/null 2>&1; then
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
        | gpg --dearmor --yes -o /etc/apt/keyrings/docker.gpg \
        || fail "could not fetch the Docker apt key"
    chmod a+r /etc/apt/keyrings/docker.gpg
    echo \
      "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
      https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
      > /etc/apt/sources.list.d/docker.list
    apt-get update -y
    apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin \
        || fail "Docker installation failed"
fi

systemctl restart docker
ACTUAL_DOCKER_BRIDGE="$(ip -4 -o addr show docker0 2>/dev/null | awk '{print $4}')"
if [ "${ACTUAL_DOCKER_BRIDGE}" != "${DOCKER_BRIDGE_CIDR}" ]; then
    fail "docker0=${ACTUAL_DOCKER_BRIDGE:-missing}, expected ${DOCKER_BRIDGE_CIDR}"
fi
echo "[CELL${CELL_IDX}] docker0=${ACTUAL_DOCKER_BRIDGE} (unique SCTP address)"

# ------------------------------------------------------------------ #
# 2. Route the core's Docker bridges via the core node.
#    Without this the gNB cannot reach the AMF, UPF or RIC.
# ------------------------------------------------------------------ #
echo "[CELL${CELL_IDX}] Adding routes to core bridges via ${CORE_LAN_IP}..."
sysctl -w net.ipv4.ip_forward=1
ip route replace 192.168.71.128/26 via ${CORE_LAN_IP} || fail "route to 192.168.71.128/26 failed"
ip route replace 192.168.72.128/26 via ${CORE_LAN_IP} || fail "route to 192.168.72.128/26 failed"
ip route show | grep 192.168.7

# ------------------------------------------------------------------ #
# 2b. Controllable gNB image.  Default: the validated stock image (no scheduler
#     control).  GNB_CTRL=1 selects the controllable scheduler: either
#     GNB_CTRL_IMAGE (a pushed image, pulled in step 4) or an image built on this
#     node by gnb/build-gnb-image.sh.  The build needs only Docker and the
#     internet, so it starts now and runs while the core node is still bringing
#     up the core network and the RIC; step 4 collects the result.
# ------------------------------------------------------------------ #
STOCK_GNB_IMAGE="ghinwa555/oai-gnb-e2-chan@sha256:1bc3c341b498fdc2096721bd0b86f5bdd3ca985a930617163c611db5a9bc5501"
GNB_BUILD_PID=""
GNB_BUILD_OUT="/local/logs/gnb-build.stdout"
if [ "${GNB_CTRL:-0}" = 1 ]; then
    if [ -n "${GNB_CTRL_IMAGE:-}" ]; then
        [[ "${GNB_CTRL_IMAGE}" =~ ^[A-Za-z0-9][A-Za-z0-9._/:@-]*$ ]] || fail "invalid GNB_CTRL_IMAGE '${GNB_CTRL_IMAGE}'"
    else
        echo "[CELL${CELL_IDX}] Building the controllable gNB image in the background (log: /local/logs/gnb-build.log)..."
        : > "${GNB_BUILD_OUT}"
        bash "${REPO_DIR}/gnb/build-gnb-image.sh" > "${GNB_BUILD_OUT}" 2>&1 &
        GNB_BUILD_PID=$!
    fi
fi

# ------------------------------------------------------------------ #
# 3. Wait for the core: it publishes ric-ready once the core network is up and
#    the RIC is listening. The core build can take a long time, so this waits on
#    that signal rather than on a fixed delay, and never starts the gNB without it.
# ------------------------------------------------------------------ #
echo "[CELL${CELL_IDX}] Waiting for the core node (up to ${READY_TIMEOUT}s)..."
wait_for_ready "${READY_URL}/ric-ready" "${READY_URL}/failed" "${READY_TIMEOUT}" 15
case $? in
    0) echo "[CELL${CELL_IDX}] Core and RIC report ready." ;;
    2) fail "core setup reported a failure (see /local/logs/CORE_FAILED on the core node)" ;;
    *) fail "core not ready after ${READY_TIMEOUT}s (see /local/logs/setup.log on the core node)" ;;
esac
ping -c1 -W2 "${AMF_IP}" > /dev/null 2>&1 \
    || fail "AMF ${AMF_IP} unreachable although the core is ready; check the route and forwarding on the core"

# ------------------------------------------------------------------ #
# 4. Pull images
# ------------------------------------------------------------------ #
GNB_IMAGE="${STOCK_GNB_IMAGE}"
if [ "${GNB_CTRL:-0}" = 1 ]; then
    if [ -n "${GNB_CTRL_IMAGE:-}" ]; then
        echo "[CELL${CELL_IDX}] Pulling controllable gNB image ${GNB_CTRL_IMAGE}..."
        docker pull "${GNB_CTRL_IMAGE}" || fail "controllable gNB image pull failed (${GNB_CTRL_IMAGE})"
        GNB_IMAGE="${GNB_CTRL_IMAGE}"
    else
        echo "[CELL${CELL_IDX}] Waiting for the controllable gNB image build (log: /local/logs/gnb-build.log)..."
        wait "${GNB_BUILD_PID}" \
            || fail "controllable gNB image build failed (see /local/logs/gnb-build.log)"
        GNB_IMAGE="$(tail -n 1 "${GNB_BUILD_OUT}")"
        docker image inspect "${GNB_IMAGE}" > /dev/null 2>&1 \
            || fail "controllable gNB image '${GNB_IMAGE}' not found after build (see /local/logs/gnb-build.log)"
    fi
fi
echo "[CELL${CELL_IDX}] gNB image: ${GNB_IMAGE}"
echo "${GNB_IMAGE}" > /local/logs/GNB_IMAGE
if [ "${GNB_IMAGE}" = "${STOCK_GNB_IMAGE}" ]; then
    docker pull "${STOCK_GNB_IMAGE}" || fail "gNB image pull failed"
fi
docker pull ghinwa555/oai-nr-ue-chan@sha256:f00af9714f0e953c79d8e4def6b71d15d9af2c504bca1e0a8db2edbb59d6fb60 \
    || fail "UE image pull failed"

# ------------------------------------------------------------------ #
# 5. Generate this cell's gNB config
#    - e2_agent block pointing at the RIC on the core node
#    - rfsimulator block with options=("chanmod")  [NOT a CLI flag]
#    - @include by BARE FILENAME, file co-located (absolute paths fail)
#    - NGU advertises this node's LAN IP so downlink GTP-U comes back here
# ------------------------------------------------------------------ #
cd "${REPO_DIR}/etc" || fail "cannot enter ${REPO_DIR}/etc"

sed -e "s/@CELL_IDX@/${CELL_IDX}/g" \
    -e "s/@GNB_ID@/${GNB_ID}/g" \
    -e "s/@PCI@/${PCI}/g" \
    -e "s/@NR_CELL_ID@/${NR_CELL_ID}/g" \
    -e "s/@AMF_IP@/${AMF_IP}/g" \
    -e "s/@RIC_IP@/${RIC_IP}/g" \
    -e "s/@CELL_LAN_IP@/${CELL_LAN_IP}/g" \
    gnb-cell.conf.tmpl > gnb-cell${CELL_IDX}.conf || fail "gNB config generation failed"

# The gNB receiver needs rfsimu_channel_ue0..ue(K-1) for uplink connections.
# Each UE receiver independently activates rfsimu_channel_enB0 (index 0) for
# downlink. CHANMOD_MODE/TYPE are overridable.
CHANMOD_MODE="${CHANMOD_MODE:-uniform}"
CHANMOD_TYPE="${CHANMOD_TYPE:-AWGN}"
bash "${REPO_DIR}/bin/gen-channelmod.sh" "${UES_PER_CELL}" \
     "channelmod-cell${CELL_IDX}.conf" "${CHANMOD_MODE}" "${CHANMOD_TYPE}" \
    || fail "channel model generation failed"

echo "[CELL${CELL_IDX}] gNB config generated:"
grep -E "gNB_ID|physCellId|nr_cellid|amf_ip|near_ric|options|min_rxtxtime|GNB_IPV4" \
     gnb-cell${CELL_IDX}.conf

# ------------------------------------------------------------------ #
# 6. Generate the compose file (gNB + K UEs)
# ------------------------------------------------------------------ #
cat > "${COMPOSE}" <<EOF
services:
  oai-gnb:
    container_name: ric5g-gnb-cell${CELL_IDX}
    image: ${GNB_IMAGE}
    network_mode: host
    cap_drop:
      - ALL
    environment:
      TZ: Europe/Paris
      ASAN_OPTIONS: detect_leaks=0
      USE_ADDITIONAL_OPTIONS: "-E --telnetsrv --rfsim --gNBs.[0].min_rxtxtime 3 --log_config.global_log_options level,nocolor,time"
    volumes:
      - ./gnb-cell${CELL_IDX}.conf:/opt/oai-gnb/etc/gnb.conf
      - ./channelmod-cell${CELL_IDX}.conf:/opt/oai-gnb/etc/channelmod_rfsimu.conf
    healthcheck:
      test: /bin/bash -c "pgrep nr-softmodem"
      interval: 10s
      timeout: 5s
      retries: 10
EOF

for u in $(seq 1 "${UES_PER_CELL}"); do
    IMSI=$((IMSI_START + u - 1))
    UE_IP="172.$((20 + CELL_IDX)).0.$((10 + u))"

    # Per-UE config: the rfsimulator block stock ue.conf does NOT have, with
    # chanmod enabled and serveraddr pointing at the gNB on the host.
    sed -e "s/@IMSI@/${IMSI}/g" \
        -e "s/@GNB_ADDR@/${CELL_LAN_IP}/g" \
        nr-ue.conf.tmpl > nr-ue-cell${CELL_IDX}-${u}.conf

    cat >> "${COMPOSE}" <<EOF

  oai-nr-ue${u}:
    container_name: ric5g-ue-cell${CELL_IDX}-${u}
    image: ghinwa555/oai-nr-ue-chan@sha256:f00af9714f0e953c79d8e4def6b71d15d9af2c504bca1e0a8db2edbb59d6fb60
    cap_drop:
      - ALL
    cap_add:
      - NET_ADMIN
      - NET_RAW
    devices:
      - /dev/net/tun:/dev/net/tun
    environment:
      TZ: Europe/Paris
      ASAN_OPTIONS: detect_leaks=0
      USE_ADDITIONAL_OPTIONS: "-E --telnetsrv --rfsim -r 106 --numerology 1 --uicc0.imsi ${IMSI} -C 3319680000 --log_config.global_log_options level,nocolor,time"
    volumes:
      - ./nr-ue-cell${CELL_IDX}-${u}.conf:/opt/oai-nr-ue/etc/nr-ue.conf
      - ./channelmod-cell${CELL_IDX}.conf:/opt/oai-nr-ue/etc/channelmod_rfsimu.conf
      - ../bin/mgen:/usr/bin/mgen:ro
      - /local/logs/mgen:/logs/mgen
    depends_on:
      - oai-gnb
    networks:
      ue_net:
        ipv4_address: ${UE_IP}
EOF
done

cat >> "${COMPOSE}" <<EOF

networks:
  ue_net:
    driver: bridge
    name: ric5g-ue-cell${CELL_IDX}
    ipam:
      config:
        - subnet: ${UE_SUBNET}
EOF

echo "[CELL${CELL_IDX}] compose generated: ${COMPOSE}"

# ------------------------------------------------------------------ #
# 7. mgen must be present (bind-mounted into every UE)
# ------------------------------------------------------------------ #
MGEN_BIN="${REPO_DIR}/bin/mgen"
if [ ! -f "$MGEN_BIN" ] || [ ! -x "$MGEN_BIN" ]; then
    fail "$MGEN_BIN missing or not executable"
fi

# ------------------------------------------------------------------ #
# 8. Start the cell
# ------------------------------------------------------------------ #
docker compose -f "${COMPOSE}" up -d || fail "docker compose up of the cell failed"

# ------------------------------------------------------------------ #
# 9. Verify the gNB: chanmod active + E2 connected
#    These are printed for inspection; whether E2 setup succeeded is confirmed
#    from the RIC log on the core node.
# ------------------------------------------------------------------ #
sleep 45
docker ps --format '{{.Names}}' | grep -qx "ric5g-gnb-cell${CELL_IDX}" \
    || fail "gNB container ric5g-gnb-cell${CELL_IDX} is not running (docker logs ric5g-gnb-cell${CELL_IDX})"
echo "[CELL${CELL_IDX}] chanmod check (expect [OCM] model allocated):"
docker logs ric5g-gnb-cell${CELL_IDX} 2>&1 | grep -i "OCM" | head -4
echo "[CELL${CELL_IDX}] E2 check:"
docker logs ric5g-gnb-cell${CELL_IDX} 2>&1 | grep -iE "E2 SETUP|E2 agent" | head -4

# ------------------------------------------------------------------ #
# 10. Wait for UEs, restart any stuck in REG-INITIATED (known race)
# ------------------------------------------------------------------ #
echo "[CELL${CELL_IDX}] Waiting for UEs to attach (180s)..."
sleep 180

STUCK=""
for u in $(seq 1 "${UES_PER_CELL}"); do
    IMSI=$((IMSI_START + u - 1))
    if ! docker exec ric5g-ue-cell${CELL_IDX}-${u} ip link show oaitun_ue1 >/dev/null 2>&1; then
        echo "[CELL${CELL_IDX}] UE${u} (${IMSI}) has no tunnel, restarting..."
        docker restart ric5g-ue-cell${CELL_IDX}-${u}
        STUCK="$STUCK UE${u}"
    fi
done
[ -n "$STUCK" ] && { echo "[CELL${CELL_IDX}] restarted:$STUCK — waiting 90s..."; sleep 90; }

# ------------------------------------------------------------------ #
# 11. DN route per UE. Without this the kernel sends app traffic out eth0
#     (the control bridge), bypassing the 5G stack entirely and producing
#     traffic that looks fine but never touched the RAN.
# ------------------------------------------------------------------ #
echo "[CELL${CELL_IDX}] Adding DN route (${DN_SUBNET}) via oaitun_ue1..."
UE_PROBLEMS=""
for u in $(seq 1 "${UES_PER_CELL}"); do
    C="ric5g-ue-cell${CELL_IDX}-${u}"
    W=0
    until docker exec "$C" ip link show oaitun_ue1 >/dev/null 2>&1; do
        if [ "$W" -ge 120 ]; then
            echo "[CELL${CELL_IDX}] WARNING: UE${u}: no oaitun_ue1 after 120s"
            break
        fi
        sleep 5
        W=$((W + 5))
    done
    if docker exec "$C" ip link show oaitun_ue1 >/dev/null 2>&1; then
        if docker exec "$C" ip route replace ${DN_SUBNET} dev oaitun_ue1; then
            echo "[CELL${CELL_IDX}] UE${u}: DN route added"
        else
            echo "[CELL${CELL_IDX}] WARNING: UE${u}: DN route failed"
            UE_PROBLEMS="${UE_PROBLEMS} UE${u}(route)"
        fi
    else
        UE_PROBLEMS="${UE_PROBLEMS} UE${u}(no-tunnel)"
    fi
done

echo "============================================"
echo "[CELL${CELL_IDX}] completed at $(date)"
echo "============================================"
docker ps --format 'table {{.Names}}\t{{.Image}}\t{{.Status}}'

if [ -n "${UE_PROBLEMS}" ]; then
    fail "UEs not ready:${UE_PROBLEMS}"
fi
touch /local/logs/CELL_READY
echo "[CELL${CELL_IDX}] All ${UES_PER_CELL} UEs have a tunnel and a DN route (CELL_READY)."
