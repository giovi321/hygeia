// Wi-Fi: mode, saved networks, access point password, host name, keep Wi-Fi on battery.
(function () {
  var MODES = [["off", "Off"], ["client", "Wi-Fi"], ["ap", "Access point"]];

  function field(label, id, input, hint, errEl) {
    return HY.h("div", { class: "field" }, HY.h("label", { for: id }, label), input,
      hint ? HY.h("div", { class: "hint" }, hint) : null, errEl || null);
  }

  function stateText(w) {
    if (w.mode === "off") { return "Wi-Fi is off"; }
    if (w.mode === "ap") {
      return "Access point " + (w.ap_ssid || "") + " is on" + (w.ap_clients ? ", " + w.ap_clients + " connected" : "");
    }
    if (w.state === "connected") {
      return "Connected to " + w.ssid + (w.rssi !== null && w.rssi !== undefined ? ", " + w.rssi + " dBm" : "") + (w.ip ? ", " + w.ip : "");
    }
    return "Wi-Fi " + (w.state || "connecting") + (w.ssid ? " (" + w.ssid + ")" : "");
  }

  HY.route("/settings/wifi", function (params, view) {
    var token = HY.token;
    var root = HY.h("div");
    var w = null, keep = false;
    var pick = null;      // network chosen for client mode
    var timer = null;
    view.appendChild(HY.h("div", { class: "head" }, HY.h("h1", null, "Wi-Fi"),
      HY.h("a", { class: "btn", href: "#/settings" }, "Settings")));
    view.appendChild(root);
    HY.onLeave(function () { clearTimeout(timer); });

    function fail(e) { var t = HY.errText(e); if (t) { HY.toast(t); } }

    function hostUrl(host) { return "http://" + host + ".local"; }

    // Where to find the device after a mode change; the page loses its connection.
    function warning(target) {
      if (target === "client") {
        return "The page will lose its connection. Reconnect to " + (pick || (w.networks[0] && w.networks[0].ssid) || "your network") +
          " and open " + hostUrl(w.host);
      }
      if (target === "ap") {
        return "The page will lose its connection. Join the access point " + w.ap_ssid + " and open " + hostUrl(w.host);
      }
      return "The page will lose its connection. Turn Wi-Fi on again from the device or its power button";
    }

    function setMode(target) {
      if (!w || target === w.mode) { return; }
      var body = { mode: target };
      if (target === "client") {
        if (!w.networks.length) { HY.toast("Save a network first"); return; }
        body.ssid = pick || (w.ssid && indexOfNet(w.ssid) >= 0 ? w.ssid : w.networks[0].ssid);
        pick = body.ssid;
      }
      // Switching away from the access point or from Wi-Fi drops this page's connection.
      var loses = w.mode === "ap" || w.mode === "client";
      var go = function () {
        HY.api("POST", "/api/wifi/mode", body).then(function () {
          HY.toast(loses ? "Switching. The page will reconnect if it can" : "Wi-Fi mode changed");
          clearTimeout(timer);
          timer = setTimeout(load, 3000);
        }, fail);
      };
      if (!loses) { go(); return; }
      HY.confirm("Change the Wi-Fi mode?", warning(target), target !== "client" || w.mode === "ap", "Switch")
        .then(function (yes) { if (yes) { go(); } });
    }

    function indexOfNet(ssid) {
      for (var i = 0; i < w.networks.length; i++) { if (w.networks[i].ssid === ssid) { return i; } }
      return -1;
    }

    function modePanel() {
      var chips = MODES.map(function (m) {
        return HY.h("button", { class: "chip" + (w.mode === m[0] ? " on" : ""), type: "button", "aria-pressed": w.mode === m[0] ? "true" : "false",
          onclick: function () { setMode(m[0]); } }, m[1]);
      });
      var kids = [HY.h("div", { class: "seg" }, chips), HY.h("p", { class: "mut", id: "wifi-state" }, stateText(w))];
      if (w.networks.length > 1) {
        var sel = HY.h("select", { id: "w-pick", onchange: function () { pick = sel.value; } },
          w.networks.map(function (n) { return HY.h("option", { value: n.ssid }, n.ssid); }));
        sel.value = pick || w.ssid || w.networks[0].ssid;
        pick = sel.value;
        kids.push(field("Network to join", "w-pick", sel, "Used when you choose Wi-Fi above"));
      }
      return HY.h("section", { class: "panel" }, HY.h("h2", null, "Mode"), kids);
    }

    function netsPanel() {
      var list = w.networks.map(function (n) {
        return HY.h("div", { class: "brow" },
          HY.h("div", { class: "grow" }, HY.h("b", null, n.ssid),
            n.last_ok > 1700000000 ? HY.h("div", { class: "mut sm" }, "Last connected " + HY.fmtAgo(n.last_ok)) : null),
          HY.h("button", { class: "btn danger", type: "button", onclick: function () {
            HY.confirm("Forget " + n.ssid + "?", "The device no longer joins this network.", true, "Forget").then(function (yes) {
              if (!yes) { return; }
              HY.api("DELETE", "/api/wifi/networks", { ssid: n.ssid }).then(function () { HY.toast("Network forgotten"); load(); }, fail);
            });
          } }, "Forget"));
      });
      if (!list.length) { list = [HY.h("p", { class: "mut" }, "No saved networks.")]; }
      var ssid = HY.h("input", { id: "w-ssid", type: "text", maxlength: "32", autocomplete: "off", autocapitalize: "off", spellcheck: "false" });
      var pass = HY.h("input", { id: "w-pass", type: "password", maxlength: "63", autocomplete: "new-password" });
      var eS = HY.h("div", { class: "ferr", role: "alert" }), eP = HY.h("div", { class: "ferr", role: "alert" });
      var add = HY.h("button", { class: "btn p", type: "button", onclick: function () {
        eS.textContent = eP.textContent = "";
        if (!ssid.value) { eS.textContent = "Enter the network name"; return; }
        add.disabled = true;
        var body = { ssid: ssid.value };
        if (pass.value) { body.password = pass.value; }
        HY.api("POST", "/api/wifi/networks", body).then(function () {
          pass.value = "";
          HY.toast("Network saved");
          load();
        }, function (e) {
          add.disabled = false;
          if (e.field === "ssid") { eS.textContent = e.message; }
          else if (e.field === "password") { eP.textContent = e.message; }
          else { fail(e); }
        });
      } }, "Save network");
      return HY.h("section", { class: "panel" }, HY.h("h2", null, "Saved networks"), list,
        HY.h("h3", null, "Add a network"),
        field("Network name", "w-ssid", ssid, null, eS),
        field("Password", "w-pass", pass, "Leave empty for an open network", eP),
        HY.h("div", { class: "actions" }, add));
    }

    function apPanel() {
      var pass = HY.h("input", { id: "w-appass", type: "password", maxlength: "63", autocomplete: "new-password", disabled: false });
      var show = HY.h("button", { class: "btn", type: "button", disabled: true, "aria-pressed": "false", onclick: function () {
        var on = pass.type === "password";
        pass.type = on ? "text" : "password";
        show.textContent = on ? "Hide" : "Show";
        show.setAttribute("aria-pressed", on ? "true" : "false");
      } }, "Show");
      pass.addEventListener("input", function () { show.disabled = !pass.value; });
      var eP = HY.h("div", { class: "ferr", role: "alert" });
      var setP = HY.h("button", { class: "btn p", type: "button", onclick: function () {
        eP.textContent = "";
        if (pass.value.length < 8 || pass.value.length > 63) { eP.textContent = "The password is 8 to 63 characters"; return; }
        setP.disabled = true;
        HY.api("PUT", "/api/wifi/ap", { password: pass.value }).then(function () {
          pass.value = ""; pass.type = "password"; show.textContent = "Show"; show.disabled = true; setP.disabled = false;
          HY.toast("Access point password changed");
        }, function (e) {
          setP.disabled = false;
          if (e.field === "password") { eP.textContent = e.message; } else { fail(e); }
        });
      } }, "Change password");
      var host = HY.h("input", { id: "w-host", type: "text", maxlength: "32", value: w.host, autocomplete: "off", autocapitalize: "off", spellcheck: "false" });
      var eH = HY.h("div", { class: "ferr", role: "alert" });
      var setH = HY.h("button", { class: "btn p", type: "button", onclick: function () {
        eH.textContent = "";
        if (!/^[a-z0-9][a-z0-9-]{0,31}$/.test(host.value)) {
          eH.textContent = "1 to 32 of a to z, 0 to 9 and -, not starting with -"; return;
        }
        setH.disabled = true;
        HY.api("PUT", "/api/wifi/ap", { host: host.value }).then(function (r) {
          setH.disabled = false;
          HY.toast("Host name is now " + r.host);
          load();
        }, function (e) {
          setH.disabled = false;
          if (e.field === "host") { eH.textContent = e.message; } else { fail(e); }
        });
      } }, "Change host name");
      return HY.h("section", { class: "panel" }, HY.h("h2", null, "Access point"),
        HY.h("p", { class: "mut sm" }, "Name: " + w.ap_ssid + ". The name is fixed."),
        field("New password", "w-appass", pass, "Shown as dots. Use Show to check what you typed", eP),
        HY.h("div", { class: "actions" }, show, setP),
        HY.h("h3", null, "Host name"),
        field("Host name", "w-host", host, "The device answers at " + hostUrl(w.host), eH),
        HY.h("div", { class: "actions" }, setH));
    }

    function keepPanel() {
      var c = HY.h("input", { id: "w-keep", type: "checkbox", onchange: function () {
        HY.api("PUT", "/api/settings", { wifi_keep_batt: c.checked }).then(function (s) {
          keep = !!s.wifi_keep_batt; c.checked = keep;
        }, function (e) { c.checked = keep; fail(e); });
      } });
      c.checked = keep;
      return HY.h("section", { class: "panel" }, HY.h("h2", null, "Battery"),
        HY.h("label", { class: "check", for: "w-keep" }, c, "Keep Wi-Fi on battery"),
        HY.h("div", { class: "hint" }, "Off: Wi-Fi switches off when the device runs on battery, which saves power"));
    }

    function draw() {
      HY.clear(root);
      root.appendChild(modePanel());
      root.appendChild(netsPanel());
      root.appendChild(apPanel());
      root.appendChild(keepPanel());
    }

    function load() {
      return Promise.all([HY.api("GET", "/api/wifi"), HY.api("GET", "/api/settings")]).then(function (r) {
        if (!HY.alive(token)) { return; }
        w = r[0]; keep = !!r[1].wifi_keep_batt;
        w.networks = w.networks || [];
        draw();
      }, function (e) {
        if (!HY.alive(token)) { return; }
        HY.clear(root);
        root.appendChild(HY.h("p", { class: "msg bad" }, HY.errText(e) || "The device did not answer"));
        root.appendChild(HY.h("button", { class: "btn", type: "button", onclick: load }, "Try again"));
      });
    }

    load();
    return function () {
      var a = document.activeElement;
      if (a && a.tagName === "INPUT" && root.contains(a)) { return; }
      return load();
    };
  }, "settings");
})();
