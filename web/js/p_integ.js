// Integrations on the Settings page: MQTT, Home Assistant discovery and webhooks, with the delivery status.
// HY.integPanels(root, cur, save) adds the panels to root. cur is the settings answer, save(patch) sends a settings
// PUT and resolves with the new settings (it rejects with the API error). It returns { sync(cur) }.
// The status comes from GET /api/integrations: once when the panels are built, then after each {"ev":"integ"} live
// message (HY.onInteg, at most once a second) and while a webhook test runs. Nothing is fetched on other pages.
(function () {
  var EVENTS = [
    ["taken", "Taken"], ["skipped", "Skipped"], ["missed", "Missed"], ["due", "Due"], ["snoozed", "Snoozed"],
    ["undone", "Undone"], ["battery_low", "Battery low"], ["battery_critical", "Battery critical"],
    ["clock_invalid", "Clock invalid"]
  ];
  var REASON = {
    memory: "Waiting for free memory",
    busy: "Waiting for an update or backup to finish",
    auth: "The receiver refused the credentials. Fix them and save the webhook again",
    offline: "Waiting for a Wi-Fi connection",
    tls: "The secure connection could not be set up",
    connect: "Could not connect",
    timeout: "The receiver did not answer in time",
    http: "The receiver answered with an error",
    storage: "Storage failed, events are kept in memory only"
  };
  var MAX_HOOKS = 4;

  function reason(name) { return REASON[name] || name; }

  function inSeconds(s) {
    if (s < 90) { return s + " s"; }
    return Math.round(s / 60) + " min";
  }

  function invalid(input, msg, errBox) {
    if (input) {
      if (msg) { input.setAttribute("aria-invalid", "true"); } else { input.removeAttribute("aria-invalid"); }
    }
    if (errBox) { errBox.textContent = msg || ""; }
  }

  function textField(id, label, value, placeholder, hint) {
    var input = HY.h("input", { id: id, type: "text", value: value || "", placeholder: placeholder || null,
      autocomplete: "off", autocapitalize: "off", spellcheck: "false" });
    var err = HY.h("div", { class: "ferr", role: "alert" });
    input.addEventListener("input", function () { invalid(input, "", err); });
    return { input: input, err: err, box: HY.h("div", { class: "field" }, HY.h("label", { for: id }, label), input,
      hint ? HY.h("div", { class: "hint" }, hint) : null, err) };
  }

  function checkField(id, label, checked) {
    var c = HY.h("input", { id: id, type: "checkbox" });
    c.checked = !!checked;
    return { input: c, box: HY.h("label", { class: "check", for: id }, c, label) };
  }

  // A write-only value (password, secret): the input only holds what was typed.
  // state(): "" untouched, otherwise the typed text; cleared() is true after Clear.
  function secretField(id, label) {
    var f = textField(id, label, "", "Not set");
    var input = f.input;
    var isSet = false, cleared = false;
    input.type = "password";
    input.setAttribute("autocomplete", "new-password");
    var clear = HY.h("button", { class: "btn", type: "button", onclick: function () {
      input.value = ""; cleared = true; draw(); fireChange();
    } }, "Clear");
    var onChange = null;
    function fireChange() { if (onChange) { onChange(); } }
    function draw() {
      input.placeholder = cleared ? "Cleared when you save" : (isSet ? "Saved" : "Not set");
      clear.disabled = !(isSet && !cleared) && !input.value;
    }
    input.addEventListener("input", function () { cleared = false; draw(); fireChange(); });
    var row = HY.h("div", { class: "row2 sec" }, HY.h("div", { class: "field" }, HY.h("label", { for: id }, label), input), clear);
    f.box = HY.h("div", null, row, f.err);
    draw();
    return {
      box: f.box, input: input, err: f.err,
      setSaved: function (set) { isSet = !!set; cleared = false; input.value = ""; draw(); },
      typed: function () { return input.value; },
      cleared: function () { return cleared; },
      dirty: function () { return cleared || input.value !== ""; },
      onChange: function (fn) { onChange = fn; }
    };
  }

  function section(title, kids) {
    return HY.h("section", { class: "panel" }, HY.h("h2", null, title), kids);
  }

  function changed(a, b) { return JSON.stringify(a) !== JSON.stringify(b); }

  HY.integPanels = function (root, cur, save) {
    var token = HY.token;
    var cfg = { mqtt: cur.mqtt || {}, webhooks: cur.webhooks || [] };
    var status = null;
    var fetchId = 0, lastFetchAt = 0, fetchTimer = null, fetching = false;

    // ---------------------------------------------------------------- status

    function mqttLine() {
      var m = status && status.mqtt;
      if (!m) { return ""; }
      var label = { off: "Off", waiting: "Waiting", connecting: "Connecting", connected: "Connected", error: "Not connected" }[m.state] || m.state;
      var parts = [label];
      if (m.error) { parts.push(reason(m.error)); }
      if (m.state === "connected" && m.since) { parts.push("since " + HY.fmtTime(m.since, HY.state && HY.state.zone)); }
      if (m.pending) { parts.push(m.pending + " waiting"); }
      return parts.join(". ");
    }

    function hookLine(i) {
      var w = status && status.webhooks && status.webhooks[i];
      if (!w) { return ""; }
      var parts = [];
      if (w.paused || (w.last && w.last.error === "auth")) {
        parts.push(reason("auth"));
      } else if (w.last && w.last.error) {
        var t = reason(w.last.error);
        if (w.last.error === "http" && w.last.status) { t += " (" + w.last.status + ")"; }
        parts.push(t);
        if (w.retry_in_s !== null && w.retry_in_s !== undefined) { parts.push("Next try in " + inSeconds(w.retry_in_s)); }
      } else if (w.last && w.last.at) {
        parts.push("Last sent " + HY.fmtAgo(w.last.at) + (w.last.status ? " (" + w.last.status + ")" : ""));
      } else {
        parts.push("Nothing sent yet");
      }
      if (w.pending) { parts.push(w.pending + " waiting"); }
      return parts.join(". ");
    }

    function outboxLine() {
      var o = status && status.outbox;
      if (!o) { return ""; }
      var t = "Waiting: " + o.pending + ". Dropped when full: " + o.dropped + ". Expired: " + o.expired +
        ". Rejected: " + o.rejected + ".";
      if (o.lost) { t += " Lost: " + o.lost + "."; }
      if (o.error) { t += " " + reason(o.error) + "."; }
      return t;
    }

    function testResult(slot, st) {
      var t = st.test;
      if (t.error) {
        return "Test failed. " + reason(t.error) + (t.error === "http" && t.status ? " (" + t.status + ")" : "");
      }
      return "Test delivered" + (t.status ? " (" + t.status + ")" : "");
    }

    function applyStatus(st, id) {
      status = st;
      statusAll(id);
    }

    function statusAll(id) {
      id = id || 0;   // only a fetch that started after the test request can end a test
      mqttStatus.textContent = mqttLine() || (status ? "" : "Status is not available");
      outbox.textContent = outboxLine();
      slots.forEach(function (s, i) {
        if (!s.el) { return; }
        s.status.textContent = s.saved() ? hookLine(i) : "";
        if (s.testing && status && status.test && !status.test.running && status.test.index === i && id > s.testAfter) {
          s.testing = false;
          clearTimeout(s.testTimer);
          s.testMsg = testResult(s, status);
        }
        s.testOut.textContent = s.testing ? "Sending a test event" : s.testMsg;
        s.testBtn.disabled = s.testing || s.dirty() || !s.saved();
      });
    }

    function fetchStatus() {
      if (!HY.alive(token)) { return; }
      var wait = 1000 - (Date.now() - lastFetchAt);
      if (fetching || wait > 0) {
        clearTimeout(fetchTimer);
        fetchTimer = setTimeout(fetchStatus, Math.max(wait, 100));
        return;
      }
      fetching = true; lastFetchAt = Date.now();
      var id = ++fetchId;
      HY.api("GET", "/api/integrations").then(function (st) {
        fetching = false;
        if (HY.alive(token)) { applyStatus(st, id); }
      }, function () {
        fetching = false;
        if (HY.alive(token) && !status) { statusAll(); }
      });
    }

    HY.onInteg = function () { fetchStatus(); };
    HY.onLeave(function () {
      HY.onInteg = null;
      clearTimeout(fetchTimer);
      slots.forEach(function (s) { clearTimeout(s.testTimer); });
    });

    // ---------------------------------------------------------------- MQTT

    var mqttStatus = HY.h("p", { class: "mut sm", role: "status" });
    var mOn = checkField("i-mqtt-on", "On", false);
    var mUrl = textField("i-mqtt-url", "Broker URL", "", "mqtt://broker.lan:1883", "mqtt:// or mqtts://. Secure connections need a certificate from a public authority");
    var mUser = textField("i-mqtt-user", "User", "");
    var mPass = secretField("i-mqtt-pass", "Password");
    var mBase = textField("i-mqtt-base", "Base topic", "", "hygeia");
    var mSave = HY.h("button", { class: "btn p", type: "button", onclick: saveMqtt }, "Save");

    function mqttDirty() {
      var m = cfg.mqtt;
      return mOn.input.checked !== !!m.on || mUrl.input.value.trim() !== (m.url || "") || mUser.input.value !== (m.user || "") ||
        mBase.input.value.trim() !== (m.base || "") || mPass.dirty();
    }

    function fillMqtt() {
      var m = cfg.mqtt;
      mOn.input.checked = !!m.on;
      mUrl.input.value = m.url || "";
      mUser.input.value = m.user || "";
      mBase.input.value = m.base || "";
      mPass.setSaved(m.password_set);
    }

    function saveMqtt() {
      var m = cfg.mqtt, p = {};
      if (mOn.input.checked !== !!m.on) { p.on = mOn.input.checked; }
      if (mUrl.input.value.trim() !== (m.url || "")) { p.url = mUrl.input.value.trim(); }
      if (mUser.input.value !== (m.user || "")) { p.user = mUser.input.value; }
      if (mBase.input.value.trim() !== (m.base || "")) { p.base = mBase.input.value.trim(); }
      if (mPass.cleared()) { p.password = ""; } else if (mPass.typed() !== "") { p.password = mPass.typed(); }
      if (!Object.keys(p).length) { HY.toast("Nothing to save"); return; }
      send({ mqtt: p }, { "mqtt.url": mUrl, "mqtt.user": mUser, "mqtt.password": mPass, "mqtt.base": mBase }, mSave, function () {
        HY.toast("MQTT settings saved");
      });
    }

    var mqttPanel = section("MQTT", [
      mqttStatus, mOn.box, mUrl.box, mUser.box, mPass.box, mBase.box,
      HY.h("div", { class: "hint" }, "Publishes the state of every medicine and accepts log, skip and snooze commands. It connects only while Wi-Fi is on for another reason."),
      HY.h("div", { class: "actions" }, mSave)]);

    // ---------------------------------------------------------------- Home Assistant

    var hOn = checkField("i-ha-on", "On", false);
    var hPrefix = textField("i-ha-prefix", "Discovery prefix", "", "homeassistant");
    var hSave = HY.h("button", { class: "btn p", type: "button", onclick: saveHa }, "Save");

    function haDirty() {
      return hOn.input.checked !== !!cfg.mqtt.ha || hPrefix.input.value.trim() !== (cfg.mqtt.ha_prefix || "");
    }

    function fillHa() {
      hOn.input.checked = !!cfg.mqtt.ha;
      hPrefix.input.value = cfg.mqtt.ha_prefix || "";
    }

    function saveHa() {
      var m = cfg.mqtt, p = {};
      if (hOn.input.checked !== !!m.ha) { p.ha = hOn.input.checked; }
      if (hPrefix.input.value.trim() !== (m.ha_prefix || "")) { p.ha_prefix = hPrefix.input.value.trim(); }
      if (!Object.keys(p).length) { HY.toast("Nothing to save"); return; }
      send({ mqtt: p }, { "mqtt.ha_prefix": hPrefix }, hSave, function () { HY.toast("Home Assistant settings saved"); });
    }

    var haPanel = section("Home Assistant discovery", [
      hOn.box, hPrefix.box,
      HY.h("div", { class: "hint" }, "Discovery sends over MQTT, so MQTT has to be on as well."),
      HY.h("div", { class: "actions" }, hSave)]);

    // ---------------------------------------------------------------- webhooks
    // A webhook keeps its slot number: pending events and the secret belong to the slot. Removing one leaves the
    // slot empty, so the webhooks after it keep their numbers.

    var slots = [];
    var hooksSig = JSON.stringify(cfg.webhooks);
    var hooksBox = HY.h("div");
    var removedNote = HY.h("p", { class: "hint", hidden: true }, "Removed webhooks are deleted when you save.");
    var addBtn = HY.h("button", { class: "btn", type: "button", onclick: addHook }, "Add webhook");
    var hSaveBtn = HY.h("button", { class: "btn p", type: "button", onclick: saveHooks }, "Save");
    var outbox = HY.h("p", { class: "mut sm", role: "status" });

    function makeSlot(i) {
      var s = { index: i, live: false, saved0: null, el: null, testing: false, testMsg: "", testAfter: 0, testTimer: null };
      s.saved = function () { return !!(s.saved0 && s.saved0.url); };
      s.dirty = function () {
        if (!s.live) { return s.saved(); }
        if (!s.saved()) { return true; }
        return s.url.input.value.trim() !== s.saved0.url || s.on.input.checked !== !!s.saved0.on ||
          changed(s.pickEvents(), s.saved0.events || []) || s.secret.dirty();
      };
      s.pickEvents = function () {
        return EVENTS.filter(function (e) { return s.ev[e[0]].input.checked; }).map(function (e) { return e[0]; });
      };
      return s;
    }

    function loadSlots() {
      var list = cfg.webhooks || [];
      var n = Math.max(list.length, slots.length);
      while (slots.length < n) { slots.push(makeSlot(slots.length)); }
      slots.forEach(function (s, i) {
        var h = list[i];
        clearTimeout(s.testTimer);
        s.testing = false;
        s.saved0 = h && h.url ? h : null;
        s.formerlySaved = null;
        s.live = !!s.saved0;
        s.fresh = false;
        s.el = null;   // rebuilt from the saved values
      });
    }

    function buildRow(s) {
      var n = s.index;
      var h = s.saved0 || { url: "", on: true, events: EVENTS.map(function (e) { return e[0]; }), secret_set: false };
      s.url = textField("i-hook-url-" + n, "Address", h.url, "https://n8n.lan/webhook/hygeia");
      s.on = checkField("i-hook-on-" + n, "On", h.on);
      s.ev = {};
      var evBoxes = EVENTS.map(function (e) {
        s.ev[e[0]] = checkField("i-hook-ev-" + n + "-" + e[0], e[1], (h.events || []).indexOf(e[0]) >= 0);
        return s.ev[e[0]].box;
      });
      s.evErr = HY.h("div", { class: "ferr", role: "alert" });
      s.secret = secretField("i-hook-secret-" + n, "Secret");
      s.secret.setSaved(h.secret_set);
      s.status = HY.h("p", { class: "mut sm", role: "status" });
      s.testOut = HY.h("p", { class: "mut sm", role: "status" });
      s.testBtn = HY.h("button", { class: "btn", type: "button", onclick: function () { runTest(s); } }, "Test");
      var rm = HY.h("button", { class: "btn danger", type: "button", onclick: function () { removeHook(s); } }, "Remove");
      var touch = function () { s.testBtn.disabled = s.testing || s.dirty() || !s.saved(); };
      s.url.input.addEventListener("input", touch);
      s.on.input.addEventListener("change", touch);
      s.secret.onChange(touch);
      EVENTS.forEach(function (e) { s.ev[e[0]].input.addEventListener("change", function () { touch(); invalid(null, "", s.evErr); }); });
      s.el = HY.h("div", { class: "hook" },
        HY.h("h3", null, "Webhook " + (n + 1)),
        s.status, s.url.box, s.on.box,
        HY.h("div", { class: "hint" }, "Events to send"),
        HY.h("div", { class: "evgrid" }, evBoxes), s.evErr,
        s.secret.box,
        HY.h("div", { class: "hint" }, "The secret signs each request in the X-Hygeia-Signature header."),
        HY.h("div", { class: "actions" }, s.testBtn, rm),
        s.testOut);
      return s.el;
    }

    function drawHooks() {
      HY.clear(hooksBox);
      var live = 0, removed = false;
      slots.forEach(function (s) {
        if (s.live) { hooksBox.appendChild(s.el || buildRow(s)); live++; }
        else {
          s.el = null;
          if (s.saved()) { removed = true; }
        }
      });
      if (!live) { hooksBox.appendChild(HY.h("p", { class: "mut" }, "No webhooks.")); }
      removedNote.hidden = !removed;
      addBtn.disabled = live >= MAX_HOOKS;
      statusAll();
    }

    function addHook() {
      var s = null, i;
      for (i = 0; i < MAX_HOOKS && !s; i++) {
        if (!slots[i]) { slots[i] = makeSlot(i); }
        if (!slots[i].live) { s = slots[i]; }
      }
      if (!s) { return; }
      s.live = true;
      s.el = null;
      s.fresh = true;   // a new webhook starts without a secret, whatever the slot held before
      var keep = s.saved0;
      s.saved0 = null;
      s.formerlySaved = keep;
      drawHooks();
      s.url.input.focus();
    }

    function removeHook(s) {
      if (s.formerlySaved && !s.saved0) { s.saved0 = s.formerlySaved; }
      s.live = false;
      s.el = null;
      s.fresh = false;
      drawHooks();
    }

    function hooksDirty() {
      return slots.some(function (s) { return s.dirty(); });
    }

    function saveHooks() {
      var last = -1;
      slots.forEach(function (s, i) { if (s.live) { last = i; } });
      var arr = [], bad = false;
      for (var i = 0; i <= last; i++) {
        var s = slots[i];
        if (!s || !s.live) { arr.push({ url: "", on: false }); continue; }
        var url = s.url.input.value.trim();
        if (!url) { invalid(s.url.input, "Enter the webhook address", s.url.err); bad = true; continue; }
        var item = { url: url, on: s.on.input.checked, events: s.pickEvents() };
        if (s.secret.cleared()) { item.secret = ""; }
        else if (s.secret.typed() !== "") { item.secret = s.secret.typed(); }
        else if (s.fresh || !s.saved()) { item.secret = ""; }
        arr.push(item);
      }
      if (bad) { return; }
      var map = {};
      slots.forEach(function (s, i) {
        if (!s.live) { return; }
        map["webhooks." + i + ".url"] = s.url;
        map["webhooks." + i + ".secret"] = s.secret;
        map["webhooks." + i + ".events"] = { input: null, err: s.evErr };
      });
      send({ webhooks: arr }, map, hSaveBtn, function () { HY.toast("Webhooks saved"); });
    }

    function runTest(s) {
      s.testing = true; s.testMsg = ""; s.testAfter = Infinity;
      statusAll();
      HY.api("POST", "/api/webhook/test", { index: s.index }).then(function () {
        if (!HY.alive(token)) { return; }
        s.testAfter = fetchId;
        clearTimeout(s.testTimer);
        s.testTimer = setTimeout(function () {
          if (!s.testing) { return; }
          s.testing = false; s.testMsg = "No result yet. Check the status above";
          statusAll();
        }, 15000);
        fetchStatus();
      }, function (e) {
        if (!HY.alive(token)) { return; }
        s.testing = false;
        s.testMsg = HY.errText(e);
        statusAll();
      });
    }

    var hooksPanel = section("Webhooks", [
      HY.h("p", { class: "hint" }, "Sends a signed message to each address when a dose is taken, skipped, missed or due, and for battery and clock events. Up to four."),
      hooksBox, removedNote,
      HY.h("div", { class: "actions" }, addBtn, hSaveBtn)]);

    // ---------------------------------------------------------------- saving

    // fields maps an API field name to { input, err } so a refusal marks the field that was refused
    function send(patch, fields, button, done) {
      var k;
      for (k in fields) { if (fields[k].err) { invalid(fields[k].input, "", fields[k].err); } }
      button.disabled = true;
      save(patch).then(function (s) {
        button.disabled = false;
        if (!HY.alive(token)) { return; }
        take(s, true);
        done();
      }, function (e) {
        button.disabled = false;
        if (!HY.alive(token)) { return; }
        var f = e && e.field && fields[e.field];
        var text = HY.errText(e);
        if (f) { invalid(f.input, text || "That value is not accepted", f.err); }
        else if (text) { HY.toast(text); }
      });
    }

    // Take the settings the device holds; a panel with unsaved edits keeps them unless force is set
    function take(s, force) {
      cfg = { mqtt: s.mqtt || cfg.mqtt, webhooks: s.webhooks || cfg.webhooks };
      if (force || !mqttDirty()) { fillMqtt(); }
      if (force || !haDirty()) { fillHa(); }
      var sig = JSON.stringify(cfg.webhooks);
      if (force || (!hooksDirty() && sig !== hooksSig)) { hooksSig = sig; loadSlots(); drawHooks(); }
    }

    // panels in page order: MQTT, Home Assistant discovery, Webhooks, with the shared outbox line
    root.appendChild(mqttPanel);
    root.appendChild(haPanel);
    root.appendChild(hooksPanel);
    root.appendChild(section("Event outbox", [outbox,
      HY.h("div", { class: "hint" }, "Events wait here until MQTT and each webhook have taken them. The counters are never reset.")]));
    fillMqtt(); fillHa(); loadSlots(); drawHooks();
    fetchStatus();

    return { sync: function (s) { take(s, false); } };
  };
})();
