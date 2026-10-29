#!/bin/bash
# =============================================================================
# core-setup.sh — core node: 5G core NFs + FlexRIC near-RT RIC (built natively)
#
# Runs on the single "core" node. Brings up the CN, then builds and starts the
# RIC, and enables forwarding so cell nodes can reach the Docker bridges
# (192.168.71.128/26 and 192.168.72.128/26) across the experimental LAN.
#
# Order: Docker -> images -> core network -> RIC build -> RIC start.
# The core network comes up before the (long) RIC compile. Progress is published
# to the cell node as marker files over HTTP (see ready-lib.sh):
#   cn-ready   core network is up
#   ric-ready  RIC is listening; the cell may start its gNB
#   failed     setup aborted; the cell stops waiting
#
# The script can be re-run on a live reservation: the core network and RIC are
# restarted, and the RIC is only rebuilt when the pinned sources or the patch
# scripts changed.
# =============================================================================

set +e

CORE_LAN_IP="${1:-10.10.1.1}"
NUM_CELLS="${2:-1}"
UES_PER_CELL="${3:-2}"
if [ "$NUM_CELLS" != 1 ]; then
    echo "ERROR: this repository supports exactly one cell" >&2
    exit 2
fi
[[ "$UES_PER_CELL" =~ ^[1-9][0-9]*$ ]] || { echo "ERROR: invalid UE count" >&2; exit 2; }
OAI_COMMIT="70508ebaf52f2aae420566d380c6537f2efb9f0c"
FLEXRIC_COMMIT="ef6d722f22191eea74089966983da1f5ec1fedd4"

REPO_DIR="/local/repository"
READY_DIR="/local/ready"
READY_PORT=8099
OAI_SRC="/opt/oai-src"
FLEXRIC_DIR="${OAI_SRC}/openair2/E2AP/flexric"

. "${REPO_DIR}/bin/ready-lib.sh"

mkdir -p /local/logs /local/logs/xapp /local/logs/mgen "${READY_DIR}"
chmod 777 /local/logs/xapp /local/logs/mgen
rm -f "${READY_DIR}/cn-ready" "${READY_DIR}/ric-ready" "${READY_DIR}/failed" \
      /local/logs/CORE_READY /local/logs/CORE_FAILED

fail() {
    echo "[CORE] ERROR: $*" >&2
    echo "$*" > /local/logs/CORE_FAILED
    echo "$*" > "${READY_DIR}/failed"
    exit 1
}

echo "============================================"
echo "[CORE] started at $(date)  lan_ip=${CORE_LAN_IP}"
echo "============================================"

# The marker server only exposes the files in READY_DIR, bound to the LAN address.
ready_server_start "${READY_DIR}" "${CORE_LAN_IP}" "${READY_PORT}" \
    /local/logs/ready-server.log \
    || fail "readiness server did not start on ${CORE_LAN_IP}:${READY_PORT}"

# ------------------------------------------------------------------ #
# 1. Install Docker
# ------------------------------------------------------------------ #
apt-get update -y
apt-get install -y ca-certificates curl gnupg lsb-release \
    || fail "apt-get install of base packages failed"

if ! docker compose version > /dev/null 2>&1; then
    echo "[CORE] Installing Docker..."
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
        | gpg --dearmor --yes -o /etc/apt/keyrings/docker.gpg \
        || fail "could not fetch the Docker apt key"
    chmod a+r /etc/apt/keyrings/docker.gpg

    echo \
      "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
      https://download.docker.com/linux/ubuntu \
      $(lsb_release -cs) stable" \
      > /etc/apt/sources.list.d/docker.list

    apt-get update -y
    apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin \
        || fail "Docker installation failed"
fi
docker --version
docker compose version || fail "docker compose plugin is not usable"

# ------------------------------------------------------------------ #
# 2. IP forwarding — REQUIRED so cell nodes can reach the CN bridges.
#    Each cell node routes 192.168.71/72.128/26 via this node's LAN IP;
#    without forwarding those packets die here.
# ------------------------------------------------------------------ #
echo "[CORE] Enabling IP forwarding..."
sysctl -w net.ipv4.ip_forward=1
grep -qx "net.ipv4.ip_forward=1" /etc/sysctl.conf \
    || echo "net.ipv4.ip_forward=1" >> /etc/sysctl.conf
iptables -P FORWARD ACCEPT

