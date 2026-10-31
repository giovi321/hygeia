#!/usr/bin/env python3
"""Development server that stands in for the device while the web UI is built.

    python tools/webmock.py [--port 8484] [--unpaired] [--slow] [--stale] [--clock-invalid] [--skew SECONDS] [--zone NAME] [--busy]
                            [--low-battery] [--ota-pending]

Serves the bundle made by tools/web_bundle.py from web/ (rebuilt on every request, so edits show on reload) and
answers /api/* from the sample files in tools/webmock/samples/ plus in-memory changes. Routes the firmware does not
implement yet answer 501 like the device. Nothing is stored on disk. Export and import follow the device shapes
(import works on the in-memory data; nothing is written to disk). POST /api/ota checks the image header like the
device, answers {"reboot_in_s":2}, drops every request from 2 s to 7 s later (the restart) and then reports the
uploaded image's version in /api/info.

  --unpaired       every /api route except info and pairing answers 401 until the code 123456 is posted
  --slow           adds 1.5 s to every /api answer
  --stale          PUT /api/medicines/{id} answers 412
  --clock-invalid  starts with an invalid clock until POST /api/time
  --skew SECONDS   the device clock runs this many seconds ahead of the host
  --zone NAME      the device time zone (default Europe/Zurich)
  --busy           writes answer 503 busy; the first GET of each read route answers 503 once
  --low-battery    POST /api/ota answers 409 battery (on battery below 30 %)
  --ota-pending    POST /api/ota answers 503 busy (the running firmware has not confirmed itself yet)
"""

import argparse
import base64
import hashlib
import json
import os
import re
import struct
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import web_bundle  # noqa: E402

SRC = os.path.join(os.path.dirname(HERE), "web")
SAMPLES = os.path.join(HERE, "webmock", "samples")
SAMPLE_NOW = 1793181600     # utc_ms / 1000 in samples/state.json
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PAIR_CODE = "123456"
OTA_SLOT = 3 * 1024 * 1024  # the size of one firmware slot in partitions.csv
OTA_RESTART_S = 2           # {"reboot_in_s":2}
OTA_BOOT_S = 5              # the mock is unreachable this long after the restart starts
BUILD_EPOCH = 1700000000


def sample(name):
    with open(os.path.join(SAMPLES, name), "r", encoding="utf-8") as f:
        return json.load(f)


class Device:
    """The in-memory device."""

    def __init__(self, args):
        self.args = args
        self.lock = threading.RLock()
        self.meds = sample("meds.json")["meds"]
        self.meds.append({
            "id": 4, "code": "Z", "name": "Zinc", "dose": "1 tab", "color": 8,
            "schedule": {"type": "fixed", "times": ["12:00"], "weekdays": 127, "min_interval_min": 0},
            "early_window_min": 60, "late_after_min": 30, "missed_after_min": 120,
            "pill_count": {"enabled": False, "remaining": 0, "alert": 0}, "archived": True, "order": 3})
        for m in self.meds:
            m["etag"] = self.etag(m)
        self.next_id = 5
        shift = int(time.time()) - SAMPLE_NOW
        self.log = sample("log.json")["log"]
        for e in self.log:
            e["ts"] += shift
            if "slot" in e:
                e["slot"] += shift
        self.log.append({"id": 4, "ts": SAMPLE_NOW + shift - 86400 * 20, "med_id": 4, "status": "taken",
                         "source": "screen", "edited": False})
        self.log.append({"id": 5, "ts": SAMPLE_NOW + shift - 86400 * 21, "med_id": 4, "status": "taken",
                         "source": "screen", "edited": False})
        self.next_log = 6
        self.settings = sample("settings.json")
        self.wifi = {"mode": "client", "ssid": "HomeNet", "host": "hygeia", "networks": ["HomeNet"]}
        st = sample("state.json")
        self.state = st
        self.shift = shift
        for t in st["tiles"]:
            for k in ("last_taken", "next_due"):
                if t.get(k):
                    t[k] += shift
        self.due = st["due"]
        for d in self.due:
            d["slot"] += shift
        self.clock_valid = not args.clock_invalid
        self.skew = args.skew
        self.zone = args.zone or st["zone"]
        self.busy_seen = set()
        self.sync = {"src": "browser", "at": int(time.time()) - 180}
        self.sessions = set()
        self.fails = 0
        self.pair_until = 0
        self.blocked_until = 0
        self.clients = []
        self.fw = "0.0.0-mock"
        self.restart_at = 0.0   # an upload finished: unreachable from restart_at for OTA_BOOT_S
        self.next_fw = None
        self.ota_busy = False   # an upload body is being read (outside the device lock)
        self.browsers = [{"index": 0, "label": "Firefox on Windows", "created": SAMPLE_NOW - 86400 * 9, "current": True},
                         {"index": 1, "label": "Safari on iPhone", "created": SAMPLE_NOW - 86400 * 30, "current": False}]

    def restarting(self):
        """True while the mock plays the restart after an upload; the new version shows once it is over."""
        with self.lock:
            if not self.restart_at:
                return False
            t = time.time()
            if t < self.restart_at:
                return False
            if t < self.restart_at + OTA_BOOT_S:
                return True
            self.fw, self.next_fw, self.restart_at = self.next_fw, None, 0.0
            self.clients = []
            return False

    @staticmethod
    def etag(m):
        keep = {k: v for k, v in m.items() if k not in ("etag", "order", "archived")}
        keep["pill_count"] = {k: v for k, v in m["pill_count"].items() if k != "remaining"}
        return "%08x" % (zlib.crc32(json.dumps(keep, sort_keys=True).encode()) & 0xFFFFFFFF)

    def now_ms(self):
        return int((time.time() + self.skew) * 1000)

    def med(self, mid):
        for m in self.meds:
            if m["id"] == mid:
                return m
        return None

    def tiles(self):
        now = int(time.time())
        due = {d["med_id"]: d for d in self.due}
        out = []
        for m in sorted((m for m in self.meds if not m["archived"]), key=lambda m: m["order"]):
            last = [e["ts"] for e in self.log if e["med_id"] == m["id"] and e["status"] == "taken"]
            t = {"id": m["id"], "code": m["code"], "name": m["name"], "color": m["color"],
                 "kind": "since", "ring": 500, "last_taken": max(last) if last else None,
                 "next_due": now + 3 * 3600, "pills": m["pill_count"]["enabled"],
                 "pills_left": m["pill_count"]["remaining"] if m["pill_count"]["enabled"] else None}
            if m["id"] in due:
                d = due[m["id"]]
                t["kind"] = "snoozed" if d["state"] == "snoozed" else d["state"]
                t["ring"] = 1000
                t["next_due"] = None
            elif m["schedule"]["type"] == "as_needed":
                t["kind"], t["ring"], t["next_due"] = "as_needed", 1000, None
            elif not m["schedule"]["times"]:
                t["kind"], t["next_due"] = "no_dose", None
            out.append(t)
        return out

    def broadcast(self, ev):
        frame = ws_frame(json.dumps({"ev": ev, "ts": int(time.time()), "med_id": 0, "log_id": 0, "slot": 0,
                                     "value": 0}))
        for c in list(self.clients):
            try:
                c.sendall(frame)
            except OSError:
                self.clients.remove(c)


