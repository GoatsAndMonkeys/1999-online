#!/usr/bin/env bash
# Build the CardputerZero arm64 .deb for 1999 Online in a Debian trixie arm64
# container.
#
#   scripts/build-deb.sh [extra cmake -D args...]
#
# Env:
#   MM_ROOT       the MonsterMesh Pi sources (shared game logic, read-only);
#                 default ../pokemesh/monster_mesh_pi next to this project
#   MM_CP0_CACHE  persistent cache for the CM0 BSP + deps (default ~/.cache/monstermesh-cp0)
#   MM_DEB_OUT    output directory for the .deb (default dist/)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
MM_ROOT="${MM_ROOT:-$(cd "$HERE/../pokemesh/monster_mesh_pi" && pwd)}"
CACHE="${MM_CP0_CACHE:-$HOME/.cache/monstermesh-cp0}"
OUT="${MM_DEB_OUT:-$HERE/dist}"
IMAGE="monstermesh-cp0-builder:trixie"

mkdir -p "$CACHE" "$OUT"
# Reuse the builder image when it is already here (set MM_REBUILD_IMAGE=1 to refresh it).
if [ -n "${MM_REBUILD_IMAGE:-}" ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/arm64 -t "$IMAGE" -f "$HERE/docker/Dockerfile.cp0" "$HERE/docker"
fi
docker run --rm --platform linux/arm64 \
    -v "$MM_ROOT":/mm:ro \
    -v "$HERE":/app:ro \
    -v "$CACHE":/cache \
    -v "$OUT":/out \
    -e EXTRA_CMAKE_ARGS="$*" \
    "$IMAGE" /bin/bash /app/docker/build-in-container.sh
echo "deb(s) in $OUT"