# ------------------------------------------------------------------ #
# 3. Pull images
# ------------------------------------------------------------------ #
echo "[CORE] Pulling images..."
for image in \
    mysql:8.0 \
    oaisoftwarealliance/oai-udr:v2.0.0 \
    oaisoftwarealliance/oai-udm:v2.0.0 \
    oaisoftwarealliance/oai-ausf:v2.0.0 \
    oaisoftwarealliance/oai-amf:v2.0.0 \
    oaisoftwarealliance/oai-smf:v2.0.0 \
    oaisoftwarealliance/oai-upf:v2.0.0 \
    oaisoftwarealliance/trf-gen-cn5g:focal
do
    docker pull "${image}" || fail "docker pull ${image} failed"
done
echo "[CORE] All images pulled."

# ------------------------------------------------------------------ #
# 4. mgen must be present (bind-mounted into upf + ext-dn)
# ------------------------------------------------------------------ #
MGEN_BIN="${REPO_DIR}/bin/mgen"
if [ ! -f "$MGEN_BIN" ] || [ ! -x "$MGEN_BIN" ]; then
    fail "$MGEN_BIN missing or not executable"
fi

# ------------------------------------------------------------------ #
# 5. Generate the subscriber DB for this topology.
#    MySQL reinitializes from oai_db.sql on every start, so every UE across
#    every cell must be present BEFORE the stack comes up. A UE missing here
#    will fail authentication, or (if only half-present) register and then fail
#    PDU session establishment.
# ------------------------------------------------------------------ #
bash "${REPO_DIR}/bin/gen-subscribers.sh" "${NUM_CELLS}" "${UES_PER_CELL}" \
    || fail "subscriber database generation failed"

# ------------------------------------------------------------------ #
# 6. Start the core network.
#    This runs from etc/ because docker-compose-core.yaml lives there; compose
#    fails with "no configuration file provided" from any other directory.
#    A previous instance is removed first so the script can be re-run.
# ------------------------------------------------------------------ #
echo "[CORE] Starting core network..."
cd "${REPO_DIR}/etc" || fail "cannot enter ${REPO_DIR}/etc"
docker compose -f docker-compose-core.yaml down > /dev/null 2>&1
docker compose -f docker-compose-core.yaml up -d \
    || fail "docker compose up of the core network failed"

echo "[CORE] Waiting for AMF..."
MAX_WAIT=600
ELAPSED=0
# The AMF image defines a container healthcheck, which is a more stable signal
# than its log text (the log never contains a fixed "started" line). If an image
# without a healthcheck is ever used, a running container is accepted instead.
amf_state() {
    docker inspect \
        -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' \
        ric5g-oai-amf 2> /dev/null
}
until state="$(amf_state)"; [ "${state}" = "healthy" ] || [ "${state}" = "running" ]; do
    if [ "$ELAPSED" -ge "$MAX_WAIT" ]; then
        fail "AMF not ready after ${MAX_WAIT}s (state: ${state:-unknown}; see docker logs ric5g-oai-amf)"
    fi
    echo "[CORE] Waiting for AMF... (${ELAPSED}s, state: ${state:-unknown})"
    sleep 10
    ELAPSED=$((ELAPSED + 10))
done
echo "[CORE] AMF ready (state: ${state})."

# Every core container must be running, not merely created.
EXPECTED="ric5g-mysql ric5g-oai-udr ric5g-oai-udm ric5g-oai-ausf ric5g-oai-amf ric5g-oai-smf ric5g-oai-upf ric5g-oai-ext-dn"
ELAPSED=0
while true; do
    MISSING=""
    RUNNING="$(docker ps --format '{{.Names}}')"
    for name in ${EXPECTED}; do
        echo "${RUNNING}" | grep -qx "${name}" || MISSING="${MISSING} ${name}"
    done
    [ -z "${MISSING}" ] && break
    if [ "$ELAPSED" -ge 120 ]; then
        fail "core containers not running:${MISSING}"
    fi
    sleep 5
    ELAPSED=$((ELAPSED + 5))
done
touch "${READY_DIR}/cn-ready"
echo "[CORE] Core network up (cn-ready published)."

