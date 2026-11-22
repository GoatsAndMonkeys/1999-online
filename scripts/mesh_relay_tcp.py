#!/usr/bin/env python3
"""
mesh_relay_tcp.py - run MonsterMesh's Meshtastic helper scripts against a
                    Meshtastic node over TCP instead of USB serial.

On the CardputerZero the Cap LoRa-1262 is a bare SX1262 on SPI, driven by
MeshZero (a meshtasticd-compatible node service). MeshZero serves the standard
Meshtastic TCP API on port 4403 (Setup > Network access). mmd only knows serial
radios or a relay subprocess, so this wrapper swaps the meshtastic library's
SerialInterface for TCPInterface and then runs the unmodified script.

  mmd --relay /opt/monstermesh/bin/mesh_relay_tcp.py /dev/null
      -> runs mesh_relay.py (next to this file) over TCP. The port argument
         is ignored; it only has to be a path that exists, because mmd
         stat()s it before relaunching the relay.

  mesh_relay_tcp.py setup_mm_channel.py /dev/null
      -> runs any other helper (here: provision the MonsterMesh channel).

Env: MM_MESHTASTIC_HOST (default 127.0.0.1), MM_MESHTASTIC_PORT (default 4403).
"""

import os
import runpy
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def log(msg):
    print(f"[relay-tcp] {msg}", file=sys.stderr, flush=True)


# Same venv convention as the wrapped scripts: re-exec into ./.venv if the
# system python has no meshtastic. Must happen here, before patching, or the
# wrapped script's own re-exec would drop the patch.
try:
    import meshtastic  # noqa: F401
except ImportError:
    py = os.path.join(HERE, ".venv", "bin", "python3")
    if os.path.exists(py) and os.path.realpath(sys.executable) != os.path.realpath(py):
        os.execv(py, [py] + sys.argv)
    log("FATAL: python package 'meshtastic' not found (expected in .venv)")
    sys.exit(1)

import meshtastic.serial_interface  # noqa: E402
import meshtastic.tcp_interface     # noqa: E402

HOST = os.environ.get("MM_MESHTASTIC_HOST", "127.0.0.1")
PORT = int(os.environ.get("MM_MESHTASTIC_PORT", "4403"))


def _tcp_interface(*_args, **_kwargs):
    log(f"Connecting via TCP: {HOST}:{PORT}")
    return meshtastic.tcp_interface.TCPInterface(HOST, portNumber=PORT)


meshtastic.serial_interface.SerialInterface = _tcp_interface


# mesh_relay.py idles forever once connected and never notices a dropped
# socket (MeshZero restarted, network access toggled off). Exit instead, so
# mmd's reconnect loop relaunches the relay every few seconds.
def _on_connection_lost(interface=None):
    log("TCP connection lost - exiting so mmd relaunches the relay")
    os._exit(1)


from pubsub import pub  # noqa: E402  (a meshtastic dependency)

pub.subscribe(_on_connection_lost, "meshtastic.connection.lost")

args = sys.argv[1:]
target = os.path.join(HERE, "mesh_relay.py")
if args and args[0].endswith(".py"):
    target = args.pop(0)
    if not os.path.isabs(target):
        target = os.path.join(HERE, target)
port_arg = args[0] if args else "/dev/null"

sys.argv = [target, port_arg] + args[1:]
runpy.run_path(target, run_name="__main__")