def ws_frame(text):
    data = text.encode()
    n = len(data)
    head = b"\x81" + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n))
    return head + data


def w_list(nets):
    return [{"ssid": n, "last_ok": i + 1} for i, n in enumerate(nets)]


def with_local(e):
    out = dict(e)
    out["local"] = time.strftime("%Y-%m-%d %H:%M", time.gmtime(e["ts"]))
    return out


class Raw:
    """A non-JSON answer body (export downloads)."""

    def __init__(self, data, ctype):
        self.data = data
        self.ctype = ctype


def err(status, code, field, message, **extra):
    body = {"error": code, "field": field, "message": message}
    body.update(extra)
    return status, body


def validate_med(dev, body, mid):
    code = body.get("code", "")
    if not isinstance(code, str) or not re.fullmatch(r"[A-Z0-9]{1,2}", code):
        return err(422, "code", "code", "The code is 1 or 2 letters or digits.")
    name = body.get("name", "")
    if not isinstance(name, str) or not name.strip():
        return err(422, "name", "name", "The name is empty.")
    if len(name.encode()) > 32:
        return err(422, "name", "name", "The name is too long.")
    dose = body.get("dose", "")
    if not isinstance(dose, str) or len(dose.encode()) > 16:
        return err(422, "dose", "dose", "The dose is too long.")
    sch = body.get("schedule") or {}
    times = sch.get("times", [])
    if sch.get("type") == "fixed":
        if not times or len(times) > 8 or len(set(times)) != len(times) \
                or not all(re.fullmatch(r"([01]\d|2[0-3]):[0-5]\d", t or "") for t in times):
            return err(422, "times", "times", "Give 1 to 8 different times.")
        if not sch.get("weekdays"):
            return err(422, "times", "weekdays", "Pick at least one day.")
    for m in dev.meds:
        if m["code"] == code and not m["archived"] and m["id"] != mid and not body.get("archived"):
            return err(409, "code_dup", "code", "Another active medicine already uses this code.")
    return None


def apply_med(m, body):
    for k in ("code", "name", "dose", "color", "early_window_min", "late_after_min", "missed_after_min"):
        if k in body:
            m[k] = body[k]
    if "schedule" in body:
        s = body["schedule"]
        m["schedule"] = {"type": s["type"], "times": s.get("times", []),
                         "weekdays": s.get("weekdays", 127), "min_interval_min": s.get("min_interval_min", 0)}
    if "pill_count" in body:
        m["pill_count"] = dict(body["pill_count"])
    for k in ("active_from", "active_until"):
        if body.get(k):
            m[k] = body[k]
        else:
            m.pop(k, None)
    m["etag"] = Device.etag(m)


