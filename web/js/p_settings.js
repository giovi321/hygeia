// Settings: screen and sound, quiet hours, rotation lock, clock, paired browsers, power off.
// Other pages add their entry to HY.settingsLinks and it is listed at the top.
HY.settingsLinks = HY.settingsLinks || [];
HY.settingsLinks.push({ href: "#/settings/wifi", text: "Wi-Fi", note: "Mode, saved networks, access point, host name" });

(function () {
  function section(title, kids) {
    return HY.h("section", { class: "panel" }, HY.h("h2", null, title), kids);
  }
  function field(label, id, input, hint) {
    return HY.h("div", { class: "field" }, HY.h("label", { for: id }, label), input,
      hint ? HY.h("div", { class: "hint" }, hint) : null);
  }

  HY.route("/settings", function (params, view) {
    var token = HY.token;
    var root = HY.h("div");
    var cur = null;   // settings as the device holds them
    var ctl = {};     // controls by key, so the device's clamped answer can be shown
    var clockBox = HY.h("div");
    var clockTimer = null;
    var offset = 0;   // device clock minus browser clock, ms
    var zoneNow = "";

    view.appendChild(HY.h("h1", null, "Settings"));
    view.appendChild(root);
    HY.onLeave(function () { clearInterval(clockTimer); });

    function save(patch) {
      return HY.api("PUT", "/api/settings", patch).then(function (s) {
        if (!HY.alive(token)) { return; }
        cur = s; sync();
        return s;
      }, function (e) {
        var t = HY.errText(e); if (t) { HY.toast(t); }
        if (HY.alive(token)) { sync(); }
      });
    }

    // show the stored values (the device clamps what it was sent)
    function sync() {
      if (!cur) { return; }
      var k;
      for (k in ctl) {
        var c = ctl[k];
        if (!(k in cur)) { continue; }
        if (c.type === "checkbox") { c.checked = !!cur[k]; }
        else if (document.activeElement !== c) { c.value = cur[k]; }
      }
      if (ctl.volume) { ctl.volume_out.textContent = cur.volume + "%"; }
      if (ctl.brightness) { ctl.brightness_out.textContent = cur.brightness + "%"; }
      if (ctl.quiet_on) {
        ctl.quiet_on.checked = !!cur.quiet;
        ctl.quiet_from.disabled = ctl.quiet_to.disabled = !cur.quiet;
        if (cur.quiet) { ctl.quiet_from.value = cur.quiet.from; ctl.quiet_to.value = cur.quiet.to; }
      }
    }

    function check(key, label, hint) {
      var c = HY.h("input", { id: "s-" + key, type: "checkbox", onchange: function () {
        var p = {}; p[key] = c.checked; save(p);
      } });
      ctl[key] = c;
      return HY.h("div", null, HY.h("label", { class: "check", for: "s-" + key }, c, label),
        hint ? HY.h("div", { class: "hint" }, hint) : null);
    }

    function range(key, label, min, max) {
      var out = HY.h("span", { class: "mut" });
      var c = HY.h("input", { id: "s-" + key, type: "range", min: String(min), max: String(max), step: "1",
        oninput: function () { out.textContent = c.value + "%"; },
        onchange: function () { var p = {}; p[key] = parseInt(c.value, 10); save(p); } });
      ctl[key] = c; ctl[key + "_out"] = out;
      return HY.h("div", { class: "field" },
        HY.h("label", { for: "s-" + key }, label, " ", out), c);
    }

    function number(key, label, min, max, hint) {
      var c = HY.h("input", { id: "s-" + key, type: "number", inputmode: "numeric", min: String(min), max: String(max), step: "1",
        onchange: function () {
          var v = parseInt(c.value, 10);
          if (isNaN(v)) { sync(); return; }
          var p = {}; p[key] = v; save(p);
        } });
      ctl[key] = c;
      return field(label, "s-" + key, c, hint);
    }

    function screenSound() {
      var chime = HY.h("button", { class: "btn", type: "button", onclick: function () {
        HY.api("POST", "/api/chime/test").then(function (r) {
          if (r && r.played === false) { HY.toast("Sound is not available on the device"); }
        }, function (e) { var t = HY.errText(e); if (t) { HY.toast(t); } });
      } }, "Play chime");
      return section("Screen and sound", [
        range("volume", "Volume", 0, 100),
        HY.h("div", { class: "actions" }, chime),
        range("brightness", "Brightness", 10, 100),
        check("sound", "Sound on"),
        check("dnd", "Do not disturb", "Reminders show on the screen and stay silent"),
        number("snooze_min", "Snooze (minutes)", 1, 120),
        number("dim_s", "Dim the screen after (seconds)", 10, 600),
        number("off_bat_s", "Screen off on battery after (seconds)", 30, 3600),
        number("off_usb_s", "Screen off on USB after (seconds)", 0, 86400, "0 keeps the screen on. Otherwise 60 or more")
      ]);
    }

    function quiet() {
      var on = HY.h("input", { id: "s-quiet-on", type: "checkbox", onchange: function () {
        if (on.checked) {
          save({ quiet: { from: ctl.quiet_from.value || "22:00", to: ctl.quiet_to.value || "07:00" } });
        } else { save({ quiet: null }); }
      } });
      var from = HY.h("input", { id: "s-quiet-from", type: "time", value: "22:00", onchange: send });
      var to = HY.h("input", { id: "s-quiet-to", type: "time", value: "07:00", onchange: send });
      function send() { if (on.checked && from.value && to.value) { save({ quiet: { from: from.value, to: to.value } }); } }
      ctl.quiet_on = on; ctl.quiet_from = from; ctl.quiet_to = to;
      return section("Quiet hours", [
        HY.h("label", { class: "check", for: "s-quiet-on" }, on, "Quiet hours on"),
        HY.h("div", { class: "row2" }, field("From", "s-quiet-from", from), field("To", "s-quiet-to", to)),
        HY.h("div", { class: "hint" }, "Reminders stay silent in this period")
      ]);
    }

    function rotation() {
      var shown = HY.h("div", { class: "hint", id: "s-rot" });
      ctl.rot_show = shown;
      return section("Rotation lock", [
        check("rot_lock", "Lock the screen rotation", "The screen keeps its current orientation"),
        shown
      ]);
    }

    // ---- clock ----
    function fmtDevice(ms) {
      try {
        return new Intl.DateTimeFormat("en-GB", { timeZone: zoneNow || undefined, dateStyle: "medium", timeStyle: "medium", hour12: false }).format(new Date(ms));
      } catch (e) { return new Date(ms).toISOString(); }
    }

    function drawClock(info, st) {
      HY.clear(clockBox);
      zoneNow = st.zone || "";
      offset = info.utc_ms - Date.now();
      var shown = HY.h("b", { id: "clock-now" }, info.clock_valid ? fmtDevice(Date.now() + offset) : "Not set");
      clearInterval(clockTimer);
      if (info.clock_valid) {
        clockTimer = setInterval(function () { shown.textContent = fmtDevice(Date.now() + offset); }, 1000);
      }
      var syncText = st.sync && st.sync.at ? "Last sync " + HY.fmtAgo(st.sync.at) + " via " + (st.sync.src || "unknown") : "Not synced yet";
      var list = null;
      var zones = [];
      try { zones = Intl.supportedValuesOf("timeZone"); } catch (e) { zones = []; }
      if (zones.indexOf("UTC") < 0) { zones.push("UTC"); }
      list = HY.h("datalist", { id: "zone-list" }, zones.map(function (z) { return HY.h("option", { value: z }); }));
      var zin = HY.h("input", { id: "s-zone", type: "text", value: zoneNow, list: "zone-list", autocomplete: "off",
        autocapitalize: "off", spellcheck: "false" });
      var zerr = HY.h("div", { class: "ferr", role: "alert" });
      var setZone = HY.h("button", { class: "btn", type: "button", onclick: function () {
        zerr.textContent = "";
        if (!zin.value) { zerr.textContent = "Choose a time zone"; return; }
        setZone.disabled = true;
        HY.api("POST", "/api/time", { utc_ms: Date.now(), zone: zin.value }).then(function () {
          HY.state = null;
          HY.toast("Time zone is now " + zin.value);
          loadClock();
        }, function (e) {
          setZone.disabled = false;
          zerr.textContent = e.field === "zone" ? "The device does not know that time zone" : (HY.errText(e) || "");
        });
      } }, "Set time zone");
      var syncBtn = HY.h("button", { class: "btn p", type: "button", onclick: function () {
        syncBtn.disabled = true;
        HY.api("POST", "/api/time", { utc_ms: Date.now() }).then(function () {
          HY.toast("Clock set from this browser");
          loadClock();
        }, function (e) {
          syncBtn.disabled = false;
          var t = HY.errText(e); if (t) { HY.toast(t); }
        });
      } }, "Sync from this browser");
      clockBox.appendChild(HY.h("div", null, "Device time: ", shown));
      clockBox.appendChild(HY.h("div", { class: "mut sm" }, syncText));
      clockBox.appendChild(field("Time zone", "s-zone", zin));
      clockBox.appendChild(list);
      clockBox.appendChild(zerr);
      clockBox.appendChild(HY.h("div", { class: "actions" }, setZone, syncBtn));
    }

    function loadClock() {
      return Promise.all([HY.api("GET", "/api/info"), HY.api("GET", "/api/state")]).then(function (r) {
        if (!HY.alive(token)) { return; }
        HY.setState(r[1]);
        drawClock(r[0], r[1]);
      }, function (e) {
        if (HY.alive(token)) { HY.clear(clockBox); clockBox.appendChild(HY.h("p", { class: "msg bad" }, HY.errText(e))); }
      });
    }

    // ---- paired browsers ----
    var brBox = HY.h("div");
    function forgetNow(body, wasCurrent, text) {
      HY.api("DELETE", "/api/browsers", body).then(function () {
        if (wasCurrent) {
          HY.info.paired = false; HY.ws.stop(); HY.go("/pair");
        } else {
          HY.toast(text); loadBrowsers();
        }
      }, function (e) { var t = HY.errText(e); if (t) { HY.toast(t); } loadBrowsers(); });
    }

    function drawBrowsers(r) {
      HY.clear(brBox);
      var list = r.browsers || [];
      if (!list.length) { brBox.appendChild(HY.h("p", { class: "mut" }, "No paired browsers.")); return; }
      list.forEach(function (b) {
        var when = b.created ? "Paired " + new Date(b.created * 1000).toLocaleDateString("en-GB") : "Paired, date unknown";
        brBox.appendChild(HY.h("div", { class: "brow" },
          HY.h("div", { class: "grow" }, HY.h("b", null, b.label || "Browser"), b.current ? " (this browser)" : "",
            HY.h("div", { class: "mut sm" }, when)),
          HY.h("button", { class: "btn danger", type: "button", onclick: function () {
            HY.confirm("Forget " + (b.current ? "this browser" : (b.label || "this browser")) + "?",
              b.current ? "You will need the code from the device to pair this browser again." :
                "That browser has to pair again to use the device.", true, "Forget")
              .then(function (yes) { if (yes) { forgetNow({ index: b.index }, b.current, "Browser forgotten"); } });
          } }, "Forget")));
      });
      if (list.length > 1) {
        brBox.appendChild(HY.h("div", { class: "actions" }, HY.h("button", { class: "btn danger", type: "button", onclick: function () {
          HY.confirm("Forget all browsers?", "Every browser, this one included, has to pair again.", true, "Forget all")
            .then(function (yes) { if (yes) { forgetNow({ all: true }, true, ""); } });
        } }, "Forget all")));
      }
    }

    function loadBrowsers() {
      return HY.api("GET", "/api/browsers").then(function (r) {
        if (HY.alive(token)) { drawBrowsers(r); }
      }, function (e) {
        if (HY.alive(token)) { HY.clear(brBox); brBox.appendChild(HY.h("p", { class: "msg bad" }, HY.errText(e))); }
      });
    }

    // ---- power off ----
    function powerOff() {
      function ask(due) {
        return HY.confirm("Power off the device?",
          (due ? "A dose is due within the next 24 hours. The device will not remind you while it is off. " : "") +
          "Press the power button on the device to switch it on again.", true, "Power off");
      }
      function done() { HY.toast("The device is powering off"); }
      HY.api("POST", "/api/power/off", {}).then(done, function (e) {
        if (e.status === 409 && e.data && e.data.error === "confirm") {
          ask(!!e.data.due_24h).then(function (yes) {
            if (!yes) { return; }
            HY.api("POST", "/api/power/off", { confirm: true }).then(done, function (er) {
              var t = HY.errText(er); if (t) { HY.toast(t); }
            });
          });
          return;
        }
        var t = HY.errText(e); if (t) { HY.toast(t); }
      });
    }

    function build() {
      HY.clear(root);
      if (HY.settingsLinks.length) {
        root.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "More settings"),
          HY.settingsLinks.map(function (l) {
            return HY.h("a", { class: "linkrow", href: l.href }, HY.h("b", null, l.text), HY.h("span", { class: "mut sm" }, l.note));
          })));
      }
      root.appendChild(screenSound());
      root.appendChild(quiet());
      root.appendChild(rotation());
      root.appendChild(section("Clock", clockBox));
      root.appendChild(section("Paired browsers", brBox));
      root.appendChild(section("Integrations", [
        HY.h("p", { class: "mut" }, "Coming in a later version"),
        HY.h("p", { class: "mut sm" }, "MQTT, Home Assistant and webhooks")]));
      root.appendChild(section("Power off", [
        HY.h("p", { class: "mut" }, "Turns the device off. It stops reminding until you switch it on."),
        HY.h("div", { class: "actions" }, HY.h("button", { class: "btn danger", type: "button", onclick: powerOff }, "Power off"))]));
    }

    function load() {
      return HY.api("GET", "/api/settings").then(function (s) {
        if (!HY.alive(token)) { return; }
        cur = s;
        if (!ctl.volume) { build(); }
        sync();
        ctl.rot_show.textContent = "Current orientation: position " + s.rot + " of 0 to 3";
      }, function (e) {
        if (HY.alive(token)) { HY.clear(root); root.appendChild(HY.h("p", { class: "msg bad" }, HY.errText(e) || "Could not load the settings")); }
      });
    }

    load().then(function () { loadClock(); loadBrowsers(); });
    // A device event refetches the settings only while nothing is being edited.
    return function () {
      var a = document.activeElement;
      if (a && a.tagName === "INPUT" && root.contains(a)) { return; }
      load();
    };
  }, "settings");
})();
