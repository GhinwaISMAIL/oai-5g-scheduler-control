#!/bin/bash
# =============================================================================
# build-gnb-image.sh — build the gNB image with the controllable DL scheduler
#
# Usage: sudo bash gnb/build-gnb-image.sh [IMAGE_TAG]
#
# Environment
#   OAI_SRC_DIR   working copy of the OAI sources   (default /opt/oai-gnb-src)
#   PUSH_TO       registry name:tag to push the result to (needs docker login)
#   KEEP_BUILD_CACHE=1  keep BuildKit cache after the build
#
# The image is built from the stock OAI Dockerfiles at the pinned commit, with
# gnb/patches/*.patch applied and gnb/src/*.{c,h} added to the MAC directory.
# The default tag contains the OAI commit and a hash of everything in gnb/src and
# gnb/patches, so a changed source gives a different tag.  Takes tens of minutes
# the first time (UHD and the full OAI build); later builds reuse the compiler cache.
# Output: the image tag on the last line of stdout, the log in /local/logs/gnb-build.log.
# =============================================================================
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OAI_SRC_DIR="${OAI_SRC_DIR:-/opt/oai-gnb-src}"
LOG_DIR="${LOG_DIR:-/local/logs}"
mkdir -p "${LOG_DIR}"
exec > >(tee -a "${LOG_DIR}/gnb-build.log") 2>&1

fail() { echo "[GNB-BUILD] ERROR: $*" >&2; exit 1; }

OAI_COMMIT="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["oai_revision"])' "${REPO_DIR}/etc/stack-versions.json")" \
    || fail "cannot read oai_revision from etc/stack-versions.json"
SRC_HASH="$(cd "${REPO_DIR}/gnb" && cat src/* patches/* | sha256sum | cut -c1-12)"
IMAGE="${1:-ric5g/oai-gnb-ctrl:${OAI_COMMIT:0:8}-${SRC_HASH}}"

push_image() {
    [ -n "${PUSH_TO:-}" ] || return 0
    docker tag "${IMAGE}" "${PUSH_TO}" && docker push "${PUSH_TO}" || fail "push to ${PUSH_TO} failed (docker login first)"
    docker image inspect --format '[GNB-BUILD] pushed digest: {{index .RepoDigests 0}}' "${PUSH_TO}" || true
}

if docker image inspect "${IMAGE}" > /dev/null 2>&1 && [ "${FORCE_REBUILD:-0}" != 1 ]; then
    echo "[GNB-BUILD] ${IMAGE} already exists; nothing to build."
    push_image
    echo "${IMAGE}"
    exit 0
fi

echo "[GNB-BUILD] $(date) OAI=${OAI_COMMIT} sources=${SRC_HASH} image=${IMAGE}"
command -v docker > /dev/null || fail "docker is not installed"
command -v git > /dev/null || fail "git is not installed"
export DOCKER_BUILDKIT=1

# --- source tree -------------------------------------------------------------
if [ ! -d "${OAI_SRC_DIR}/.git" ]; then
    git clone https://gitlab.eurecom.fr/oai/openairinterface5g.git "${OAI_SRC_DIR}" || fail "OAI clone failed"
fi
git -C "${OAI_SRC_DIR}" fetch --quiet origin 2> /dev/null || true
git -C "${OAI_SRC_DIR}" reset --hard --quiet
git -C "${OAI_SRC_DIR}" clean -fdq -e openair2/E2AP/flexric
git -C "${OAI_SRC_DIR}" checkout --detach --quiet "${OAI_COMMIT}" || fail "checkout ${OAI_COMMIT} failed"
git -C "${OAI_SRC_DIR}" submodule update --init --recursive || fail "submodule update failed"
[ "$(git -C "${OAI_SRC_DIR}" rev-parse HEAD)" = "${OAI_COMMIT}" ] || fail "OAI is not at ${OAI_COMMIT}"

# --- scheduler sources -------------------------------------------------------
MAC_DIR="${OAI_SRC_DIR}/openair2/LAYER2/NR_MAC_gNB"
for p in "${REPO_DIR}"/gnb/patches/*.patch; do
    git -C "${OAI_SRC_DIR}" apply --check "${p}" || fail "patch ${p} does not apply to ${OAI_COMMIT}"
    git -C "${OAI_SRC_DIR}" apply "${p}" || fail "patch ${p} failed"
done
cp "${REPO_DIR}"/gnb/src/*.c "${REPO_DIR}"/gnb/src/*.h "${MAC_DIR}/"

# --- images (OAI's documented three-step build) ---------------------------------
cd "${OAI_SRC_DIR}"
docker build --target ran-base  --tag ran-base:latest  --file docker/Dockerfile.base.ubuntu . || fail "ran-base build failed"
docker build --target ran-build --tag ran-build:latest --file docker/Dockerfile.build.ubuntu . || fail "ran-build failed (compile error: see the log above)"
docker build --target oai-gnb   --tag "${IMAGE}"       --file docker/Dockerfile.gNB.ubuntu .   || fail "gNB image build failed"

# --- check that the binary carries the controller ------------------------------
docker run --rm --entrypoint /bin/sh "${IMAGE}" -c 'grep -q "SCHED_CTRL" /opt/oai-gnb/bin/nr-softmodem' \
    || fail "built image does not contain the controllable scheduler"

push_image
[ "${KEEP_BUILD_CACHE:-1}" = 1 ] || docker builder prune -f > /dev/null
echo "[GNB-BUILD] done $(date)"
echo "${IMAGE}"
