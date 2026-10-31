#!/usr/bin/env python3
"""Development server that stands in for the device while the web UI is built.

    python tools/webmock.py [--port 8484] [--unpaired] [--slow] [--stale] [--clock-invalid] [--skew SECONDS] [--zone NAME] [--busy]

Serves the bundle made by tools/web_bundle.py from web/ (rebuilt on every request, so edits show on reload) and
answers /api/* from the sample files in tools/webmock/samples/ plus in-memory changes. Routes the firmware does not
implement yet answer 501 like the device. Nothing is stored on disk.

  --unpaired       every /api route except info and pairing answers 401 until the code 123456 is posted
  --slow           adds 1.5 s to every /api answer
  --stale          PUT /api/medicines/{id} answers 412
  --clock-invalid  starts with an invalid clock until POST /api/time
  --skew SECONDS   the device clock runs this many seconds ahead of the host
  --zone NAME      the device time zone (default Europe/Zurich)
  --busy           writes answer 503 busy; the first GET of each read route answers 503 once
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
        if u.path == "/ws":
            return self.ws()
        if u.path.startswith("/api/"):
            if self.dev.args.slow:
                time.sleep(1.5)
            with self.dev.lock:
                try:
                    status, body, headers = self.api(u.path, parse_qs(u.query))
                except Exception as e:  # keep the mock alive, say what broke
                    status, body, headers = 500, {"error": "internal", "field": None, "message": repr(e)}, {}
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
        body = self.read_body() if m in ("POST", "PUT") else {}
        if body is None:
            return 400, err(400, "invalid", None, "The body is not valid JSON")[1], {}
        r = self.route(m, path, q, body)
        if len(r) == 2:
            return r[0], r[1], {}
        return r

    def route(self, m, path, q, body):
        dev = self.dev
        now = int(time.time())

        if path == "/api/info" and m == "GET":
            return 200, {"api": 1, "fw": "0.0.0-mock", "name": "hygeia",
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

        if path == "/api/time" and m == "POST":
            ms = body.get("utc_ms")
            if not isinstance(ms, (int, float)):
                return 400, err(400, "invalid", "utc_ms", "Send utc_ms")[1]
            dev.skew = ms / 1000.0 - time.time()
            dev.clock_valid = True
            if body.get("zone"):
                dev.zone = body["zone"]
            dev.sync = {"src": "browser", "at": now}
            dev.broadcast("meds_changed")
            return 200, {"utc_ms": dev.now_ms(), "zone": dev.zone}

        if path == "/api/settings":
            if m == "GET":
                return 200, sample("settings.json")
            return 501, err(501, "not_implemented", None, "Not available in this firmware")[1]

        if path == "/api/log" and m == "GET":
            return 200, {"log": sorted(dev.log, key=lambda e: -e["ts"]), "next": None}

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
            return 200, {}

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
                return 200, {}
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

        mm = re.fullmatch(r"/api/log/(\d+)/undo", path)
        if mm and m == "POST":
            for e in dev.log:
                if e["id"] == int(mm.group(1)):
                    dev.log.remove(e)
                    if e.get("slot"):
                        dev.due.append({"med_id": e["med_id"], "slot": e["slot"], "state": "due",
                                        "snoozed_until": None})
                    dev.broadcast("undone")
                    return 200, {}
            return 410, err(410, "undo_expired", None, "Too late to undo")[1]

        return 501, err(501, "not_implemented", None, "Not available in this firmware")[1]

    def dose(self, med, act, body):
        dev = self.dev
        now = int(time.time())
        slot = body.get("slot")
        if act == "snooze":
            for d in dev.due:
                if d["med_id"] == med["id"]:
                    d["state"], d["snoozed_until"] = "snoozed", now + 15 * 60
            dev.broadcast("snoozed")
            return 200, {}
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
