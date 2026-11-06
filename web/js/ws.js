// Live updates: any event from the device, or a resync, refetches the current page's data. Firmware update events go to
// HY.onUpdate (set by the Firmware page) and cabinet test results and run state changes to HY.onCabinet (set by the Cabinet open page); neither refetches.
HY.ws = (function () {
  var sock = null, stable = null, timer = null, wait = null, tries = 0, running = false;
  var delays = [1000, 2000, 5000, 10000];

  function poke() {
    clearTimeout(wait);
    wait = setTimeout(function () { HY.refresh(); }, 300);
  }

  function retry() {
    if (!running) { return; }
    var d = delays[Math.min(tries, delays.length - 1)];
    tries++;
    clearTimeout(timer);
    timer = setTimeout(open, d);
  }

  function open() {
    if (!running) { return; }
    var url = (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws";
    try { sock = new WebSocket(url); } catch (e) { retry(); return; }
    sock.onopen = function () {
      clearTimeout(stable);
      stable = setTimeout(function () { tries = 0; }, 5000);   // a socket that drops at once keeps backing off
      poke();
    };
    sock.onmessage = function (e) {
      tries = 0;
      var msg = null;
      try { msg = JSON.parse(e.data); } catch (x) { msg = null; }
      if (msg && msg.ev === "cabinet") {   // a test result: the Cabinet open page shows it, nothing refetches
        if (typeof HY.onCabinet === "function") { HY.onCabinet(msg); }
        return;
      }
      if (msg && msg.ev === "update") {   // firmware update progress: no page refetch
        if (typeof HY.onUpdate === "function") { HY.onUpdate(msg); }
        return;
      }
      poke();
    };
    sock.onclose = function () { clearTimeout(stable); sock = null; retry(); };
    sock.onerror = function () { /* onclose follows */ };
  }

  return {
    start: function () { if (!running) { running = true; tries = 0; open(); } },
    stop: function () {
      running = false;
      clearTimeout(timer);
      if (sock) { sock.onclose = null; sock.close(); sock = null; }
    }
  };
})();
