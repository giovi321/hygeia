// History: a 30 day grid per medicine, adherence, and the entries of a tapped day.
// Also holds the zone helpers the log and schedule pages use.

// The device's time zone name (from /api/state), cached on HY.state.
HY.deviceZone = function () {
  if (HY.state && HY.state.zone) { return Promise.resolve(HY.state.zone); }
  return HY.api("GET", "/api/state").then(function (s) {
    HY.state = s;
    return s.zone || "UTC";
  }, function () { return "UTC"; });
};

// "2026-10-28" and "08:30" in a zone to epoch seconds.
HY.wallToEpoch = function (dateStr, timeStr, zone) {
  var d = dateStr.split("-"), t = (timeStr || "00:00").split(":");
  var base = Date.UTC(+d[0], +d[1] - 1, +d[2], +t[0], +t[1] || 0);
  var fmt;
  try {
    fmt = new Intl.DateTimeFormat("en-GB", { timeZone: zone, hourCycle: "h23", year: "numeric", month: "numeric",
      day: "numeric", hour: "numeric", minute: "numeric", second: "numeric" });
  } catch (e) {
    fmt = new Intl.DateTimeFormat("en-GB", { hourCycle: "h23", year: "numeric", month: "numeric",
      day: "numeric", hour: "numeric", minute: "numeric", second: "numeric" });
  }
  function offset(ms) {
    var p = {};
    fmt.formatToParts(new Date(ms)).forEach(function (x) { p[x.type] = +x.value; });
    return Date.UTC(p.year, p.month - 1, p.day, p.hour, p.minute, p.second) - Math.floor(ms / 1000) * 1000;
  }
  var ms = base - offset(base);
  ms = base - offset(ms);
  return Math.floor(ms / 1000);
};

// Epoch seconds to "YYYY-MM-DD" in a zone.
HY.epochToDate = function (utc, zone) {
  var f;
  try { f = new Intl.DateTimeFormat("en-CA", { timeZone: zone, year: "numeric", month: "2-digit", day: "2-digit" }); }
  catch (e) { f = new Intl.DateTimeFormat("en-CA", { year: "numeric", month: "2-digit", day: "2-digit" }); }
  return f.format(new Date(utc * 1000));
};

HY.addDays = function (dateStr, n) {
  var d = dateStr.split("-");
  var t = new Date(Date.UTC(+d[0], +d[1] - 1, +d[2] + n));
  return t.toISOString().slice(0, 10);
};

HY.dayName = function (dateStr) {
  var d = dateStr.split("-");
  var w = new Date(Date.UTC(+d[0], +d[1] - 1, +d[2])).getUTCDay();   // 0 = Sunday
  return HY.DAYS[(w + 6) % 7];
};

HY.dayLabel = function (dateStr) {
  var d = dateStr.split("-");
  var mon = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"][+d[1] - 1];
  return HY.dayName(dateStr) + " " + (+d[2]) + " " + mon;
};

HY.statusText = function (s) {
  return s === "taken" ? "Taken" : s === "skipped" ? "Skipped" : s === "missed" ? "Missed" : s;
};

// The history and log pages share a small tab row.
HY.histTabs = function (on) {
  return HY.h("div", { class: "seg tabs" },
    HY.h("a", { class: "chip" + (on === "grid" ? " on" : ""), href: "#/history" }, "Grid"),
    HY.h("a", { class: "chip" + (on === "log" ? " on" : ""), href: "#/log" }, "Log"));
};