class Handler(BaseHTTPRequestHandler):
    server_version = "hygeia-mock"
    dev = None

    def log_message(self, fmt, *a):
        if os.environ.get("WEBMOCK_VERBOSE"):
            sys.stderr.write("%s %s\n" % (self.command, self.path))

    # ---- plumbing ----
    def send(self, status, body=None, ctype="application/json", headers=None, raw=None):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else b"")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def session(self):
        for part in (self.headers.get("Cookie") or "").split(";"):
            k, _, v = part.strip().partition("=")
            if k == "hy_s":
                return v
        return None

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except ValueError:
            return None

    def do_HEAD(self):
        self.handle_any()

    def do_GET(self):
        self.handle_any()

    do_POST = do_PUT = do_DELETE = do_GET

    def handle_any(self):
        u = urlsplit(self.path)
        if self.dev.restarting():
            self.close_connection = True   # the device is restarting: no answer at all
            return
        if u.path == "/ws":
            return self.ws()
        if u.path.startswith("/api/"):
            if self.dev.args.slow:
                time.sleep(1.5)
            self.ota_body = self.read_ota_body(u.path)
            with self.dev.lock:
                try:
                    status, body, headers = self.api(u.path, parse_qs(u.query))
                except Exception as e:  # keep the mock alive, say what broke
                    status, body, headers = 500, {"error": "internal", "field": None, "message": repr(e)}, {}
            if isinstance(body, Raw):
                return self.send(status, raw=body.data, ctype=body.ctype, headers=headers)
            return self.send(status, body, headers=headers)
        if self.command not in ("GET", "HEAD"):
            return self.send(405, {"error": "method", "field": None, "message": "Method not allowed"})
        try:
            assets = {a.path: a for a in web_bundle.build(SRC)}
        except web_bundle.BundleError as e:
            return self.send(500, raw=str(e).encode(), ctype="text/plain")
        path = "/index.html" if u.path == "/" else u.path
        a = assets.get(path)
        if not a:
            return self.send(404, raw=b"not found", ctype="text/plain")
        return self.send(200, raw=a.gz, ctype=a.mime, headers={"Content-Encoding": "gzip"})

    # ---- websocket ----
    def ws(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if os.environ.get("WEBMOCK_VERBOSE"):
            print("ws upgrade, origin", self.headers.get("Origin"), file=sys.stderr, flush=True)
        if not key or (self.dev.args.unpaired and self.session() not in self.dev.sessions):
            return self.send(401, {"error": "unpaired", "field": None, "message": "This browser is not paired"})
        acc = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        # browsers insist on HTTP/1.1 in the upgrade answer
        head = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        self.wfile.write((head + "Sec-WebSocket-Accept: " + acc + "\r\n\r\n").encode())
        self.wfile.flush()
        sock = self.connection
        self.dev.clients.append(sock)
        try:
            while True:    # read and drop client frames until close
                head = self.rfile.read(2)
                if len(head) < 2:
                    break
                op, ln = head[0] & 0x0F, head[1] & 0x7F
                if ln == 126:
                    ln = struct.unpack(">H", self.rfile.read(2))[0]
                elif ln == 127:
                    ln = struct.unpack(">Q", self.rfile.read(8))[0]
                if head[1] & 0x80:
                    self.rfile.read(4)
                self.rfile.read(ln)
                if op == 8:
                    break
        except OSError:
            pass
        finally:
            if sock in self.dev.clients:
                self.dev.clients.remove(sock)
            self.close_connection = True

    # ---- api ----
    def api(self, path, q):
        dev = self.dev
        m = self.command
        open_route = path in ("/api/info", "/api/pair/start", "/api/pair")
        if m not in ("GET", "HEAD") and self.headers.get("X-Hygeia") != "1":
            return 403, err(403, "forbidden", None, "Write requests need X-Hygeia: 1")[1], {}
        if dev.args.unpaired and not open_route and self.session() not in dev.sessions:
            return 401, err(401, "unpaired", None, "This browser is not paired")[1], {}
        if dev.args.busy and open_route is False:
            if m != "GET" and path != "/api/time":
                return 503, err(503, "busy", None, "The device is busy, try again")[1], {}
            if m == "GET" and path not in dev.busy_seen:
                dev.busy_seen.add(path)
                return 503, err(503, "busy", None, "The device is busy, try again")[1], {}
        if path == "/api/import" and m == "POST":
            return self.backup_import(q)
        if path == "/api/ota" and m == "POST":
            return self.ota()
        body = self.read_body() if m in ("POST", "PUT", "DELETE") else {}
        if body is None:
            return 400, err(400, "invalid", None, "The body is not valid JSON")[1], {}
        r = self.route(m, path, q, body)
        if len(r) == 2:
            return r[0], r[1], {}
        return r

    def route(self, m, path, q, body):
        dev = self.dev
        now = int(time.time())

        extra = self.route_extra(m, path, q, body, now)
        if extra is not None:
            return extra

        if path == "/api/info" and m == "GET":
            return 200, {"api": 1, "fw": dev.fw, "name": "hygeia",
                         "paired": (not dev.args.unpaired) or self.session() in dev.sessions,
                         "utc_ms": dev.now_ms(), "clock_valid": dev.clock_valid}

        if path == "/api/pair/start" and m == "POST":
            if dev.blocked_until > now:
                return 429, err(429, "blocked", None, "Too many wrong codes, try again in a minute")[1], \
                    {"Retry-After": str(dev.blocked_until - now)}
            dev.pair_until = now + 120
            return 200, {"expires_in": 120}

        if path == "/api/pair" and m == "POST":
            if dev.pair_until <= now:
                return 404, err(404, "not_found", "code", "No pairing code is running: start pairing again")[1]
            if body.get("code") != PAIR_CODE:
                dev.fails += 1
                if dev.fails >= 5:
                    dev.fails, dev.pair_until, dev.blocked_until = 0, 0, now + 60
                return 422, err(422, "wrong_code", "code", "Wrong code")[1]
            tok = hashlib.sha256(os.urandom(16)).hexdigest()
            dev.sessions.add(tok)
            dev.fails, dev.pair_until = 0, 0
            return 200, {"paired": True}, {"Set-Cookie": "hy_s=%s; Path=/; HttpOnly; SameSite=Strict" % tok}

        if path == "/api/state" and m == "GET":
            s = dict(dev.state)
            s.update({"utc_ms": dev.now_ms() if dev.clock_valid else 0, "clock_valid": dev.clock_valid,
                      "zone": dev.zone, "sync": dev.sync if dev.clock_valid else {"src": "", "at": 0},
                      "tiles": dev.tiles(), "due": dev.due})
            return 200, s

        if path == "/api/medicines":
            if m == "GET":
                return 200, {"api": 1, "meds": sorted(dev.meds, key=lambda x: x["order"])}
            if m == "POST":
                bad = validate_med(dev, body, 0)
                if bad:
                    return bad
                med = {"id": dev.next_id, "archived": False,
                       "order": 1 + max([x["order"] for x in dev.meds] + [-1])}
                dev.next_id += 1
                med["pill_count"] = {"enabled": False, "remaining": 0, "alert": 0}
                apply_med(med, body)
                dev.meds.append(med)
                dev.broadcast("meds_changed")
                return 201, med

        if path == "/api/medicines/order" and m == "PUT":
            ids = body.get("ids")
            act = [x["id"] for x in dev.meds if not x["archived"]]
            if not isinstance(ids, list) or sorted(ids) != sorted(act):
                return 400, err(400, "invalid", "ids", "Send every active medicine id once")[1]
            for i, mid in enumerate(ids):
                dev.med(mid)["order"] = i
            dev.broadcast("meds_changed")
            return 200, {"ids": ids}

        mm = re.fullmatch(r"/api/medicines/(\d+)(?:/(archive|unarchive|pills|log|skip|snooze))?", path)
        if mm:
            med = dev.med(int(mm.group(1)))
            act = mm.group(2)
            if not med:
                return 404, err(404, "not_found", None, "No such medicine")[1]
            if not act and m == "GET":
                return 200, med
            if not act and m == "PUT":
                if "If-Match" not in self.headers:
                    return 428, err(428, "precondition_required", None, "Send If-Match with the etag from the last read")[1]
                if dev.args.stale or self.headers.get("If-Match", "").strip('"') != med["etag"]:
                    return 412, err(412, "stale", None, "This medicine was changed on the device")[1]
                bad = validate_med(dev, body, med["id"])
                if bad:
                    return bad
                apply_med(med, body)
                dev.broadcast("meds_changed")
                return 200, med
            if not act and m == "DELETE":
                if not med["archived"]:
                    return 400, err(400, "invalid", None, "Only archived medicines can be deleted")[1]
                n = len([e for e in dev.log if e["med_id"] == med["id"]])
                if q.get("confirm") != ["1"]:
                    return 409, err(409, "confirm", None, "Deleting also deletes %d log entries" % n,
                                    log_entries=n)[1]
                dev.log = [e for e in dev.log if e["med_id"] != med["id"]]
                dev.meds.remove(med)
                dev.broadcast("meds_changed")
                return 200, {"deleted": True, "log_entries": n}
            if act == "archive" and m == "POST":
                med["archived"] = True
                dev.due = [d for d in dev.due if d["med_id"] != med["id"]]
                dev.broadcast("meds_changed")
                return 200, med
            if act == "unarchive" and m == "POST":
                if any(x["code"] == med["code"] and not x["archived"] for x in dev.meds):
                    return err(409, "code_dup", "code", "Another active medicine already uses this code.")
                med["archived"] = False
                dev.broadcast("meds_changed")
                return 200, med
            if act == "pills" and m == "POST":
                if "add" in body:
                    med["pill_count"]["remaining"] = max(0, min(9999, med["pill_count"]["remaining"] + int(body["add"])))
                else:
                    med["pill_count"]["remaining"] = int(body.get("left", med["pill_count"]["remaining"]))
                return 200, med
            if act in ("log", "skip", "snooze") and m == "POST":
                return self.dose(med, act, body)

        return 501, err(501, "not_implemented", None, "Not available in this firmware")[1]

    # ---- routes added with the device handlers: the shapes copy the firmware's answers ----
    def route_extra(self, m, path, q, body, now):
        dev = self.dev

        if path == "/api/time" and m == "POST":
            ms = body.get("utc_ms")
            if isinstance(ms, bool) or not isinstance(ms, (int, float)) or ms < BUILD_EPOCH * 1000:
                return 400, err(400, "invalid", "utc_ms", "The time was refused")[1]
            auto = bool(body.get("auto"))
            zone = None if auto else body.get("zone")
            if zone is not None and not (isinstance(zone, str) and ("/" in zone or zone == "UTC")):
                return 400, err(400, "invalid", "zone", "That zone is not in the device's table")[1]
            diff = int(ms - dev.now_ms()) if dev.clock_valid else 0
            do_set = (not auto) or (not dev.clock_valid) or abs(ms - dev.now_ms()) > 2000
            if do_set:
                dev.skew = ms / 1000.0 - time.time()
                dev.clock_valid = True
                dev.sync = {"src": "browser", "at": now}
                if zone:
                    dev.zone = zone
                dev.broadcast("meds_changed")
            return 200, {"set": do_set, "diff_ms": diff, "zone_set": bool(do_set and zone)}

        if path == "/api/browsers":
            if m == "GET":
                return 200, {"browsers": dev.browsers, "max": 4}
            if m == "DELETE":
                if body.get("all") is True:
                    dev.browsers = []
                elif isinstance(body.get("index"), int) and not isinstance(body.get("index"), bool)                         and body["index"] in [b["index"] for b in dev.browsers]:
                    dev.browsers = [b for b in dev.browsers if b["index"] != body["index"]]
                elif isinstance(body.get("index"), int):
                    return 404, err(404, "not_found", "index", "No paired browser at that index")[1]
                else:
                    return 400, err(400, "invalid", "index", 'Send {"index":n} or {"all":true}')[1]
                return 200, {"paired": len(dev.browsers)}

        if path == "/api/settings":
            if m == "GET":
                return 200, dev.settings
            if m == "PUT":
                new = dict(dev.settings)
                rng = {"volume": (0, 100), "brightness": (10, 100), "snooze_min": (1, 120), "dim_s": (10, 600),
                       "off_bat_s": (30, 3600), "off_usb_s": (0, 86400)}
                for k, v in body.items():
                    if k in ("sound", "dnd", "rot_lock", "wifi_keep_batt"):
                        if not isinstance(v, bool):
                            return 400, err(400, "invalid", k, "Wrong type")[1]
                        new[k] = v
                    elif k in rng:
                        if isinstance(v, bool) or not isinstance(v, int) or v < 0:
                            return 400, err(400, "invalid", k, "Wrong type")[1]
                        new[k] = max(rng[k][0], min(rng[k][1], v))
                    elif k == "quiet":
                        ok = v is None or (isinstance(v, dict) and re.fullmatch(r"\d\d:\d\d", str(v.get("from"))) and
                                           re.fullmatch(r"\d\d:\d\d", str(v.get("to"))))
                        if not ok:
                            return 400, err(400, "invalid", "quiet", "Wrong type")[1]
                        new["quiet"] = v
                dev.settings = new   # "rot" is the applied rotation: never written here
                return 200, new

        if path == "/api/log":
            if m == "GET":
                return self.log_page(q, now)
            if m == "POST":
                mid, ts = body.get("med_id"), body.get("ts")
                if not isinstance(mid, int):
                    return 400, err(400, "invalid", "med_id", "med_id is required")[1]
                if not dev.med(mid):
                    return 404, err(404, "not_found", None, "Not found")[1]
                if not isinstance(ts, int) or ts < BUILD_EPOCH or ts > now + 60:
                    return 400, err(400, "invalid", "ts", "The time is not valid")[1]
                if body.get("status", "taken") != "taken":
                    return 400, err(400, "invalid", "status", "Only taken doses can be added")[1]
                e = {"id": dev.next_log, "ts": ts, "tz": "UTC0", "med_id": mid, "status": "taken", "slot": 0,
                     "delta_min": 0, "source": "web", "edited": False}
                if body.get("note"):
                    e["note"] = body["note"]
                dev.next_log += 1
                dev.log.append(e)
                dev.broadcast("taken")
                return 201, with_local(e)

        mm = re.fullmatch(r"/api/log/(\d+)(/undo)?", path)
        if mm:
            lid = int(mm.group(1))
            e = next((x for x in dev.log if x["id"] == lid), None)
            if mm.group(2):
                if m != "POST":
                    return None
                if not e:
                    return 404, err(404, "not_found", None, "Not found")[1]
                if now - e["ts"] > 5:
                    return 410, err(410, "undo_expired", None, "Too late to undo")[1]
                dev.log.remove(e)
                if e.get("slot"):
                    dev.due.append({"med_id": e["med_id"], "slot": e["slot"], "state": "due", "snoozed_until": None})
                dev.broadcast("undone")
                return 200, {"undone": True}
            if m == "PUT":
                orig = body.get("orig_ts")
                if not isinstance(orig, int):
                    return 400, err(400, "invalid", "orig_ts", "orig_ts is required")[1]
                if not e or e["ts"] != orig:
                    return 404, err(404, "not_found", None, "Not found")[1]
                if "ts" in body:
                    if not isinstance(body["ts"], int) or body["ts"] < BUILD_EPOCH or body["ts"] > now + 60:
                        return 400, err(400, "invalid", "ts", "The time is not valid")[1]
                    e["ts"] = body["ts"]
                if "status" in body:
                    if body["status"] not in ("taken", "skipped", "missed"):
                        return 400, err(400, "invalid", "status", "Wrong status")[1]
                    e["status"] = body["status"]
                if body.get("note") is not None:
                    e["note"] = body["note"]
                e["edited"] = True
                dev.broadcast("log_edited")
                return 200, with_local(e)
            if m == "DELETE":
                try:
                    ts = int(q.get("ts", [""])[0])
                except ValueError:
                    return 400, err(400, "invalid", "ts", "Send ?ts= with the entry's time")[1]
                if not e or e["ts"] != ts:
                    return 404, err(404, "not_found", None, "Not found")[1]
                dev.log.remove(e)
                dev.broadcast("log_edited")
                return 200, {"deleted": True}

        if path == "/api/history" and m == "GET":
            return self.history(q, now)
        if path == "/api/schedule" and m == "GET":
            return self.schedule(q, now)

        if path == "/api/wifi" and m == "GET":
            w = dev.wifi
            on = w["mode"] == "client"
            return 200, {"mode": w["mode"], "state": "connected" if on else w["mode"],
                         "ssid": w["ssid"] if on else None, "rssi": -52 if on else None,
                         "ip": "192.168.1.23" if on else None, "host": w["host"],
                         "ap_ssid": "hygeia-AB12", "ap_clients": 0, "networks": w_list(w["networks"])}
        if path == "/api/wifi/mode" and m == "POST":
            mode, ssid = body.get("mode"), body.get("ssid")
            if mode not in ("off", "client", "ap"):
                return 400, err(400, "invalid", "mode", "mode is off, client or ap")[1]
            if ssid is not None and (not isinstance(ssid, str) or mode != "client"):
                return 400, err(400, "invalid", "ssid", "ssid is for the client mode")[1]
            if ssid is not None and ssid not in dev.wifi["networks"]:
                return 404, err(404, "not_found", "ssid", "That network is not saved")[1]
            dev.wifi["mode"] = mode
            if ssid:
                dev.wifi["ssid"] = ssid
            return 200, {"mode": mode, "ssid": ssid}
        if path == "/api/wifi/networks" and m in ("POST", "DELETE"):
            nets = dev.wifi["networks"]
            ssid = body.get("ssid")
            if not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32:
                return 400, err(400, "invalid", "ssid", "The name is 1 to 32 characters")[1]
            if m == "POST":
                pw = body.get("password", "")
                if not isinstance(pw, str) or (pw and not 8 <= len(pw) <= 63):
                    return 400, err(400, "invalid", "password", "The password is 8 to 63 characters, or empty")[1]
                if ssid not in nets:
                    if len(nets) >= 5:
                        return 409, err(409, "full", None, "Five networks are saved: forget one first")[1]
                    nets.append(ssid)
                return 201, {"networks": w_list(nets)}
            if ssid not in nets:
                return 404, err(404, "not_found", "ssid", "That network is not saved")[1]
            nets.remove(ssid)
            return 200, {"networks": w_list(nets)}
        if path == "/api/wifi/ap" and m == "PUT":
            pw, host = body.get("password"), body.get("host")
            if not isinstance(pw, str) and not isinstance(host, str):
                return 400, err(400, "invalid", None, 'Send "password", "host" or both')[1]
            if isinstance(pw, str) and not 8 <= len(pw) <= 63:
                return 400, err(400, "invalid", "password", "The password is 8 to 63 characters")[1]
            if isinstance(host, str):
                if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,31}", host):
                    return 400, err(400, "invalid", "host", "The host name is 1 to 32 of a-z, 0-9 and -")[1]
                dev.wifi["host"] = host
            return 200, {"host": dev.wifi["host"], "ap_ssid": "hygeia-AB12"}

        if path == "/api/export.csv" and m == "GET":
            return self.export("csv", q)
        if path == "/api/export.json" and m == "GET":
            return self.export("json", q)

        if path == "/api/chime/test" and m == "POST":
            return 200, {"played": True}

        if path == "/api/power/off" and m == "POST":
            due = any(not x["archived"] and x["schedule"]["type"] == "fixed" and x["schedule"]["times"]
                      for x in dev.meds)
            if body.get("confirm") is not True:
                return 409, {"error": "confirm", "message": 'Send {"confirm":true} to power off', "due_24h": due}
            return 200, {"powering_off": True, "due_24h": due}
        return None

    # ---- export and import: the device's shapes ----
    def export_name(self, ext):
        return 'attachment; filename="hygeia-%s.%s"' % (time.strftime("%Y%m%d", time.localtime()), ext)

    def export(self, kind, q):
        dev = self.dev
        log = sorted(dev.log, key=lambda e: (e["ts"], e["id"]))
        if kind == "csv":
            meds = {x["id"]: x for x in dev.meds}

            def cell(s):
                s = "" if s is None else str(s)
                if s[:1] in ("=", "+", "-", "@", "\t", "\r"):
                    s = "'" + s
                if any(c in s for c in ',"\r\n'):
                    s = '"' + s.replace('"', '""') + '"'
                return s
            rows = ["id,local_time,utc,tz,code,name,status,slot_local,delta_min,note,source,edited"]
            for e in log:
                mm = meds.get(e["med_id"], {})
                rows.append(",".join([
                    str(e["id"]), time.strftime("%Y-%m-%d %H:%M", time.gmtime(e["ts"])), str(e["ts"]),
                    cell(e.get("tz", "UTC0")), cell(mm.get("code", "?")), cell(mm.get("name", "")),
                    cell(e["status"]),
                    time.strftime("%Y-%m-%d %H:%M", time.gmtime(e["slot"])) if e.get("slot") else "",
                    str(e.get("delta_min", 0)), cell(e.get("note", "")), cell(e.get("source", "screen")),
                    "true" if e.get("edited") else "false"]))
            data = ("\r\n".join(rows) + "\r\n").encode("utf-8")
            return 200, Raw(data, "text/csv; charset=utf-8"), {"Content-Disposition": self.export_name("csv")}
        secrets = (q.get("secrets") or ["0"])[0]
        if secrets not in ("0", "1"):
            return 400, err(400, "invalid", "secrets", "secrets is 0 or 1")[1], {}
        meds = [{k: v for k, v in x.items() if k != "etag"} for x in dev.meds]
        doc = {"v": 1, "api": 1, "exported": int(time.time()), "meds": meds, "next_id": dev.next_id, "gaps": [],
               "settings": dev.settings}
        if secrets == "1":
            doc["wifi"] = {"networks": [{"ssid": s, "pass": "example-pass"} for s in dev.wifi["networks"]],
                           "ap_pass": "abcdefgh23"}
        doc["log"] = [{k: v for k, v in e.items() if k != "local"} for e in log]   # always the last member
        data = json.dumps(doc, separators=(",", ":")).encode("utf-8")
        return 200, Raw(data, "application/json"), {"Content-Disposition": self.export_name("json")}

    # ---- firmware update: the device's checks on the first bytes, no flash ----
    def read_ota_body(self, path):
        """Reads an upload body before the device lock is taken, so other requests are served meanwhile (the device
        reads it in a worker). None when nothing was read: not an upload, a refusal that needs no body, or another
        upload is being read (busy)."""
        dev = self.dev
        n = int(self.headers.get("Content-Length") or 0)
        if (path != "/api/ota" or self.command != "POST" or not n or n > OTA_SLOT or dev.args.low_battery
                or dev.args.ota_pending or self.headers.get("X-Hygeia") != "1"
                or (dev.args.unpaired and self.session() not in dev.sessions)):
            return None
        with dev.lock:
            if dev.ota_busy or dev.restart_at:
                return None
            dev.ota_busy = True
        try:
            return self.rfile.read(n)
        finally:
            with dev.lock:
                dev.ota_busy = False

    def ota(self):
        dev = self.dev
        n = int(self.headers.get("Content-Length") or 0)
        data = self.ota_body
        if dev.restart_at:
            self.close_connection = data is None
            return 409, err(409, "busy", None, "A firmware update finished, the device is restarting")[1], {}
        if not n:
            return 400, err(400, "invalid", "body", "Send the firmware file as the request body")[1], {}
        if n > OTA_SLOT:
            self.close_connection = True   # the device closes on a large unread body
            return 413, err(413, "too_large", None, "The file is larger than the firmware slot")[1], {}
        if dev.args.ota_pending:
            self.close_connection = True
            return 503, err(503, "busy", None, "The device is still checking its own firmware, try again shortly")[1], \
                {"Retry-After": "2"}
        if dev.args.low_battery:
            self.close_connection = True
            return 409, err(409, "battery", None, "Battery below 30 %: connect USB before updating")[1], {}
        if data is None:   # another upload was being read
            self.close_connection = True
            return 409, err(409, "busy", None, "An export, import or firmware update is running")[1], {}
        if len(data) < n:
            self.close_connection = True
            return 408, err(408, "timeout", None, "The file did not arrive in time")[1], {}
        # esp_image_header_t (24 bytes: magic, segment count, ..., chip id at 12), a segment header (8), then
        # esp_app_desc_t
        ok = (len(data) >= 32 + 256 and data[0] == 0xE9 and data[1] > 0
              and struct.unpack_from("<H", data, 12)[0] == 0x0009
              and struct.unpack_from("<I", data, 32)[0] == 0xABCD5432
              and data[80:112].split(b"\0", 1)[0] == b"hygeia")
        if not ok:
            return 422, err(422, "not_hygeia", None, "This file is not a hygeia firmware image")[1], {}
        if len(data) < 64 * 1024:   # stands in for esp_ota_end's image check: a real image is far larger
            return 422, err(422, "corrupt", None, "The firmware file is damaged or incomplete")[1], {}
        dev.next_fw = data[48:80].split(b"\0", 1)[0].decode("utf-8", "replace") or "unknown"
        dev.restart_at = time.time() + OTA_RESTART_S
        return 200, {"reboot_in_s": OTA_RESTART_S}, {}

    def backup_import(self, q):
        dev = self.dev
        mode = (q.get("mode") or ["merge"])[0]
        if mode not in ("merge", "replace"):
            return 400, err(400, "invalid", "mode", "mode is merge or replace")[1], {}
        preview = (q.get("preview") or ["0"])[0] == "1"
        settings = (q.get("settings") or ["0"])[0] == "1"
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return 400, err(400, "invalid", "body", "Send the backup file as the request body")[1], {}
        if n > 4 * 1024 * 1024:
            return 413, err(413, "too_large", None, "The file is above 4 MB")[1], {}
        try:
            doc = json.loads(self.rfile.read(n).decode("utf-8"))
        except ValueError:
            return 400, err(400, "invalid", "json", "That request is not valid")[1], {}
        if not isinstance(doc, dict):
            return 400, err(400, "invalid", "json", "That request is not valid")[1], {}
        if doc.get("v") != 1:
            return 400, err(400, "invalid", "v", "That request is not valid")[1], {}
        if not isinstance(doc.get("meds"), list):
            return 400, err(400, "invalid", "meds", "That request is not valid")[1], {}
        fmeds, flog, fgaps = doc["meds"], doc.get("log") or [], doc.get("gaps") or []
        cur = {x["id"]: x for x in dev.meds}
        kinds = {}
        if mode == "replace":
            kinds = {x["id"]: "add" for x in fmeds}
        else:
            for x in fmeds:
                c = cur.get(x["id"])
                if c and c["code"] == x["code"]:
                    same = dev.etag(x) == dev.etag(c)
                    kinds[x["id"]] = "same" if same else "change"
                else:
                    kinds[x["id"]] = "add"
        have = {(e["med_id"], e["ts"], e["status"]) for e in dev.log}
        add = dup = skip = 0
        for e in flog:
            k = kinds.get(e.get("med_id"))
            if k is None:
                skip += 1
            elif mode == "merge" and k != "add" and (e["med_id"], e["ts"], e["status"]) in have:
                dup += 1
            else:
                add += 1
        counts = {"mode": mode, "exported": doc.get("exported", 0),
                  "meds": {"add": sum(1 for v in kinds.values() if v == "add"),
                           "change": sum(1 for v in kinds.values() if v == "change"),
                           "same": sum(1 for v in kinds.values() if v == "same")},
                  "log": {"add": add, "dup": dup, "skip": skip}, "gaps": {"add": len(fgaps)},
                  "settings": "settings" in doc, "secrets": "wifi" in doc,
                  "settings_applied": False, "secrets_applied": False}
        if preview:
            return 200, dict({"preview": True}, **counts), {}
        if mode == "replace":
            dev.meds = [dict(x) for x in fmeds]
            dev.log = [dict(e) for e in flog if e.get("med_id") in kinds]
        else:
            remap = {}
            for x in fmeds:
                if kinds[x["id"]] == "change":
                    cur[x["id"]].update(x)
                elif kinds[x["id"]] == "add":
                    nm = dict(x, id=dev.next_id)
                    remap[x["id"]] = dev.next_id
                    dev.next_id += 1
                    dev.meds.append(nm)
            for e in flog:
                k = kinds.get(e.get("med_id"))
                if k is None or (k != "add" and (e["med_id"], e["ts"], e["status"]) in have):
                    continue
                dev.log.append(dict(e, id=dev.next_log, med_id=remap.get(e["med_id"], e["med_id"])))
                dev.next_log += 1
        for x in dev.meds:
            x["etag"] = dev.etag(x)
        dev.next_id = max([dev.next_id] + [x["id"] + 1 for x in dev.meds])
        dev.next_log = max([dev.next_log] + [e["id"] + 1 for e in dev.log])
        if settings and isinstance(doc.get("settings"), dict):
            dev.settings.update({k: v for k, v in doc["settings"].items() if k != "rot"})
            counts["settings_applied"] = True
        if settings and "wifi" in doc:
            for net in doc["wifi"].get("networks", []):
                if net.get("ssid") not in dev.wifi["networks"]:
                    dev.wifi["networks"].append(net["ssid"])
            counts["secrets_applied"] = True
        dev.broadcast("meds_changed")
        dev.broadcast("log_edited")
        return 200, dict({"imported": True}, **counts), {}

    def log_page(self, q, now):
        dev = self.dev
        try:
            to = int(q.get("to", [now + 1])[0])
            frm = int(q.get("from", [to - 30 * 86400])[0])
            med = int(q.get("med", [0])[0])
            limit = int(q.get("limit", [200])[0])
            after = q.get("after", [None])[0]
            a_ts, a_id = (int(x) for x in after.split(".")) if after else (0, 0)
        except ValueError:
            return 400, err(400, "invalid", None, "A query value is not a number")[1]
        if frm >= to:
            return 400, err(400, "invalid", "from", "from is not before to")[1]
        if not 1 <= limit <= 500:
            return 400, err(400, "invalid", "limit", "limit is 1 to 500")[1]
        rows = sorted((e for e in dev.log if max(frm, a_ts) <= e["ts"] < to and (not med or e["med_id"] == med)
                       and (e["ts"], e["id"]) > (a_ts, a_id)), key=lambda e: (e["ts"], e["id"]))
        page = rows[:limit]
        nxt = "%d.%d" % (page[-1]["ts"], page[-1]["id"]) if len(rows) > limit else None
        return 200, {"log": [with_local(e) for e in page], "next": nxt}

    def history(self, q, now):
        dev = self.dev
        days = {"7": 7, "30": 30}.get(q.get("days", ["7"])[0])
        if not days:
            return 400, err(400, "invalid", "days", "days is 7 or 30")[1]
        if not dev.clock_valid:
            return 409, err(409, "clock_invalid", None, "The device clock is not set")[1]
        today = int(now // 86400)
        out, tt, tr = [], 0, 0
        for med in sorted((x for x in dev.meds if not x["archived"]), key=lambda x: x["order"]):
            marks, taken_n, resolved = [], 0, 0
            for d in range(today - days + 1, today + 1):
                ents = [e for e in dev.log if e["med_id"] == med["id"] and e["ts"] // 86400 == d]
                sch = med["schedule"]
                if sch["type"] == "as_needed":
                    marks.append("as_needed_taken" if any(e["status"] == "taken" for e in ents) else "none")
                    continue
                n = len(sch["times"]) if (sch["weekdays"] >> ((d + 3) % 7)) & 1 else 0
                got = len([e for e in ents if e["status"] == "taken"])
                skp = len([e for e in ents if e["status"] == "skipped"])
                if n == 0:
                    marks.append("none")
                elif got >= n:
                    late = any(e.get("delta_min", 0) >= med["late_after_min"] for e in ents)
                    marks.append("late" if late else "on_time")
                    taken_n += n
                    resolved += n
                elif skp:
                    marks.append("skipped")
                    resolved += n
                elif d < today:
                    marks.append("missed")
                    resolved += n
                else:
                    marks.append("none")
            tt += taken_n
            tr += resolved
            out.append({"id": med["id"], "code": med["code"], "name": med["name"], "color": med["color"],
                        "in_progress": False, "adherence": {"taken": taken_n, "resolved": resolved}, "marks": marks})

        def day(n):
            return time.strftime("%Y-%m-%d", time.gmtime(n * 86400))
        return 200, {"days": days, "first": day(today - days + 1), "today": day(today),
                     "adherence": {"taken": tt, "resolved": tr}, "meds": out}

    def schedule(self, q, now):
        dev = self.dev
        try:
            frm = int(q.get("from", [now])[0])
            to = int(q.get("to", [frm + 7 * 86400])[0])
        except ValueError:
            return 400, err(400, "invalid", "from", "from is a time in seconds")[1]
        if to <= frm or to - frm > 8 * 86400:
            return 400, err(400, "invalid", "to", "The range is between 1 second and 8 days")[1]
        if not dev.clock_valid:
            return 409, err(409, "clock_invalid", None, "The device clock is not set")[1]
        slots = []
        for med in dev.meds:
            sch = med["schedule"]
            if med["archived"] or sch["type"] != "fixed":
                continue
            for d in range(frm // 86400 - 1, to // 86400 + 1):
                if not (sch["weekdays"] >> ((d + 3) % 7)) & 1:
                    continue
                for t in sch["times"]:
                    hh, mi = (int(x) for x in t.split(":"))
                    slot = d * 86400 + hh * 3600 + mi * 60
                    if not frm <= slot < to:
                        continue
                    got = [e for e in dev.log if e["med_id"] == med["id"] and abs(e["ts"] - slot) < 3 * 3600]
                    if any(e["status"] == "taken" for e in got):
                        st = "taken"
                    elif any(e["status"] == "skipped" for e in got):
                        st = "skipped"
                    elif slot > now:
                        st = "upcoming"
                    elif now < slot + med["late_after_min"] * 60:
                        st = "due"
                    elif now < slot + med["missed_after_min"] * 60:
                        st = "late"
                    else:
                        st = "missed"
                    slots.append({"med_id": med["id"], "slot": slot, "state": st})
        slots.sort(key=lambda x: (x["slot"], x["med_id"]))
        return 200, {"from": frm, "to": to, "slots": slots}

    def dose(self, med, act, body):
        dev = self.dev
        now = int(time.time())
        slot = body.get("slot")
        if act == "snooze":
            for d in dev.due:
                if d["med_id"] == med["id"]:
                    d["state"], d["snoozed_until"] = "snoozed", now + 15 * 60
            dev.broadcast("snoozed")
            return 200, {"until": now + 15 * 60}
        if act == "log" and not body.get("confirm") and med["schedule"]["type"] == "as_needed":
            gap = med["schedule"]["min_interval_min"] * 60
            last = [e["ts"] for e in dev.log if e["med_id"] == med["id"] and e["status"] == "taken"]
            if gap and last and now - max(last) < gap:
                return err(409, "too_soon", None, "The last dose was %d min ago. The minimum gap is %d min."
                           % ((now - max(last)) // 60, gap // 60))
        entry = {"id": dev.next_log, "ts": now, "med_id": med["id"],
                 "status": "taken" if act == "log" else "skipped", "source": "web", "edited": False}
        if slot:
            entry["slot"] = slot
        dev.next_log += 1
        dev.log.append(entry)
        dev.due = [d for d in dev.due if d["med_id"] != med["id"]]
        if act == "log" and med["pill_count"]["enabled"] and med["pill_count"]["remaining"] > 0:
            med["pill_count"]["remaining"] -= 1
        dev.broadcast("taken" if act == "log" else "skipped")
        return 200, {"log_id": entry["id"], "undo_until": now + 5}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8484)
    ap.add_argument("--unpaired", action="store_true")
    ap.add_argument("--slow", action="store_true")
    ap.add_argument("--stale", action="store_true")
    ap.add_argument("--clock-invalid", action="store_true")
    ap.add_argument("--skew", type=float, default=0.0)
    ap.add_argument("--zone", default="")
    ap.add_argument("--busy", action="store_true")
    ap.add_argument("--low-battery", action="store_true")
    ap.add_argument("--ota-pending", action="store_true")
    args = ap.parse_args(argv)
    Handler.dev = Device(args)
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    srv.daemon_threads = True
    print("webmock: http://127.0.0.1:%d/  (pairing code %s)" % (args.port, PAIR_CODE), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
