#!/usr/bin/env python3
"""fake_meshtasticd.py - a pretend Meshtastic node for testing src/backend/mesh_link.cpp.

Speaks the client "stream" protocol (0x94 0xC3 len_hi len_lo + protobuf) on a
TCP port of 127.0.0.1, using the REAL protobuf classes from the official
`meshtastic` Python package, so it is an independent check of the app's
hand-written codec. It never touches a radio.

    ~/meshtastic-venv/bin/python3 tools/fake_meshtasticd.py --port 44100 --log /tmp/fake.jsonl

Behaviour
  * want_config_id  -> my_info, (lora config), node_infos, config_complete_id
  * a text packet   -> queueStatus, then a ROUTING ack and (for a direct
                       message) a text reply "re: <text>" from the addressee
  * every ToRadio it decodes is appended to --log as one JSON object per line

Commands: one JSON object per line on stdin, e.g.
  {"cmd":"text","from":572662306,"to":4294967295,"text":"hi all","channel":1}
  {"cmd":"encrypted","from":572662306}
  {"cmd":"nodeinfo","from":1,"long":"Name","short":"NM"}
  {"cmd":"telemetry","from":1,"battery":55}
  {"cmd":"ack","request_id":123,"from":1,"error":8}
  {"cmd":"mode","ack":"normal|nak|silent|implicit|queue_fail","reply":true,"echo":false}
  {"cmd":"garbage"}            debug text, noise and a false frame header
  {"cmd":"raw","hex":"..."}    bytes exactly as given
  {"cmd":"frame","hex":"..."}  the bytes wrapped in a stream header
  {"cmd":"split", ...}         like "text", delivered in three pieces
  {"cmd":"fuzz","seed":1,"count":1000}   mutated valid frames
  {"cmd":"many_nodes","count":600}       that many extra node_infos
  {"cmd":"rebooted"}  {"cmd":"drop"}  {"cmd":"quit"}
Prints "LISTENING <port>" on stdout once it accepts connections.
"""
import argparse
import heapq
import json
import os
import random
import selectors
import socket
import sys
import time

from meshtastic.protobuf import config_pb2, mesh_pb2, portnums_pb2, telemetry_pb2

START1, START2, MAX_LEN = 0x94, 0xC3, 512
BROADCAST = 0xFFFFFFFF
MY_NUM = 0x0A0B0C0D
T0 = 1795000000  # a fixed "now" so the tests can compare last_heard exactly

LONG_NAME = "Ev\x07il\nName " + "x" * 80  # control characters and > 64 bytes

# num, long_name, short_name, snr, hops_away, battery, last_heard
NODES = [
    (MY_NUM, "Pocket 1999", "PK99", 0.0, None, 101, T0),
    (0x11111111, "Alice Base", "ALC", 7.25, 0, 88, T0 - 60),
    (0x22222222, "Bob Mobile", "BOB", -3.5, 2, 15, T0 - 3600),
    (0x33333333, "Zoë \U0001F98A Fuchs", "\U0001F98A", 1.0, 1, 42, T0 - 120),
    (0x44444444, None, None, -12.75, None, None, 0),
    (0x55555555, LONG_NAME, "EVL", 0.5, 3, 0, T0 - 5),
]


def frame(msg) -> bytes:
    b = msg.SerializeToString()
    assert len(b) <= MAX_LEN, len(b)
    return bytes([START1, START2, len(b) >> 8, len(b) & 0xFF]) + b


def raw_frame(b: bytes) -> bytes:
    return bytes([START1, START2, (len(b) >> 8) & 0xFF, len(b) & 0xFF]) + b


