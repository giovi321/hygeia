#!/usr/bin/env python3
"""Writes the "Screen and web parity" table of docs/src/content/docs/reference/rest-api.md.

    python tools/gen_parity.py            rewrite the table between the two marker comments
    python tools/gen_parity.py --check    exit 1 when the table in the file is not current

The rows come from the route table in components/hy_core/src/routes.c: method, path and the place on the screen
(an empty place means the route is web only). WEB_PAGE below names the web UI page that uses each route. A route
that has no entry there stops the script, so a new route has to be placed on a page before the table can be made.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROUTES = os.path.join(ROOT, "components", "hy_core", "src", "routes.c")
ROUTES_H = os.path.join(ROOT, "components", "hy_core", "include", "hy", "routes.h")
DOC = os.path.join(ROOT, "docs", "src", "content", "docs", "reference", "rest-api.md")
START = "<!-- parity:start -->"
END = "<!-- parity:end -->"

# Pages of the web UI, by route id in routes.c.
WEB_PAGE = {
    "HY_R_INFO": "Every page (version, pairing state), Firmware",
    "HY_R_STATE": "Dashboard",
    "HY_R_PAIR_START": "Pair this browser",
    "HY_R_PAIR": "Pair this browser",
    "HY_R_BROWSERS": "Settings",
    "HY_R_BROWSERS_DEL": "Settings",
    "HY_R_MEDS": "Medicines",
    "HY_R_MED_NEW": "Medicines",
    "HY_R_MED": "Medicines",
    "HY_R_MED_PUT": "Medicines",
    "HY_R_MED_DEL": "Medicines",
    "HY_R_MED_ARCHIVE": "Medicines",
    "HY_R_MED_UNARCHIVE": "Medicines",
    "HY_R_MED_ORDER": "Medicines",
    "HY_R_MED_PILLS": "Medicines",
    "HY_R_MED_LOG": "Dashboard",
    "HY_R_MED_SKIP": "Dashboard",
    "HY_R_MED_SNOOZE": "Dashboard",
    "HY_R_LOG": "Log",
    "HY_R_LOG_NEW": "Log",
    "HY_R_LOG_PUT": "Log",
    "HY_R_LOG_DEL": "Log",
    "HY_R_LOG_UNDO": "Dashboard",
    "HY_R_HISTORY": "History",
    "HY_R_SCHEDULE": "Schedule",
    "HY_R_EXPORT_CSV": "Backup",
    "HY_R_EXPORT_JSON": "Backup",
    "HY_R_IMPORT": "Backup",
    "HY_R_SETTINGS": "Settings, Wi-Fi",
    "HY_R_SETTINGS_PUT": "Settings, Wi-Fi",
    "HY_R_TIME": "Settings",
    "HY_R_WIFI": "Wi-Fi",
    "HY_R_WIFI_MODE": "Wi-Fi",
    "HY_R_WIFI_NET_ADD": "Wi-Fi",
    "HY_R_WIFI_NET_DEL": "Wi-Fi",
    "HY_R_WIFI_AP": "Wi-Fi",
    "HY_R_CHIME_TEST": "Settings",
    "HY_R_POWER_OFF": "Settings",
    "HY_R_OTA": "Firmware",
}

ROW = re.compile(r'\{\s*HY_M_(GET|POST|PUT|DELETE)\s*,\s*"([^"]+)"\s*,\s*(HY_R_[A-Z0-9_]+)\s*,[^,]+,\s*"([^"]*)"\s*\}')


def routes():
    with open(ROUTES, "r", encoding="utf-8") as f:
        src = f.read()
    rows = ROW.findall(src)
    if not rows:
        sys.exit("gen_parity: no routes found in " + ROUTES)
    # every table row has to be parsed: compare with the row starts and with the route ids of the enum
    starts = len(re.findall(r"^\s*\{\s*HY_M_", src, re.M))
    with open(ROUTES_H, "r", encoding="utf-8") as f:
        head = f.read()
    enum = re.search(r"typedef enum \{(.*?)HY_R_COUNT", head, re.S)
    ids = len(re.findall(r"HY_R_[A-Z0-9_]+", enum.group(1))) if enum else -1
    if len(rows) != starts or len(rows) != ids:
        sys.exit("gen_parity: parsed %d rows, the table has %d and the enum %d route ids; a row did not parse"
                 % (len(rows), starts, ids))
    return rows


def table():
    out = ["| Route | Web page | Screen |", "|---|---|---|"]
    for method, path, rid, screen in routes():
        if rid not in WEB_PAGE:
            sys.exit("gen_parity: %s (%s %s) has no web page in WEB_PAGE" % (rid, method, path))
        out.append("| `%s %s` | %s | %s |" % (method, path, WEB_PAGE[rid], screen or "web only"))
    return "\n".join(out)


def main():
    check = "--check" in sys.argv[1:]
    with open(DOC, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    nl = "\r\n" if "\r\n" in text else "\n"
    text = text.replace("\r\n", "\n")
    a, b = text.find(START), text.find(END)
    if a < 0 or b < a:
        sys.exit("gen_parity: the marker comments are missing in " + DOC)
    new = text[:a + len(START)] + "\n" + table() + "\n" + text[b:]
    if check:
        if new != text:
            print("gen_parity: the parity table is out of date; run python tools/gen_parity.py")
            return 1
        print("gen_parity: the parity table is current")
        return 0
    if new != text:
        with open(DOC, "w", encoding="utf-8", newline="") as f:
            f.write(new.replace("\n", nl))
    print("gen_parity: %d routes" % len(routes()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