# ------------------------------------------------------------------ #
# 7. Build FlexRIC natively, from OAI's submodule.
#
#    The RIC and the xApps MUST come from the same source tree as the gNB's E2
#    agent (openair2/E2AP/flexric). A RIC built from the standalone FlexRIC
#    repository sends subscription requests that OAI's embedded agent does not
#    answer: the SCTP association is up, the request is sent, and the gNB never
#    responds. No indications are ever produced, for any service model.
#
#    The RIC therefore runs as a host process, not a container.
#
#    A stamp records what was built. If it matches the pinned commits and the
#    current patch scripts, the build is skipped.
# ------------------------------------------------------------------ #
PATCH_HASH="$(cat "${REPO_DIR}"/bin/patch-flexric-*.py | sha256sum | cut -d' ' -f1)"
BUILD_STAMP="${OAI_COMMIT} ${FLEXRIC_COMMIT} ${PATCH_HASH} KPM_V3_00 E2AP_V3 SQLITE3_XAPP"
STAMP_FILE="${OAI_SRC}/.ric-build-stamp"

if [ -x "${FLEXRIC_DIR}/build/examples/ric/nearRT-RIC" ] \
   && [ "$(cat "${STAMP_FILE}" 2>/dev/null)" = "${BUILD_STAMP}" ] \
   && [ -f /usr/local/etc/flexric/flexric.conf ]; then
    echo "[CORE] FlexRIC already built for these sources and patches; skipping build."
else
    echo "[CORE] Installing FlexRIC build dependencies..."
    apt-get install -y git cmake ninja-build build-essential swig libsctp-dev \
        python3-dev pkg-config libconfig-dev libconfig++-dev bison flex \
        libpcre2-dev autoconf automake libtool gcc-10 g++-10 \
        || fail "FlexRIC build dependencies failed to install"

    # gcc-11 will not build FlexRIC
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 100
    update-alternatives --set gcc /usr/bin/gcc-10
    update-alternatives --set g++ /usr/bin/g++-10
    gcc --version | head -1

    # asn1c pinned to OAI's commit; vlm_master generates code that will not link
    if [ ! -x /opt/asn1c/bin/asn1c ]; then
        echo "[CORE] Building asn1c..."
        rm -rf /tmp/asn1c-src
        git clone https://github.com/mouse07410/asn1c /tmp/asn1c-src \
            || fail "asn1c clone failed"
        (
            cd /tmp/asn1c-src \
            && git checkout 940dd5fa9f3917913fd487b13dfddfacd0ded06e \
            && autoreconf -iv \
            && CFLAGS="-O2 -fno-strict-aliasing" ./configure --prefix /opt/asn1c/ \
            && make -j"$(nproc)" && make install && ldconfig
        ) || fail "asn1c build failed"
    fi

    echo "[CORE] Building FlexRIC from the OAI submodule..."
    rm -rf "${OAI_SRC}"
    git clone https://gitlab.eurecom.fr/oai/openairinterface5g.git "${OAI_SRC}" \
        || fail "OAI clone failed"
    git -C "${OAI_SRC}" checkout --detach "${OAI_COMMIT}" || fail "OAI checkout failed"
    git -C "${OAI_SRC}" submodule update --init --recursive \
        || fail "OAI submodule update failed"

    ACTUAL_OAI_COMMIT="$(git -C "${OAI_SRC}" rev-parse HEAD)"
    ACTUAL_FLEXRIC_COMMIT="$(git -C "${FLEXRIC_DIR}" rev-parse HEAD)"
    [ "${ACTUAL_OAI_COMMIT}" = "${OAI_COMMIT}" ] \
        || fail "OAI ${ACTUAL_OAI_COMMIT}, expected ${OAI_COMMIT}"
    [ "${ACTUAL_FLEXRIC_COMMIT}" = "${FLEXRIC_COMMIT}" ] \
        || fail "FlexRIC ${ACTUAL_FLEXRIC_COMMIT}, expected ${FLEXRIC_COMMIT}"
    echo "[CORE] OAI=${ACTUAL_OAI_COMMIT} FlexRIC=${ACTUAL_FLEXRIC_COMMIT}"

    python3 "${REPO_DIR}/bin/patch-flexric-sqlite-buffers.py" \
        "${FLEXRIC_DIR}/src/xApp/db/sqlite3/sqlite3_wrapper.c" \
        || fail "patch-flexric-sqlite-buffers failed"
    python3 "${REPO_DIR}/bin/patch-flexric-receipt-timestamps.py" \
        "${FLEXRIC_DIR}/src/xApp/db/sqlite3/sqlite3_wrapper.c" \
        || fail "patch-flexric-receipt-timestamps failed"
    python3 "${REPO_DIR}/bin/patch-flexric-iapp-log.py" \
        "${FLEXRIC_DIR}/src/ric/iApps/stdout.c" \
        || fail "patch-flexric-iapp-log failed"
    # The patch scripts insert SQL schema strings with trailing whitespace.
    # git diff --check exits non-zero on whitespace errors, so normalise first.
    sed -i 's/[[:space:]]*$//' \
        "${FLEXRIC_DIR}/src/xApp/db/sqlite3/sqlite3_wrapper.c" \
        "${FLEXRIC_DIR}/src/ric/iApps/stdout.c"
    git -C "${FLEXRIC_DIR}" diff --check || fail "whitespace check on patched FlexRIC failed"

    (
        cd "${FLEXRIC_DIR}" && mkdir -p build && cd build \
        && cmake -DKPM_VERSION=KPM_V3_00 -DE2AP_VERSION=E2AP_V3 -DXAPP_DB=SQLITE3_XAPP .. \
        && make -j"$(nproc)" \
        && make install
    ) || fail "FlexRIC build failed"
    ls -1 /usr/local/lib/flexric/
    echo "${BUILD_STAMP}" > "${STAMP_FILE}"