class Fake:
    def __init__(self, args):
        self.args = args
        self.log = open(args.log, "a", buffering=1, encoding="utf-8") if args.log else None
        self.sel = selectors.DefaultSelector()
        self.listeners = []
        self.client = None
        self.rx = b""
        self.junk = b""
        self.timers = []  # (due, seq, bytes or callable)
        self.seq = 0
        self.next_id = 0x70000001
        self.mode_ack = "normal"
        self.mode_reply = True
        self.mode_echo = False
        self.running = True
        self.stdin_buf = b""

        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", args.port))
        self.listeners.append(s)
        if args.v6:
            s6 = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
            s6.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            s6.bind(("::1", args.port))
            self.listeners.append(s6)
        for l in self.listeners:
            l.listen(4)
            l.setblocking(False)
            self.sel.register(l, selectors.EVENT_READ, "listen")
        self.sel.register(0, selectors.EVENT_READ, "stdin")

    # ---- plumbing -------------------------------------------------------
    def write_log(self, obj):
        if self.log:
            self.log.write(json.dumps(obj, ensure_ascii=False) + "\n")

    def send(self, data: bytes):
        if self.client is None:
            return
        try:
            self.client.sendall(data)
        except OSError:
            self.drop_client()

    def later(self, delay, what):
        self.seq += 1
        heapq.heappush(self.timers, (time.monotonic() + delay, self.seq, what))

    def drop_client(self):
        if self.client is not None:
            try:
                self.sel.unregister(self.client)
            except (KeyError, ValueError):
                pass
            self.client.close()
            self.client = None
            self.write_log({"ev": "disconnect"})
        self.rx = b""
        self.timers = []

    def flush_junk(self):
        if self.junk:
            self.write_log({"ev": "junk", "n": len(self.junk), "hex": self.junk[:256].hex()})
            self.junk = b""

    # ---- building packets ----------------------------------------------
    def packet(self, frm, to, portnum, payload, channel=0, pid=None, request_id=0,
               hop_start=3, hop_limit=3, snr=5.5, rx_time=T0 + 10):
        fr = mesh_pb2.FromRadio()
        p = fr.packet
        setattr(p, "from", frm)
        p.to = to
        p.channel = channel
        if pid is None:
            pid = self.next_id
            self.next_id += 1
        p.id = pid
        p.rx_time = rx_time
        p.rx_snr = snr
        p.hop_limit = hop_limit
        p.hop_start = hop_start
        p.decoded.portnum = portnum
        p.decoded.payload = payload
        if request_id:
            p.decoded.request_id = request_id
        return fr

    def text_packet(self, c):
        return self.packet(
            c["from"], c.get("to", MY_NUM), portnums_pb2.PortNum.TEXT_MESSAGE_APP,
            c["text"].encode("utf-8"), channel=c.get("channel", 0), pid=c.get("id"),
            hop_start=c.get("hop_start", 3), hop_limit=c.get("hop_limit", 3),
            snr=c.get("snr", 5.5), rx_time=c.get("rx_time", T0 + 10))

    def routing(self, frm, request_id, error):
        r = mesh_pb2.Routing()
        r.error_reason = error
        return self.packet(frm, MY_NUM, portnums_pb2.PortNum.ROUTING_APP,
                           r.SerializeToString(), request_id=request_id)

    # ---- the handshake --------------------------------------------------
    def config_messages(self, config_id):
        out = []
        fr = mesh_pb2.FromRadio()
        fr.my_info.my_node_num = MY_NUM
        fr.my_info.reboot_count = 7
        out.append(fr)
        if self.args.lora_hop_limit:
            fr = mesh_pb2.FromRadio()
            fr.config.lora.hop_limit = self.args.lora_hop_limit
            fr.config.lora.region = config_pb2.Config.LoRaConfig.RegionCode.US
            out.append(fr)
            fr = mesh_pb2.FromRadio()
            fr.config.device.role = config_pb2.Config.DeviceConfig.Role.CLIENT
            out.append(fr)
        for num, long_name, short_name, snr, hops, battery, heard in NODES:
            out.append(self.node_info(num, long_name, short_name, snr, hops, battery, heard))
        fr = mesh_pb2.FromRadio()
        fr.config_complete_id = config_id
        out.append(fr)
        return out

    @staticmethod
    def node_info(num, long_name, short_name, snr, hops, battery, heard):
        fr = mesh_pb2.FromRadio()
        n = fr.node_info
        n.num = num
        if long_name is not None:
            n.user.id = "!%08x" % num
            n.user.long_name = long_name
            n.user.short_name = short_name
            n.user.hw_model = mesh_pb2.HardwareModel.TBEAM
        n.snr = snr
        n.last_heard = heard
        if hops is not None:
            n.hops_away = hops
        if battery is not None:
            n.device_metrics.battery_level = battery
            n.device_metrics.voltage = 4.1
        n.position.latitude_i = 407000000
        return fr

    def send_config(self, config_id):
        for fr in self.config_messages(config_id):
            self.send(frame(fr))

    def fuzz(self, seed, count):
        """Mutated copies of valid FromRadio messages, each in a correct stream header."""
        rnd = random.Random(seed)
        corpus = [m.SerializeToString() for m in self.config_messages(1)]
        corpus.append(self.text_packet({"from": 0x11111111, "text": "fuzz me \U0001F98A"}).SerializeToString())
        corpus.append(self.routing(0x11111111, 77, 0).SerializeToString())
        t = telemetry_pb2.Telemetry()
        t.device_metrics.battery_level = 50
        corpus.append(self.packet(0x22222222, BROADCAST, portnums_pb2.PortNum.TELEMETRY_APP,
                                  t.SerializeToString()).SerializeToString())
        u = mesh_pb2.User()
        u.long_name = "Fuzzy"
        u.short_name = "FZ"
        corpus.append(self.packet(0x22222222, BROADCAST, portnums_pb2.PortNum.NODEINFO_APP,
                                  u.SerializeToString()).SerializeToString())
        fr = mesh_pb2.FromRadio()
        fr.queueStatus.res = 1
        fr.queueStatus.mesh_packet_id = 99
        corpus.append(fr.SerializeToString())
        out = bytearray()
        for _ in range(count):
            b = bytearray(rnd.choice(corpus))
            for _ in range(rnd.randrange(1, 4)):
                op = rnd.randrange(5)
                if op == 0 and b:
                    b[rnd.randrange(len(b))] = rnd.randrange(256)
                elif op == 1 and b:
                    b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
                elif op == 2 and b:
                    del b[rnd.randrange(len(b)):]
                elif op == 3:
                    pos = rnd.randrange(len(b) + 1)
                    b[pos:pos] = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 9)))
                elif op == 4 and len(b) > 2:
                    i = rnd.randrange(len(b) - 1)
                    del b[i:i + rnd.randrange(1, 4)]
            out += raw_frame(bytes(b[:MAX_LEN]))
            if rnd.randrange(20) == 0:
                out += bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 12)))
        self.send(bytes(out))

    # ---- ToRadio --------------------------------------------------------
    def handle_to_radio(self, body: bytes):
        self.flush_junk()
        tr = mesh_pb2.ToRadio()
        try:
            tr.ParseFromString(body)
        except Exception as ex:  # pylint: disable=broad-except
            self.write_log({"ev": "bad_toradio", "hex": body.hex(), "error": str(ex)})
            return
        variant = tr.WhichOneof("payload_variant")
        rec = {"ev": "toradio", "variant": variant, "raw_hex": body.hex()}
        if variant == "want_config_id":
            rec["id"] = tr.want_config_id
            self.write_log(rec)
            self.send_config(tr.want_config_id)
        elif variant == "heartbeat":
            rec["nonce"] = tr.heartbeat.nonce
            self.write_log(rec)
        elif variant == "disconnect":
            rec["disconnect"] = tr.disconnect
            self.write_log(rec)
        elif variant == "packet":
            p = tr.packet
            which = p.WhichOneof("payload_variant")
            rec.update({
                "from": getattr(p, "from"), "to": p.to, "channel": p.channel, "id": p.id,
                "want_ack": p.want_ack, "hop_limit": p.hop_limit, "hop_start": p.hop_start,
                "priority": p.priority, "payload_variant": which,
                "portnum": p.decoded.portnum if which == "decoded" else -1,
                "payload_hex": p.decoded.payload.hex() if which == "decoded" else "",
                "want_response": p.decoded.want_response if which == "decoded" else False,
            })
            if which == "decoded" and p.decoded.portnum == portnums_pb2.PortNum.TEXT_MESSAGE_APP:
                try:
                    rec["text"] = p.decoded.payload.decode("utf-8")
                except UnicodeDecodeError:
                    rec["text_invalid"] = True
            self.write_log(rec)
            if "text" in rec:
                self.on_text(p, rec["text"])
        else:
            self.write_log(rec)

    def on_text(self, p, text):
        fr = mesh_pb2.FromRadio()
        fr.queueStatus.res = mesh_pb2.Routing.Error.NO_CHANNEL if self.mode_ack == "queue_fail" else 0
        fr.queueStatus.free = 15
        fr.queueStatus.maxlen = 16
        fr.queueStatus.mesh_packet_id = p.id
        self.send(frame(fr))
        if self.mode_echo:
            echo = mesh_pb2.FromRadio()
            echo.packet.CopyFrom(p)
            setattr(echo.packet, "from", MY_NUM)
            self.send(frame(echo))
        direct = p.to != BROADCAST
        if self.mode_ack == "normal":
            # a broadcast gets the implicit ack from our own radio
            self.later(0.03, frame(self.routing(p.to if direct else MY_NUM, p.id, 0)))
        elif self.mode_ack == "nak":
            self.later(0.03, frame(self.routing(MY_NUM, p.id, mesh_pb2.Routing.Error.NO_RESPONSE)))
        elif self.mode_ack == "implicit":
            self.later(0.03, frame(self.routing(MY_NUM, p.id, 0)))
        if direct and self.mode_reply and self.mode_ack == "normal":
            reply = self.packet(p.to, MY_NUM, portnums_pb2.PortNum.TEXT_MESSAGE_APP,
                                ("re: " + text).encode("utf-8"), channel=p.channel,
                                hop_start=3, hop_limit=2, snr=6.5, rx_time=T0 + 20)
            self.later(0.08, frame(reply))

    def feed(self, data: bytes):
        self.rx += data
        while self.rx:
            if self.rx[0] != START1:
                self.junk += self.rx[:1]
                self.rx = self.rx[1:]
                continue
            if len(self.rx) < 2:
                return
            if self.rx[1] != START2:
                self.junk += self.rx[:1]
                self.rx = self.rx[1:]
                continue
            if len(self.rx) < 4:
                return
            n = (self.rx[2] << 8) | self.rx[3]
            if n > MAX_LEN:
                self.junk += self.rx[:1]
                self.rx = self.rx[1:]
                continue
            if len(self.rx) < 4 + n:
                return
            body, self.rx = self.rx[4:4 + n], self.rx[4 + n:]
            self.handle_to_radio(body)

    # ---- commands -------------------------------------------------------
    def command(self, c):
        cmd = c.get("cmd")
        self.write_log({"ev": "command", "cmd": cmd})
        if cmd == "text":
            self.send(frame(self.text_packet(c)))
        elif cmd == "split":
            b = frame(self.text_packet(c))
            self.send(b[:3])
            self.later(0.05, b[3:9])
            self.later(0.10, b[9:])
        elif cmd == "encrypted":
            fr = mesh_pb2.FromRadio()
            p = fr.packet
            setattr(p, "from", c["from"])
            p.to = c.get("to", BROADCAST)
            p.id = self.next_id
            self.next_id += 1
            p.encrypted = bytes(range(40))
            p.rx_time = c.get("rx_time", T0 + 30)
            p.rx_snr = c.get("snr", -7.75)
            p.hop_start = c.get("hop_start", 3)
            p.hop_limit = c.get("hop_limit", 3)
            self.send(frame(fr))
        elif cmd == "nodeinfo":
            u = mesh_pb2.User()
            u.id = "!%08x" % c["from"]
            u.long_name = c["long"]
            u.short_name = c["short"]
            self.send(frame(self.packet(c["from"], BROADCAST, portnums_pb2.PortNum.NODEINFO_APP,
                                        u.SerializeToString())))
        elif cmd == "telemetry":
            t = telemetry_pb2.Telemetry()
            t.time = T0
            t.device_metrics.battery_level = c["battery"]
            t.device_metrics.voltage = 3.9
            self.send(frame(self.packet(c["from"], BROADCAST, portnums_pb2.PortNum.TELEMETRY_APP,
                                        t.SerializeToString())))
        elif cmd == "ack":
            self.send(frame(self.routing(c.get("from", MY_NUM), c["request_id"], c.get("error", 0))))
        elif cmd == "mode":
            self.mode_ack = c.get("ack", self.mode_ack)
            self.mode_reply = c.get("reply", self.mode_reply)
            self.mode_echo = c.get("echo", self.mode_echo)
        elif cmd == "garbage":
            self.send(b"INFO  | 12:00:01 123 [Router] debug text from the node\r\n")
            self.send(bytes([0x00, 0xFF, 0x94, 0x94, 0x12, 0xC3, 0xC3, 0x94]))
            self.send(bytes([START1, START2, 0x7F, 0xFF]))        # impossible length
            self.send(bytes([START1, START2, 0x00, 0x10]) + b"\xff" * 6)  # false header
        elif cmd == "raw":
            self.send(bytes.fromhex(c["hex"]))
        elif cmd == "frame":
            self.send(raw_frame(bytes.fromhex(c["hex"])))
        elif cmd == "fuzz":
            self.fuzz(c.get("seed", 1), c.get("count", 1000))
        elif cmd == "many_nodes":
            out = bytearray()
            for i in range(c.get("count", 600)):
                out += frame(self.node_info(0x90000000 + i, "Node %d" % i, "N%d" % (i % 100), 1.0, 1, 50, T0))
            self.send(bytes(out))
        elif cmd == "rebooted":
            fr = mesh_pb2.FromRadio()
            fr.rebooted = True
            self.send(frame(fr))
        elif cmd == "drop":
            self.drop_client()
        elif cmd == "quit":
            self.running = False

    def on_stdin(self):
        data = os.read(0, 65536)
        if not data:
            self.running = False  # our parent went away
            return
        self.stdin_buf += data
        while b"\n" in self.stdin_buf:
            line, self.stdin_buf = self.stdin_buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                self.command(json.loads(line.decode("utf-8")))
            except Exception as ex:  # pylint: disable=broad-except
                self.write_log({"ev": "bad_command", "line": line.decode("utf-8", "replace"), "error": repr(ex)})

    # ---- main loop ------------------------------------------------------
    def run(self):
        print("LISTENING %d" % self.args.port, flush=True)
        while self.running:
            timeout = 0.5
            if self.timers:
                timeout = max(0.0, min(timeout, self.timers[0][0] - time.monotonic()))
            for key, _ in self.sel.select(timeout):
                if key.data == "listen":
                    conn, _addr = key.fileobj.accept()
                    self.drop_client()
                    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    self.client = conn
                    self.sel.register(conn, selectors.EVENT_READ, "client")
                    self.write_log({"ev": "connect"})
                elif key.data == "stdin":
                    self.on_stdin()
                elif key.data == "client" and key.fileobj is self.client:
                    try:
                        data = self.client.recv(65536)
                    except OSError:
                        data = b""
                    if not data:
                        self.flush_junk()
                        self.drop_client()
                    else:
                        self.feed(data)
            now = time.monotonic()
            while self.timers and self.timers[0][0] <= now:
                _, _, what = heapq.heappop(self.timers)
                self.send(what)
        self.flush_junk()
        self.drop_client()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--log", default="")
    ap.add_argument("--lora-hop-limit", type=int, default=0,
                    help="send a LoRa config with this hop_limit during the handshake (0 = send no config)")
    ap.add_argument("--v6", action="store_true", help="also listen on ::1")
    args = ap.parse_args()
    try:
        Fake(args).run()
    except OSError as ex:
        print("ERROR %s" % ex, flush=True)
        sys.exit(2)


if __name__ == "__main__":
    main()
