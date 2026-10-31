// Dashboard: one ring tile per active medicine, due actions, device line.

// Lines for the device panel and the sidebar.
HY.deviceLines = function (s) {
  var out = [];
  var b = s.battery || {};
  var bt = "Battery " + b.pct + "%";
  if (b.charging) { bt += ", charging"; } else if (b.usb) { bt += ", on USB"; } else if (b.estimate) { bt += ", about " + b.estimate + " left"; }
  out.push(bt);
  var n = s.net || {};
  if (n.mode === "client") {
    out.push(n.state === "connected" ? "Wi-Fi " + n.ssid + ", " + n.rssi + " dBm" : "Wi-Fi " + (n.state || "connecting"));
  } else if (n.mode === "ap") {
    out.push("Access point on");
  } else {
    out.push("Wi-Fi off");
  }
  if (!s.clock_valid) { out.push("Clock not set"); }
  else if (s.sync && s.sync.at) { out.push("Clock synced " + HY.fmtAgo(s.sync.at) + " via " + (s.sync.src || "unknown")); }
  else { out.push("Clock not synced yet"); }
  return out;
};

HY.setState = function (s) {
  HY.state = s;
  var el = document.getElementById("devline");
  if (!el) { return; }
  HY.clear(el);
  HY.deviceLines(s).forEach(function (l) { el.appendChild(HY.h("div", null, l)); });
};