(function () {
  var MARK_TEXT = {
    on_time: "taken on time", late: "taken late", missed: "missed", skipped: "skipped",
    untracked: "not tracked", none: "nothing scheduled", as_needed_taken: "taken"
  };

  function pct(a) {
    if (!a || !a.resolved) { return "no resolved doses yet"; }
    return Math.round(100 * a.taken / a.resolved) + "% (" + a.taken + " of " + a.resolved + ")";
  }

  HY.route("/history", function (params, view) {
    var token = HY.token;
    var root = HY.h("div");
    var detail = HY.h("div", { class: "panel", hidden: true });
    var picked = null;   // {med, date}
    view.appendChild(HY.h("h1", null, "History"));
    view.appendChild(HY.histTabs("grid"));
    view.appendChild(root);
    view.appendChild(detail);

    function legend() {
      function item(cls, text, dot) {
        return HY.h("span", { class: "lg" }, HY.h("i", { class: "cell " + cls }, dot ? HY.h("b", { class: "dot" }) : null), text);
      }
      return HY.h("div", { class: "legend sm mut" },
        item("m-on_time", "On time"), item("m-late", "Late", true), item("m-missed", "Missed"),
        item("m-skipped", "Skipped"), item("m-untracked", "Not tracked"), item("m-none", "Nothing scheduled"));
    }

    function showDay(med, date) {
      picked = { med: med, date: date };
      detail.hidden = false;
      HY.clear(detail);
      detail.appendChild(HY.h("h2", null, med.name + ", " + HY.dayLabel(date)));
      var body = HY.h("div", null, "Loading");
      detail.appendChild(body);
      HY.deviceZone().then(function (zone) {
        var from = HY.wallToEpoch(date, "00:00", zone);
        var to = HY.wallToEpoch(HY.addDays(date, 1), "00:00", zone);
        return HY.api("GET", "/api/log?med=" + med.id + "&from=" + from + "&to=" + to + "&limit=100");
      }).then(function (r) {
        if (!HY.alive(token) || !picked || picked.date !== date || picked.med !== med) { return; }
        HY.clear(body);
        var list = r.log || [];
        if (!list.length) { body.textContent = "No entries on this day."; return; }
        list.forEach(function (e) {
          var t = (e.local || "").slice(11);
          body.appendChild(HY.h("div", { class: "entry" },
            HY.h("b", null, t), " ", HY.statusText(e.status),
            e.note ? HY.h("span", { class: "mut" }, ", " + e.note) : null));
        });
        body.appendChild(HY.h("a", { class: "btn", href: "#/log" }, "Open the log"));
      }, function (e) {
        if (HY.alive(token)) { body.textContent = HY.errText(e) || "Could not load the entries"; }
      });
    }

    function medBlock(m, first) {
      var cells = HY.h("div", { class: "grid30", style: { "--c": HY.medColor(m.color) }, role: "group", "aria-label": m.name + ", last 30 days" });
      (m.marks || []).forEach(function (mk, i) {
        var date = HY.addDays(first, i);
        var cls = "cell m-" + (MARK_TEXT[mk] ? mk : "none");
        var label = HY.dayLabel(date) + ": " + (MARK_TEXT[mk] || mk);
        cells.appendChild(HY.h("button", { class: cls, type: "button", title: label, "aria-label": label,
          onclick: function () { showDay(m, date); } },
          mk === "late" ? HY.h("b", { class: "dot" }) : null));
      });
      return HY.h("div", { class: "panel hmed" },
        HY.h("div", { class: "hhead" },
          HY.h("b", { style: { color: HY.medColor(m.color) } }, m.code), " ",
          HY.h("span", { class: "grow" }, m.name),
          HY.h("span", { class: "mut sm" }, pct(m.adherence))),
        cells);
    }

    function draw(r) {
      HY.clear(root);
      var meds = r.meds || [];
      if (!meds.length) { root.appendChild(HY.h("p", { class: "mut" }, "No active medicines yet.")); return; }
      root.appendChild(HY.h("p", null, "Last 30 days: " + pct(r.adherence)));
      meds.forEach(function (m) { root.appendChild(medBlock(m, r.first)); });
      root.appendChild(legend());
    }

    function load() {
      return HY.api("GET", "/api/history?days=30").then(function (r) {
        if (HY.alive(token)) { draw(r); }
      }, function (e) {
        if (!HY.alive(token)) { return; }
        HY.clear(root);
        var t = HY.errText(e);
        root.appendChild(HY.h("p", { class: "msg bad" }, e.error === "clock_invalid" ?
          "The device clock is not set, so there is no history to show yet." : (t || "Could not load the history")));
      });
    }

    load();
    return load;
  }, "history");
})();
