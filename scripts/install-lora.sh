#!/usr/bin/env bash
# install-lora.sh - connect mmd to the Cap LoRa-1262 through MeshZero.
#
# Run on the CardputerZero AFTER install.sh, with MeshZero installed and
# Setup > Network access turned on (Meshtastic TCP API on port 4403):
#
#   sudo bash install-lora.sh
#
# Expects next to it: mesh_relay_tcp.py, mesh_relay.py, setup_mm_channel.py.
# Downloads the 'meshtastic' Python package from PyPI into a venv.
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
RUN_USER="${SUDO_USER:-mark}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN=/opt/monstermesh/bin
STATE=/var/lib/monstermesh

echo "== Copying relay scripts to $BIN"
install -d "$BIN"
for f in mesh_relay_tcp.py mesh_relay.py setup_mm_channel.py; do
    install -m 0755 "$HERE/$f" "$BIN/$f"
done

echo "== Installing the meshtastic Python package (venv in $BIN/.venv)"
[ -x "$BIN/.venv/bin/python3" ] || python3 -m venv "$BIN/.venv"
"$BIN/.venv/bin/pip" install --disable-pip-version-check -q --upgrade meshtastic
"$BIN/.venv/bin/python3" -c "import meshtastic.tcp_interface; print('meshtastic', meshtastic.__version__ if hasattr(meshtastic, '__version__') else 'ok')"

echo "== Pointing mmd at MeshZero (127.0.0.1:4403)"
install -d -o "$RUN_USER" -g "$RUN_USER" "$STATE/saves"
install -d /etc/systemd/system/monstermesh.service.d
cat > /etc/systemd/system/monstermesh.service.d/lora.conf <<EOF
[Service]
# Relay over MeshZero's Meshtastic TCP API. /dev/null is a placeholder port:
# the TCP relay ignores it, but mmd stat()s it before each relaunch.
ExecStart=
ExecStart=$BIN/mmd --relay $BIN/mesh_relay_tcp.py /dev/null $STATE/saves
EOF
systemctl daemon-reload
# Keep mmd off the radio while the channel is provisioned.
systemctl stop monstermesh

echo "== Checking MeshZero's TCP API"
if (exec 3<>/dev/tcp/127.0.0.1/4403) 2>/dev/null; then
    echo "port 4403 is open"
    echo "== Adding the MonsterMesh channel to the node"
    sudo -u "$RUN_USER" "$BIN/.venv/bin/python3" "$BIN/mesh_relay_tcp.py" setup_mm_channel.py /dev/null \
        || echo "channel setup failed - rerun: $BIN/.venv/bin/python3 $BIN/mesh_relay_tcp.py setup_mm_channel.py /dev/null"
else
    echo "port 4403 is closed: open MeshZero, turn on Setup > Network access, then run:"
    echo "  $BIN/.venv/bin/python3 $BIN/mesh_relay_tcp.py setup_mm_channel.py /dev/null"
fi
systemctl start monstermesh

sleep 3
systemctl --no-pager status monstermesh | head -20