fi

# make install OVERWRITES flexric.conf and resets NEAR_RIC_IP to 127.0.0.1.
# Rewrite it afterwards, or the cell nodes cannot reach the RIC.
sed -i "s|^NEAR_RIC_IP.*|NEAR_RIC_IP = ${CORE_LAN_IP}|" /usr/local/etc/flexric/flexric.conf
echo "[CORE] flexric.conf:"; cat /usr/local/etc/flexric/flexric.conf

# ------------------------------------------------------------------ #
# 8. Start the native RIC.
#
#    Runs under nohup so it survives this script exiting. Its log is at
#    /local/logs/nearRT-RIC.log.
#
#    stdbuf -oL -eL is REQUIRED: FlexRIC block-buffers stdout when it is not
#    writing to a TTY (here it writes to a file). Without line-buffering the log
#    stays empty for a long time after the RIC is already up.
#
#    Do NOT auto-start an xApp here. An xApp started before the gNBs have
#    completed E2 setup crashes and leaves an orphaned SCTP association that
#    poisons the RIC's E42 state from boot. Run xApps by hand, after the cells
#    have registered:
#
#      cd /opt/oai-src/openair2/E2AP/flexric
#      ./build/examples/xApp/c/monitor/xapp_kpm_moni
# ------------------------------------------------------------------ #
if pgrep -x nearRT-RIC > /dev/null; then
    echo "[CORE] Stopping previous nearRT-RIC..."
    pkill -INT -x nearRT-RIC
    sleep 3
    pkill -x nearRT-RIC
fi

echo "[CORE] Starting nearRT-RIC on ${CORE_LAN_IP}:36421..."
cd "${FLEXRIC_DIR}" || fail "cannot enter ${FLEXRIC_DIR}"
nohup stdbuf -oL -eL ./build/examples/ric/nearRT-RIC > /local/logs/nearRT-RIC.log 2>&1 &

# Ready means: process alive and either the service models loaded or the E2
# SCTP port is listening.
ELAPSED=0
while true; do
    if pgrep -x nearRT-RIC > /dev/null \
       && { grep -q "Loading SM ID = 2" /local/logs/nearRT-RIC.log \
            || ss -lnS 2> /dev/null | grep -q ":36421"; }; then
        break
    fi
    if [ "$ELAPSED" -ge 90 ]; then
        tail -20 /local/logs/nearRT-RIC.log
        fail "nearRT-RIC not ready after 90s (see /local/logs/nearRT-RIC.log)"
    fi
    sleep 3
    ELAPSED=$((ELAPSED + 3))
done
echo "[CORE] RIC ready — E2 on ${CORE_LAN_IP}:36421, E42 on ${CORE_LAN_IP}:36422"
grep -E "nearRT-RIC IP|Loading SM ID" /local/logs/nearRT-RIC.log

touch "${READY_DIR}/ric-ready" /local/logs/CORE_READY

# ------------------------------------------------------------------ #
# Done. Cell nodes poll ric-ready and then start on their own.
# ------------------------------------------------------------------ #
echo "============================================"
echo "[CORE] completed at $(date)"
echo "============================================"
docker ps --format 'table {{.Names}}\t{{.Image}}\t{{.Status}}'
