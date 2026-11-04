// Cabinet open: when a movement is detected and a dose is overdue or due soon, the device chimes and/or lights the
// screen. Two triggers: any movement, or a movement taught on the device. The page sets the options, shows the taught
// state, runs the test with live counters and downloads the taught and recent samples.
(function () {
  var POLL_MS = 3000;
  var TEACHING_MSG = "The movement is being taught on the device";
  var CHECKING_MSG = "The movement is being tested on the device";

  function sensText(n) { return n + " of 5"; }
  function winText(m) { return m === 0 ? "Overdue only" : m + " min"; }

  function taughtText(c) {
    if (!c.taught) { return "Not taught"; }
    if (!c.taught_at) { return "Taught"; }
    var d = new Date(c.taught_at * 1000);
    return "Taught on " + d.toLocaleDateString("en-GB", { day: "numeric", month: "short", year: "numeric" }) + " at " +
      d.toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit", hour12: false });
  }

  HY.route("/settings/cabinet", function (params, view) {
    var token = HY.token;
    var set = null;       // the cabinet object of the settings
    var st = null;        // GET /api/cabinet
    var timer = null;
    var busy = false;

    view.appendChild(HY.h("div", { class: "head" }, HY.h("h1", null, "Cabinet open"),
      HY.h("a", { class: "btn", href: "#/settings" }, "Settings")));
    var errBox = HY.h("p", { class: "msg bad", role: "alert" });
    view.appendChild(errBox);
    HY.onLeave(function () {
      clearTimeout(timer);
      if (HY.onCabinet === onCabinet) { HY.onCabinet = null; }
    });

    function fail(e) { var t = HY.errText(e); if (t) { HY.toast(t); } }

    // ---- reminder options

    var on = HY.h("input", { id: "c-on", type: "checkbox", onchange: function () { put({ on: on.checked }); } });
    var rAny = HY.h("input", { id: "c-mode-any", type: "radio", name: "c-mode", value: "any",
      onchange: function () { if (rAny.checked) { put({ mode: "any" }); } } });
    var rTaught = HY.h("input", { id: "c-mode-taught", type: "radio", name: "c-mode", value: "taught",
      onchange: function () { if (rTaught.checked) { put({ mode: "taught" }); } } });
    var modeHint = HY.h("div", { class: "hint", id: "c-mode-hint" });
    var sens = HY.h("select", { id: "c-sens", onchange: function () { put({ sensitivity: parseInt(sens.value, 10) }); } });
    var i;
    for (i = 1; i <= 5; i++) { sens.appendChild(HY.h("option", { value: String(i) }, sensText(i))); }
    var win = HY.h("select", { id: "c-win", onchange: function () { put({ window_min: parseInt(win.value, 10) }); } });
    for (i = 0; i <= 120; i += 5) { win.appendChild(HY.h("option", { value: String(i) }, winText(i))); }
    var chime = HY.h("input", { id: "c-chime", type: "checkbox", onchange: function () { put({ chime: chime.checked }); } });
    var light = HY.h("input", { id: "c-light", type: "checkbox", onchange: function () { put({ light: light.checked }); } });

    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Reminder"),
      HY.h("div", null, HY.h("label", { class: "check", for: "c-on" }, on, "Cabinet open"),
        HY.h("div", { class: "hint" }, "When a movement is detected and a dose is overdue or due soon, the device reminds you. Otherwise it does nothing.")),
      HY.h("div", { class: "field", role: "radiogroup", "aria-labelledby": "c-trigger" },
        HY.h("div", { id: "c-trigger" }, "Trigger"),
        HY.h("label", { class: "check", for: "c-mode-any" }, rAny, "Any movement"),
        HY.h("label", { class: "check", for: "c-mode-taught" }, rTaught, "Taught movement"),
        modeHint),
      HY.h("div", { class: "field" }, HY.h("label", { for: "c-sens" }, "Sensitivity"), sens,
        HY.h("div", { class: "hint" }, "1 least sensitive, 5 most")),
      HY.h("div", { class: "field" }, HY.h("label", { for: "c-win" }, "Remind if due within"), win,
        HY.h("div", { class: "hint" }, "Never earlier than the medicine's own early window")),
      HY.h("label", { class: "check", for: "c-chime" }, chime, "Chime"),
      HY.h("label", { class: "check", for: "c-light" }, light, "Light the screen"),
      HY.h("div", { class: "hint" }, "Do not disturb and quiet hours silence the chime. The screen still lights.")));

    // ---- the movement

    var taughtLine = HY.h("p", { id: "c-taught" });
    var thrLine = HY.h("p", { class: "mut", id: "c-thr" });
    var pausedLine = HY.h("p", { class: "banner warn", id: "c-paused", hidden: true }, "Paused: the motion sensor did not answer. Change a setting to try again.");
    var countLine = HY.h("p", { class: "mut sm", id: "c-counts" });
    var forgetBtn = HY.h("button", { class: "btn danger", type: "button", onclick: forget }, "Forget the movement");
    var dl = HY.h("a", { class: "btn", id: "c-dl", href: "/api/cabinet/samples.csv", download: "cabinet-samples.csv" }, "Download samples");
    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Movement"),
      pausedLine, taughtLine, thrLine, countLine,
      HY.h("p", { class: "hint" }, "The movement is taught on the device: Settings, Cabinet open, Teach the movement."),
      HY.h("div", { class: "actions" }, dl, forgetBtn)));

    // ---- test

    var startBtn = HY.h("button", { class: "btn p", type: "button", id: "c-start", onclick: function () { runTest(true); } }, "Start");
    var stopBtn = HY.h("button", { class: "btn", type: "button", id: "c-stop", onclick: function () { runTest(false); } }, "Stop");
    var cA = HY.h("span", { id: "c-ta" }, "0");
    var cB = HY.h("span", { id: "c-tb" }, "0");
    var labA = HY.h("span");
    var labB = HY.h("span");
    var lastLine = HY.h("p", { class: "mut sm", id: "c-last" });
    var testMsg = HY.h("p", { class: "msg", role: "status", id: "c-test-msg" });
    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Test"),
      HY.h("p", { class: "mut" }, "Do the movement while the test runs. The device counts it and stays silent. The screen has to be on."),
      HY.h("div", { class: "actions" }, startBtn, stopBtn),
      HY.h("p", null, labA, " ", cA, HY.h("br"), labB, " ", cB), lastLine, testMsg));

    function showCounters(t) {
      var taught = st && st.mode === "taught";
      labA.textContent = taught ? "Matches:" : "Movements:";
      labB.textContent = taught ? "Other movements:" : "";
      labB.hidden = !taught; cB.hidden = !taught;
      cA.textContent = String(t ? t.matches : 0);
      cB.textContent = String(t ? t.others : 0);
    }

    function showLast(last) {
      if (!last) { lastLine.textContent = ""; return; }
      if (st && st.mode !== "taught") { lastLine.textContent = "Last: movement"; return; }
      var txt = last.match ? "Last: matched" : "Last: other movement";
      if (last.dist !== null && last.dist !== undefined && last.thr) {
        txt += ", distance " + last.dist + " (limit " + last.thr + ")";
      }
      lastLine.textContent = txt;
    }

    // ---- state to controls

    function show() {
      if (!set || !st) { return; }
      var a = document.activeElement;
      on.checked = !!set.on;
      rAny.checked = set.mode !== "taught";
      rTaught.checked = set.mode === "taught";
      rTaught.disabled = !st.taught;
      modeHint.textContent = st.taught ? "" : "Teach the movement on the device first";
      if (a !== sens) { sens.value = String(set.sensitivity); }
      if (a !== win) { win.value = String(set.window_min); }
      chime.checked = !!set.chime;
      light.checked = !!set.light;
      // switched on and set to act, but the device paused itself after a sensor fault
      pausedLine.hidden = !(set.on && (set.chime || set.light) && !st.live && (set.mode !== "taught" || st.taught));
      taughtLine.textContent = taughtText(st);
      thrLine.textContent = st.taught && st.threshold !== null ? "Threshold " + st.threshold : "";
      thrLine.hidden = !thrLine.textContent;
      var c = st.counts;
      countLine.textContent = "Since start: " + c.wom + " motion interrupts, " + c.captures + " captures, " + c.matches +
        " matches, " + c.others + " other, " + c.reactions + " reminders";
      startBtn.disabled = busy || st.testing || st.teaching || st.checking;
      stopBtn.disabled = busy || !st.testing;
      showCounters(st.test);
      if (st.teaching) { testMsg.textContent = TEACHING_MSG; }
      else if (st.checking) { testMsg.textContent = CHECKING_MSG; }
      else if (!st.testing && (testMsg.textContent === TEACHING_MSG || testMsg.textContent === CHECKING_MSG)) { testMsg.textContent = ""; }
      forgetBtn.disabled = !st.taught || st.teaching || st.checking;
      clearTimeout(timer);
      if (st.testing || st.teaching || st.checking) { timer = setTimeout(load, POLL_MS); }   // the test ends by itself after 10 minutes or when the screen goes off
    }

    // ---- actions

    function put(patch) {
      return HY.api("PUT", "/api/settings", { cabinet: patch }).then(function (s) {
        if (!HY.alive(token)) { return; }
        set = s.cabinet;
        return HY.api("GET", "/api/cabinet").then(function (c) { if (HY.alive(token)) { st = c; show(); } });
      }, function (e) {
        fail(e);
        if (HY.alive(token)) { load(); }
      });
    }

    function runTest(want) {
      busy = true; show();
      return HY.api("POST", "/api/cabinet/test", { on: want }).then(function (r) {
        busy = false;
        if (!HY.alive(token)) { return; }
        testMsg.textContent = "";
        if (want) { lastLine.textContent = ""; }
        return load().then(function () { if (want && r && r.test && st) { showCounters(r.test); } });
      }, function (e) {
        busy = false;
        if (!HY.alive(token)) { return; }
        testMsg.textContent = HY.errText(e) || "The test did not start";
        return load();
      });
    }

    function forget() {
      HY.confirm("Forget the movement?", "The taught movement is removed from the device. Taught movement does nothing until it is taught again.",
        true, "Forget").then(function (yes) {
        if (!yes) { return; }
        HY.api("DELETE", "/api/cabinet/teach").then(function () {
          if (HY.alive(token)) { HY.toast("Movement forgotten"); return load(); }
        }, function (e) { fail(e); if (HY.alive(token)) { load(); } });
      });
    }

    // A test result from the device
    function onCabinet(msg) {
      if (!HY.alive(token) || !st) { return; }
      st.test = msg.test;
      showCounters(msg.test);
      showLast(msg.last);
    }
    HY.onCabinet = onCabinet;

    function load() {
      return Promise.all([HY.api("GET", "/api/settings"), HY.api("GET", "/api/cabinet")]).then(function (r) {
        if (!HY.alive(token)) { return; }
        errBox.textContent = "";
        set = r[0].cabinet;
        st = r[1];
        show();
      }, function (e) {
        if (HY.alive(token)) { errBox.textContent = HY.errText(e) || "Could not load the settings"; }
      });
    }

    load();
    return function () {
      var a = document.activeElement;
      if (a && (a.tagName === "SELECT" || a.tagName === "INPUT") && view.contains(a)) { return; }
      return load();
    };
  }, "settings");
})();
