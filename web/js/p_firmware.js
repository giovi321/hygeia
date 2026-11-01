// Firmware: the running version, an update from GitHub and an upload of a new image. The device restarts after an install or a good upload.
HY.settingsLinks = HY.settingsLinks || [];
HY.settingsLinks.push({ href: "#/settings/firmware", text: "Firmware", note: "Version and update" });

(function () {
  var MAX_BYTES = 3 * 1024 * 1024;     // one firmware slot
  var POLL_MS = 2000;
  var POLL_LIMIT_MS = 60000;
  var ROLLBACK_S = 150;

  function mb(n) { return (n / 1048576).toFixed(1) + " MB"; }

  HY.route("/settings/firmware", function (params, view) {
    var token = HY.token;
    var file = null;
    var timer = null;
    var oldFw = "";
    var uploading = false;
    var repo = "";
    var ust = null;          // the last update status from the device
    var uTimer = null;
    var waiting = false;     // waitForDevice runs
    var uBusy = false;       // a check or install request is in flight

    view.appendChild(HY.h("div", { class: "head" }, HY.h("h1", null, "Firmware"),
      HY.h("a", { class: "btn", href: "#/settings" }, "Settings")));
    HY.onLeave(function () {
      clearTimeout(timer); clearTimeout(uTimer);
      if (HY.onUpdate === onUpdate) { HY.onUpdate = null; }
    });

    var cur = HY.h("p", null, "Reading the version");
    var pick = HY.h("input", { id: "f-file", type: "file", accept: ".bin,application/octet-stream" });
    var fileErr = HY.h("div", { class: "ferr", role: "alert" });
    var go = HY.h("button", { class: "btn p", type: "button", disabled: true, onclick: start }, "Upload and restart");
    var bar = HY.h("progress", { max: "100", value: "0", hidden: true, "aria-label": "Upload progress" });
    var status = HY.h("p", { class: "msg", role: "status" });

    var uMsg = HY.h("p", { class: "msg", role: "status" });
    var uCheck = HY.h("button", { class: "btn", type: "button", onclick: doCheck }, "Check for update");
    var uInstall = HY.h("button", { class: "btn p", type: "button", hidden: true, onclick: doInstall }, "Install");
    var uNotes = HY.h("a", { class: "btn", href: "#", target: "_blank", rel: "noopener", hidden: true }, "Release notes");
    var uBar = HY.h("progress", { max: "100", value: "0", hidden: true, "aria-label": "Download progress" });

    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Installed"), cur));
    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Update from GitHub"),
      HY.h("p", { class: "mut" }, "The device asks github.com for the latest release when you press the button, and downloads it only " +
        "when you press Install. It needs a Wi-Fi network with internet. Nothing is checked in the background. " +
        "On battery the install needs a charge of 30 % or more; on USB it always works."),
      HY.h("div", { class: "actions" }, uCheck, uInstall, uNotes), uBar, uMsg,
      HY.h("p", { class: "hint" }, "If the new firmware does not start properly, the device restarts into the previous version " +
        "after up to " + ROLLBACK_S + " seconds. Keep the device powered during the update.")));
    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Upload a file"),
      HY.h("p", { class: "mut" }, "Choose the firmware file hygeia.bin from a build. The device checks the file, restarts and starts it. " +
        "On battery the update needs a charge of 30 % or more; on USB it always works."),
      HY.h("div", { class: "field" }, HY.h("label", { for: "f-file" }, "Firmware file (.bin)"), pick, fileErr),
      HY.h("div", { class: "actions" }, go), bar, status,
      HY.h("p", { class: "hint" }, "If the new firmware does not start properly, the device restarts into the previous version " +
        "after up to " + ROLLBACK_S + " seconds. Keep the device powered during the update.")));

    function say(text, bad) { status.textContent = text; status.className = "msg" + (bad ? " bad" : ""); }

    function load() {
      return HY.api("GET", "/api/info").then(function (i) {
        if (!HY.alive(token)) { return; }
        oldFw = i.fw || "";
        repo = i.repo || "";
        cur.textContent = "Version " + (i.fw || "unknown") + (i.api ? " (API " + i.api + ")" : "");
      }, function (e) {
        if (HY.alive(token)) { cur.textContent = HY.errText(e) || "The device did not answer"; }
      });
    }

    pick.addEventListener("change", function () {
      fileErr.textContent = ""; say("");
      file = pick.files && pick.files[0] ? pick.files[0] : null;
      if (file && file.size === 0) { fileErr.textContent = "The file is empty"; file = null; }
      else if (file && file.size > MAX_BYTES) { fileErr.textContent = "The file is larger than the firmware slot (" + mb(MAX_BYTES) + ")"; file = null; }
      go.disabled = !file;
    });

    function lock(on) { uploading = on; go.disabled = on || !file || updBusy(); pick.disabled = on || updBusy(); render(); }

    // ---- update from GitHub ----
    function updBusy() {
      var s = ust ? ust.state : "idle";
      return uBusy || waiting || s === "checking" || s === "downloading" || s === "verifying" || s === "restarting";
    }
    function uSay(text, bad) { uMsg.textContent = text; uMsg.className = "msg" + (bad ? " bad" : ""); }

    function render() {
      var st = ust || { state: "idle" };
      var busy = updBusy();
      var avail = st.state === "available" && !busy;
      uCheck.disabled = busy || uploading;
      uInstall.hidden = !avail;
      uInstall.disabled = busy || uploading;
      uNotes.hidden = !avail || !repo;
      if (avail) {
        uInstall.textContent = "Install " + st.latest;
        if (repo) { uNotes.setAttribute("href", "https://github.com/" + repo + "/releases/tag/" + encodeURIComponent(st.latest)); }
      }
      uBar.hidden = !(st.state === "downloading" || st.state === "verifying");
      uBar.value = st.state === "verifying" ? 100 : (st.pct || 0);
      if (!uploading) { go.disabled = !file || busy; pick.disabled = busy; }
      if (waiting) { return; }
      switch (st.state) {
      case "checking": uSay("Checking GitHub"); break;
      case "up_to_date": uSay("Up to date. Version " + st.installed + " is the latest release"); break;
      case "available":
        uSay(st.installed_release ? "Version " + st.latest + " is available. Installed: " + st.installed :
          "Version " + st.latest + " is the latest release. Installed: " + st.installed +
          ", a development build, which may be newer than the release");
        break;
      case "downloading": uSay("Downloading " + (st.pct || 0) + " %"); break;
      case "verifying": uSay("Checking the firmware"); break;
      case "failed": uSay(st.message || "The update failed", true); break;
      case "restarting": break;
      default: if (!uBusy) { uSay(""); }
      }
      clearTimeout(uTimer);
      if (st.state === "checking" || st.state === "downloading" || st.state === "verifying") {
        uTimer = setTimeout(loadUpdate, 1000);
      }
    }

    function apply(st) {
      if (!HY.alive(token)) { return; }
      ust = st;
      if (st.installed) { oldFw = oldFw || st.installed; }
      if (st.state === "restarting" && !waiting) {
        waiting = true; oldFw = st.installed || oldFw;
        uBar.hidden = true;
        waitForDevice(uSay, uBar, true);
      }
      render();
    }

    function loadUpdate() {
      return HY.api("GET", "/api/update").then(apply, function (e) {
        if (!HY.alive(token)) { return; }
        var t = HY.errText(e);
        if (t && !updBusy()) { uSay(t, true); }
        if (ust && (ust.state === "checking" || ust.state === "downloading" || ust.state === "verifying")) {
          clearTimeout(uTimer); uTimer = setTimeout(loadUpdate, 1000);
        }
      });
    }

    // A live event: the state and the percent; the full status (latest, message) comes from the device when it settles
    function onUpdate(msg) {
      if (!HY.alive(token) || !msg) { return; }
      var st = msg.state;
      if (st === "checking" || st === "downloading" || st === "verifying" || st === "restarting") {
        var n = {}, k;
        for (k in (ust || {})) { n[k] = ust[k]; }
        n.state = st; n.pct = msg.pct || 0;
        apply(n);
        if (st === "restarting") { loadUpdate(); }
      } else {
        loadUpdate();
      }
    }
    HY.onUpdate = onUpdate;

    function doCheck() {
      if (updBusy() || uploading) { return; }
      uBusy = true; render(); uSay("Checking GitHub");
      HY.api("POST", "/api/update/check").then(function (st) {
        uBusy = false; apply(st);
      }, function (e) {
        if (!HY.alive(token)) { return; }
        uBusy = false;
        var t = HY.errText(e);
        loadUpdate().then(function () { if (t) { uSay(t, true); } });
      });
    }

    function doInstall() {
      if (updBusy() || uploading || !ust || ust.state !== "available") { return; }
      var latest = ust.latest;
      HY.confirm("Install " + latest + "?", "The device downloads the firmware from GitHub, checks it and restarts. " +
        "It cannot remind you for a short while. Keep it powered until it is back.", false, "Install").then(function (yes) {
        if (!yes || !HY.alive(token)) { return; }
        uBusy = true; render();
        HY.api("POST", "/api/update/install", { version: latest }).then(function (st) {
          uBusy = false; apply(st);
        }, function (e) {
          if (!HY.alive(token)) { return; }
          uBusy = false;
          var t = HY.errText(e);
          loadUpdate().then(function () { if (t) { uSay(t, true); } });
        });
      });
    }

    // After a good upload or an install: ask /api/info every 2 s until the device answers again, for up to 60 s.
    function waitForDevice(say, bar, viaUpdate) {
      var t0 = Date.now();
      var gone = false;
      var same = "";   // a version equal to the old one, seen before any failed poll
      say("The device restarts. This page reconnects when it is back");
      function again() {
        if (!HY.alive(token)) { return; }
        if (Date.now() - t0 >= POLL_LIMIT_MS) {
          lock(false); bar.hidden = true; waiting = false;
          if (viaUpdate) {
            ust = null; render();   // the buttons work again; the message below stays
            say(same ? "The device still reports version " + same + ", the version it had before. The restart did not happen: " +
              "check for the update again in a minute" :
              "The device did not answer within 60 seconds. If the new firmware fails, the device may come back on the previous " +
              "version after up to " + ROLLBACK_S + " seconds. Reload this page in a minute", true);
            return;
          }
          if (same && !oldFw) {   // the version before the update was not known
            say("The device still answers as version " + same + " and did not restart within 60 seconds. If that is the version you " +
              "uploaded, the update is done. Otherwise upload again", true);
            return;
          }
          if (same) {
            say("The device still reports version " + same + ", the version it had before. If you uploaded a file with the same version, " +
              "the update is done. Otherwise the restart did not happen: upload again", true);
            return;
          }
          say("The device did not answer within 60 seconds. If the new firmware fails, the device may come back on the previous version " +
            "after up to " + ROLLBACK_S + " seconds. Reload this page in a minute", true);
          return;
        }
        timer = setTimeout(tick, POLL_MS);
      }
      function tick() {
        if (!HY.alive(token)) { return; }
        var ctl = typeof AbortController === "function" ? new AbortController() : null;
        var cut = setTimeout(function () { if (ctl) { ctl.abort(); } }, 1500);
        fetch("/api/info", { cache: "no-store", credentials: "same-origin", signal: ctl ? ctl.signal : undefined })
          .then(function (r) { return r.ok ? r.json() : Promise.reject(new Error("http")); })
          .then(function (i) {
            clearTimeout(cut);
            if (!HY.alive(token)) { return; }
            if (!gone && (!oldFw || i.fw === oldFw)) { same = i.fw || ""; again(); return; }   // not restarted yet
            HY.info = i;
            lock(false); pick.value = ""; file = null; go.disabled = true;
            bar.hidden = true; waiting = false;
            cur.textContent = "Version " + i.fw + (i.api ? " (API " + i.api + ")" : "");
            if (viaUpdate) {
              var same2 = i.fw === oldFw;
              var back = same2 ? "The device is back on version " + i.fw + ", the same as before the update. The new firmware did " +
                "not start and the device returned to it" : "The device is back on version " + i.fw;
              oldFw = i.fw; ust = null;
              say(back, same2);
              loadUpdate().then(function () { if (HY.alive(token)) { say(back, same2); } });
              return;
            }
            if (i.fw === oldFw) {
              say("The device is back on version " + i.fw + ", the same as before the update. If the new file has the same version, " +
                "the update is done. Otherwise the new firmware did not start and the device returned to the previous version", true);
            } else {
              say("The device is back on version " + i.fw);
            }
            oldFw = i.fw;
          }, function () {
            clearTimeout(cut);
            gone = true;
            again();
          });
      }
      timer = setTimeout(tick, POLL_MS);
    }

    function start() {
      if (!file || uploading) { return; }
      HY.confirm("Update the firmware?", "The device restarts and cannot remind you for a short while. Keep it powered until it is back.",
        false, "Upload").then(function (yes) { if (yes && HY.alive(token)) { upload(); } });
    }

    function upload() {
      var xhr = new XMLHttpRequest();
      lock(true);
      bar.hidden = false; bar.value = 0;
      say("Uploading 0 %");
      xhr.open("POST", "/api/ota");
      xhr.setRequestHeader("X-Hygeia", "1");
      xhr.setRequestHeader("Content-Type", "application/octet-stream");
      xhr.timeout = 330000;
      xhr.upload.onprogress = function (e) {
        if (!HY.alive(token) || !e.lengthComputable) { return; }
        var pct = Math.min(100, Math.floor(e.loaded * 100 / e.total));
        bar.value = pct;
        say(pct >= 100 ? "Upload done. The device is checking the file" : "Uploading " + pct + " % (" + mb(e.loaded) + " of " + mb(e.total) + ")");
      };
      xhr.onload = function () {
        if (!HY.alive(token)) { return; }
        var data = null;
        try { data = JSON.parse(xhr.responseText); } catch (e) { data = null; }
        if (xhr.status === 200) { bar.value = 100; waitForDevice(say, bar, false); return; }
        lock(false); bar.hidden = true;
        if (xhr.status === 401) { if (HY.info) { HY.info.paired = false; } location.hash = "#/pair"; return; }
        say((data && data.message) || ("The device answered " + xhr.status), true);
      };
      xhr.onerror = xhr.ontimeout = function () {
        if (!HY.alive(token)) { return; }
        lock(false); bar.hidden = true;
        say("The connection to the device dropped during the upload. The device keeps its current firmware unless the update finished", true);
        load();
      };
      xhr.send(file);
    }

    load().then(loadUpdate);
  }, "settings");
})();
