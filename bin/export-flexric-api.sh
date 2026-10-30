#!/bin/bash
# =============================================================================
# export-flexric-api.sh — collect the FlexRIC and OAI sources that define the E2
# control interface, so that an E2 control message and a matching xApp can be
# written against the exact pinned versions.
#
# Usage: bash bin/export-flexric-api.sh [OAI_SRC] [OUT_TARBALL]
#   defaults: /opt/oai-src  /local/logs/flexric-api.tar.gz
#
# Writes the archive and a file list beside it (<OUT>.txt).  Missing files are
# skipped and reported; the script never fails the node setup.
# =============================================================================
set -uo pipefail

OAI_SRC="${1:-/opt/oai-src}"
OUT="${2:-/local/logs/flexric-api.tar.gz}"
FLEX="openair2/E2AP/flexric"
RAN="openair2/E2AP/RAN_FUNCTION"

if [ ! -d "${OAI_SRC}/${FLEX}/src" ]; then
    echo "[EXPORT] ${OAI_SRC}/${FLEX}/src not found; nothing exported" >&2
    exit 0
fi

LIST="$(mktemp)"
trap 'rm -f "${LIST}"' EXIT
cd "${OAI_SRC}" || exit 0

{
    # service-model data types, agent and RIC side, for the MAC, RC and slice models
    find "${FLEX}/src/sm/mac_sm" "${FLEX}/src/sm/rc_sm" "${FLEX}/src/sm/slice_sm" \
        -type f \( -name '*.h' -o -name '*.c' -o -name 'CMakeLists.txt' \) 2> /dev/null
    find "${FLEX}/src/sm" -maxdepth 1 -type f -name '*.h' 2> /dev/null
    # agent and xApp interfaces
    find "${FLEX}/src/agent" "${FLEX}/src/xApp" -type f \( -name '*.h' -o -name 'CMakeLists.txt' \) 2> /dev/null
    find "${FLEX}/src/xApp" -type f -name '*.c' 2> /dev/null
    # example xApps, in particular any that send control messages
    find "${FLEX}/examples/xApp" -type f \( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' -o -name '*.py' \) 2> /dev/null
    echo "${FLEX}/CMakeLists.txt"
    echo "${FLEX}/examples/CMakeLists.txt"
    # the gNB side of the same interface
    find "${RAN}" -type f \( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' \) 2> /dev/null
    echo "openair2/E2AP/CMakeLists.txt"
} | sort -u > "${LIST}"

mkdir -p "$(dirname "${OUT}")"
tar -czf "${OUT}" --ignore-failed-read -T "${LIST}" 2> /dev/null
{
    echo "OAI    $(git -C "${OAI_SRC}" rev-parse HEAD 2> /dev/null)"
    echo "FlexRIC $(git -C "${OAI_SRC}/${FLEX}" rev-parse HEAD 2> /dev/null)"
    echo "files  $(wc -l < "${LIST}")"
    cat "${LIST}"
} > "${OUT%.tar.gz}.txt"
echo "[EXPORT] $(wc -l < "${LIST}") files -> ${OUT} ($(du -h "${OUT}" | cut -f1)); list: ${OUT%.tar.gz}.txt"
exit 0
