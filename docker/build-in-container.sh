#!/usr/bin/env bash
# Runs inside the cp0 builder container (see scripts/build-deb.sh).
#   /mm    = monster_mesh_pi: shared game sources (read-only)
#   /app   = this project (read-only)
#   /cache = persistent cache: BSP sysroot + fetched deps
#   /out   = where the .deb lands
set -euo pipefail

mkdir -p /work/monster_mesh_pi
# Only the two trees the build needs; never the host's build/.cache/dist dirs.
# The source is often an SMB share: skip its .smbdelete* temp files, and treat
# rsync's "some files vanished" (24) as success.
sync_tree() {
    rsync -a --delete --exclude '.smbdelete*' --exclude '._*' "$@" || [ $? -eq 24 ]
}
sync_tree /mm/src/ /work/monster_mesh_pi/src/
# The preset pack and screenshots are not part of the package.
sync_tree --exclude build/ --exclude .cache/ --exclude dist/ --exclude assets/milkdrop/ \
    --exclude docs/screenshots/ /app/ /work/monster_mesh_pi/cardputer/

cd /work/monster_mesh_pi/cardputer
mkdir -p /cache/cp0
ln -sfn /cache/cp0 .cache

# Reuse a previously downloaded BSP archive instead of fetching 400+ MB again.
if [ ! -d .cache/sdk_bsp-src ] && [ -f .cache/sdk_bsp.tar.gz ]; then
    echo "Extracting cached CM0 BSP"
    mkdir -p .cache/sdk_bsp-src
    tar xzf .cache/sdk_bsp.tar.gz -C .cache/sdk_bsp-src
fi

# shellcheck disable=SC2086
cmake --preset cp0-cross -DMM_ROOT=/work/monster_mesh_pi ${EXTRA_CMAKE_ARGS:-}
cmake --build --preset cp0-cross-rel -j"$(nproc)"
rm -rf dist
cpack --preset cp0-cross-deb

cp -v dist/*.deb /out/
for deb in dist/*.deb; do
    echo "== $deb"
    dpkg-deb -I "$deb"
    dpkg-deb -c "$deb"
done
file build/cp0-cross/Release/1999online
