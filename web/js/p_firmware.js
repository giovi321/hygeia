// Firmware: the running version and an upload of a new image. The device restarts after a good upload.
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

    view.appendChild(HY.h("div", { class: "head" }, HY.h("h1", null, "Firmware"),
      HY.h("a", { class: "btn", href: "#/settings" }, "Settings")));
    HY.onLeave(function () { clearTimeout(timer); });

    var cur = HY.h("p", null, "Reading the version");
    var pick = HY.h("input", { id: "f-file", type: "file", accept: ".bin,application/octet-stream" });
    var fileErr = HY.h("div", { class: "ferr", role: "alert" });
    var go = HY.h("button", { class: "btn p", type: "button", disabled: true, onclick: start }, "Upload and restart");
    var bar = HY.h("progress", { max: "100", value: "0", hidden: true, "aria-label": "Upload progress" });
    var status = HY.h("p", { class: "msg", role: "status" });

    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Installed"), cur));
    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Update"),
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

    function lock(on) { uploading = on; go.disabled = on || !file; pick.disabled = on; }

    // After a good upload: ask /api/info every 2 s until the device answers again, for up to 60 s.
    function waitForDevice() {
      var t0 = Date.now();
      var gone = false;
      var same = "";   // a version equal to the old one, seen before any failed poll
      say("The device restarts. This page reconnects when it is back");
      function again() {
        if (!HY.alive(token)) { return; }
        if (Date.now() - t0 >= POLL_LIMIT_MS) {
          lock(false); bar.hidden = true;
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
            bar.hidden = true;
            cur.textContent = "Version " + i.fw + (i.api ? " (API " + i.api + ")" : "");
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
        if (xhr.status === 200) { bar.value = 100; waitForDevice(); return; }
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

    load();
  }, "settings");
})();