HY.route("/", function (params, view) {
  var token = HY.token;
  var root = HY.h("div");
  var busy = false;
  view.appendChild(HY.h("h1", null, "Dashboard"));
  view.appendChild(root);

  function load() {
    return HY.api("GET", "/api/state").then(function (s) {
      if (!HY.alive(token)) { return; }
      HY.setState(s);
      draw(s);
    }, function (e) {
      if (!HY.alive(token)) { return; }
      if (!HY.state) { HY.clear(root); root.appendChild(HY.h("p", { class: "msg bad" }, HY.errText(e) || "")); }
    });
  }

  function after(promise, okText, undoable) {
    busy = true;
    return promise.then(function (r) {
      busy = false;
      if (r && r.log_id && undoable) {
        HY.toast(okText, function () {
          HY.api("POST", "/api/log/" + r.log_id + "/undo").then(load, function (e) { HY.toast(HY.errText(e)); load(); });
        });
      } else {
        HY.toast(okText);
      }
      return load();
    }, function (e) {
      busy = false;
      var t = HY.errText(e);
      if (t) { HY.toast(t); }
      return load();
    });
  }

  function doTaken(t, due) {
    if (busy) { return; }
    var body = {};
    if (due) { body.slot = due.slot; }
    var send = function (b) { return HY.api("POST", "/api/medicines/" + t.id + "/log", b); };
    busy = true;
    send(body).then(function (r) {
      busy = false;
      return after(Promise.resolve(r), "Logged " + t.name, true);
    }, function (e) {
      busy = false;
      if (e.error === "too_soon") {
        HY.confirm("Taken too soon?", e.message, false, "Log it anyway").then(function (yes) {
          if (!yes) { return; }
          body.confirm = true;
          after(send(body), "Logged " + t.name, true);
        });
        return;
      }
      var tx = HY.errText(e);
      if (tx) { HY.toast(tx); }
      load();
    });
  }

  function doSkip(t, due) {
    if (!busy) { after(HY.api("POST", "/api/medicines/" + t.id + "/skip", { slot: due.slot }), "Skipped " + t.name, true); }
  }

  function doSnooze(t, due) {
    if (!busy) { after(HY.api("POST", "/api/medicines/" + t.id + "/snooze", { slot: due.slot }), "Snoozed " + t.name, false); }
  }

  function statusOf(t, due, zone) {
    var l = [];
    var warn = false, bad = false;
    if (due) {
      if (due.state === "snoozed") { l.push("Snoozed until " + HY.fmtTime(due.snoozed_until, zone)); warn = true; }
      else if (due.state === "late") { l.push("Late, due " + HY.fmtTime(due.slot, zone)); warn = true; }
      else if (due.state === "missed") { l.push("Missed, due " + HY.fmtTime(due.slot, zone)); bad = true; }
      else { l.push("Due " + HY.fmtTime(due.slot, zone)); warn = true; }
    } else if (t.kind === "as_needed") { l.push("As needed"); }
    else if (t.kind === "no_dose") { l.push("No dose scheduled"); }
    else if (t.kind === "missed") { l.push("Missed"); bad = true; }
    else if (t.next_due) { l.push("Next " + HY.fmtTime(t.next_due, zone)); }
    else { l.push("Nothing scheduled soon"); }
    return { text: l[0], warn: warn, bad: bad };
  }

  function tile(t, dueMap, zone) {
    var due = dueMap[t.id];
    var st = statusOf(t, due, zone);
    var color = HY.medColor(t.color);
    var ringColor = st.bad ? "#E5484D" : (st.warn ? "#F6C453" : color);
    var pct = Math.max(0, Math.min(1000, t.ring | 0)) / 10;
    var info = HY.h("div", { class: "grow" },
      HY.h("b", null, t.name),
      HY.h("div", { class: st.bad ? "bad" : (st.warn ? "warn" : "mut") }, st.text),
      HY.h("div", { class: "mut sm" },
        t.last_taken ? "Last taken " + HY.fmtAgo(t.last_taken) : "Not taken yet",
        t.pills && t.pills_left !== null ? " · " + t.pills_left + " left" : ""));
    var actions;
    if (due) {
      actions = HY.h("div", { class: "actions" },
        HY.h("button", { class: "btn p", type: "button", onclick: function () { doTaken(t, due); } }, "Taken"),
        HY.h("button", { class: "btn", type: "button", onclick: function () { doSkip(t, due); } }, "Skip"),
        due.state === "missed" ? null :
          HY.h("button", { class: "btn", type: "button", onclick: function () { doSnooze(t, due); } }, "Snooze"));
    } else {
      actions = HY.h("div", { class: "actions" },
        HY.h("button", { class: "btn", type: "button", onclick: function () { doTaken(t, null); } },
          t.kind === "as_needed" ? "Taken" : "Log a dose"));
    }
    return HY.h("div", { class: "panel tile" + (st.bad ? " is-bad" : (st.warn ? " is-warn" : "")) },
      HY.h("div", { class: "tile-top" },
        HY.h("div", { class: "ring", style: { "--p": pct, "--c": ringColor } },
          HY.h("b", { style: { color: color } }, t.code)),
        info),
      actions);
  }

  function draw(s) {
    HY.clear(root);
    if (!s.clock_valid) {
      var fix = HY.h("button", { class: "btn p", type: "button", onclick: function () {
        fix.disabled = true;
        HY.api("POST", "/api/time", { utc_ms: Date.now() }).then(load, function (e) {
          fix.disabled = false;
          var t = HY.errText(e); if (t) { HY.toast(t); }
        });
      } }, "Set from this browser");
      root.appendChild(HY.h("div", { class: "banner bad" },
        HY.h("p", null, "The device clock is not set. Reminders are paused and logging is off until it is."),
        HY.h("div", { class: "actions" }, fix)));
    }
    var zb = HY.timesync.banner(s.zone, load);
    if (zb) { root.appendChild(zb); }

    var dueMap = {};
    (s.due || []).forEach(function (d) { dueMap[d.med_id] = d; });
    var grid = HY.h("div", { class: "tiles" });
    (s.tiles || []).forEach(function (t) { grid.appendChild(tile(t, dueMap, s.zone)); });
    if (!(s.tiles || []).length) {
      grid.appendChild(HY.h("p", { class: "mut" }, "No active medicines. Add one under Medicines."));
    }
    root.appendChild(grid);

    var dev = HY.h("div", { class: "panel" }, HY.h("div", { class: "k" }, "Device"));
    HY.deviceLines(s).forEach(function (l) { dev.appendChild(HY.h("div", null, l)); });
    if (s.zone) { dev.appendChild(HY.h("div", { class: "mut" }, "Time zone " + s.zone)); }
    root.appendChild(dev);
  }

  HY.timesync.run().then(function (changed) { if (changed && HY.alive(token)) { load(); } });
  load();
  var iv = setInterval(function () { if (HY.state && !busy) { draw(HY.state); } }, 30000);
  HY.onLeave(function () { clearInterval(iv); });
  return load;
}, "dash");
