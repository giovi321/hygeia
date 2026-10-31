// Clock help: corrects the device clock from this browser when it is more than 2 s off, and offers
// the browser's time zone when it differs from the device's. The zone never changes without a tap.
HY.timesync = (function () {
  var last = 0;

  function browserZone() {
    try { return Intl.DateTimeFormat().resolvedOptions().timeZone || ""; } catch (e) { return ""; }
  }

  function dismissKey(a, b) { return "hy.tz." + a + ">" + b; }

  function dismissed(a, b) {
    try { return localStorage.getItem(dismissKey(a, b)) === "1"; } catch (e) { return false; }
  }

  // Resolves true when the device clock was written.
  function run() {
    var now = Date.now();
    if (!HY.info || !HY.info.paired || now - last < 5000) { return Promise.resolve(false); }
    last = now;
    var t0 = performance.now(), w0 = Date.now();
    return HY.api("GET", "/api/info").then(function (info) {
      var rtt = performance.now() - t0;
      var offset = info.utc_ms - (w0 + rtt / 2);
      if (!info.clock_valid || Math.abs(offset) > 2000) {
        return HY.api("POST", "/api/time", { utc_ms: Math.round(Date.now() + rtt / 2), auto: true })
          .then(function () { return true; });
      }
      return false;
    }).catch(function () { return false; });
  }

  // A banner element, or null when zones match or the person dismissed this pair.
  function banner(deviceZone, onDone) {
    var mine = browserZone();
    if (!mine || !deviceZone || mine === deviceZone || dismissed(mine, deviceZone)) { return null; }
    var box;
    var use = HY.h("button", { class: "btn p", type: "button", onclick: function () {
      use.disabled = true;
      HY.api("POST", "/api/time", { utc_ms: Date.now(), zone: mine }).then(function () {
        HY.toast("Time zone is now " + mine);
        onDone();
      }, function (e) {
        use.disabled = false;
        var t = HY.errText(e);
        if (t) { HY.toast(t); }
      });
    } }, "Use " + mine);
    var no = HY.h("button", { class: "btn", type: "button", onclick: function () {
      try { localStorage.setItem(dismissKey(mine, deviceZone), "1"); } catch (e) { /* private mode */ }
      if (box.parentNode) { box.parentNode.removeChild(box); }
    } }, "Dismiss");
    box = HY.h("div", { class: "banner warn" },
      HY.h("p", null, "Your browser is in " + mine + ", the device in " + deviceZone),
      HY.h("div", { class: "actions" }, use, no));
    return box;
  }

  return { run: run, banner: banner, browserZone: browserZone };
})();
